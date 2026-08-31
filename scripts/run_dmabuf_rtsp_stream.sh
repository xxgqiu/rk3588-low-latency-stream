#!/usr/bin/env bash
set -euo pipefail

# 推荐的一键验证：自动启动临时 MediaMTX、启动 DMA-BUF/RGA C++ pipeline，
# 再用 ffprobe 和 ffmpeg 做真实客户端验证。所有日志写入板端 Build/runtime，退出时
# trap 会清理全部子进程，避免下次运行遇到端口占用或摄像头仍被占用。
BUILD_DIR=${BUILD_DIR:-/home/cat/Build/rk3588-low-latency-stream}
MEDIAMTX_VERSION=${MEDIAMTX_VERSION:-v1.20.1}
MEDIAMTX=${MEDIAMTX:-/home/cat/Build/tools/mediamtx-$MEDIAMTX_VERSION/mediamtx}
BINARY="$BUILD_DIR/dmabuf_rtsp_stream"
DURATION=${DURATION:-20}
PORT=${PORT:-8554}
PATH_NAME=${PATH_NAME:-live}
URL=${URL:-rtsp://127.0.0.1:$PORT/$PATH_NAME}
ENCODER_DELAY_MS=${ENCODER_DELAY_MS:-0}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG_DIR=${LOG_DIR:-$BUILD_DIR/runtime/m4c_$STAMP}

if [[ ! -x "$BINARY" ]]; then
    echo "错误：未找到 $BINARY，请先执行 ./scripts/build_board.sh。" >&2
    exit 2
fi
if [[ ! -x "$MEDIAMTX" ]]; then
    echo "错误：未找到 MediaMTX，请先执行 ./scripts/install_mediamtx.sh。" >&2
    exit 3
fi
if ! command -v ffprobe >/dev/null || ! command -v ffmpeg >/dev/null; then
    echo "错误：板端需要 ffprobe 和 ffmpeg。" >&2
    exit 4
fi
if ss -ltn "sport = :$PORT" | tail -n +2 | grep -q .; then
    echo "错误：端口 $PORT 已被占用。" >&2
    exit 5
fi

mkdir -p "$LOG_DIR"
BINARY=$(realpath "$BINARY")
MEDIAMTX=$(realpath "$MEDIAMTX")
LOG_DIR=$(realpath "$LOG_DIR")
MEDIAMTX_LOG="$LOG_DIR/mediamtx.log"
STREAM_LOG="$LOG_DIR/dmabuf_rtsp_stream.log"
PROBE_LOG="$LOG_DIR/ffprobe.log"
DECODE_LOG="$LOG_DIR/ffmpeg_decode.log"
CONFIG="$LOG_DIR/mediamtx.yml"
: >"$MEDIAMTX_LOG"
: >"$STREAM_LOG"
: >"$PROBE_LOG"
: >"$DECODE_LOG"

# 每次运行生成最小配置，只启用 RTSP/TCP，减少其他协议对测试的干扰。
cat >"$CONFIG" <<EOF
logLevel: info
rtsp: true
rtspAddress: :$PORT
rtspTransports: [tcp]
rtmp: false
hls: false
webrtc: false
srt: false
moq: false
paths:
  $PATH_NAME:
    source: publisher
EOF

MEDIAMTX_PID=""
STREAM_PID=""
PROBE_PID=""
DECODE_PID=""
cleanup() {
    # 保存主流程的真实退出码；清理中的 kill/wait 失败不能覆盖原始根因状态。
    local status=$?
    trap - EXIT INT TERM
    for pid in "$STREAM_PID" "$PROBE_PID" "$DECODE_PID" "$MEDIAMTX_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    echo "[m4c] logs=$LOG_DIR status=$status"
    exit "$status"
}
trap cleanup EXIT INT TERM

echo "[m4c] starting MediaMTX port=$PORT"
(
    cd "$LOG_DIR"
    exec "$MEDIAMTX" "$CONFIG"
) >"$MEDIAMTX_LOG" 2>&1 &
MEDIAMTX_PID=$!
# READY 检查分两层：进程仍存活 + TCP 端口实际进入 LISTEN。
for _ in $(seq 1 50); do
    if ! kill -0 "$MEDIAMTX_PID" 2>/dev/null; then
        echo "错误：MediaMTX 启动失败。" >&2
        tail -n 40 "$MEDIAMTX_LOG" >&2
        exit 6
    fi
    if ss -ltn "sport = :$PORT" | tail -n +2 | grep -q .; then
        break
    fi
    sleep 0.1
done
if ! ss -ltn "sport = :$PORT" | tail -n +2 | grep -q .; then
    echo "错误：MediaMTX 未在 $PORT 监听。" >&2
    exit 7
fi

echo "[m4c] starting DMA-BUF/RGA pipeline duration=${DURATION}s delay=${ENCODER_DELAY_MS}ms"
stdbuf -oL -eL "$BINARY" \
    --device /dev/video-camera0 \
    --url "$URL" \
    --duration "$DURATION" \
    --fps 24 \
    --bitrate 4000000 \
    --gop 48 \
    --frame-queue 1 \
    --packet-queue 8 \
    --encoder-delay-ms "$ENCODER_DELAY_MS" >"$STREAM_LOG" 2>&1 &
STREAM_PID=$!

# C++ 程序完成 V4L2/MPP/RGA/RTSP 初始化后才打印 DMABUF_STREAM_READY。
for _ in $(seq 1 100); do
    if ! kill -0 "$STREAM_PID" 2>/dev/null; then
        echo "错误：DMA-BUF 推流管线在 READY 前退出。" >&2
        tail -n 80 "$STREAM_LOG" >&2
        exit 8
    fi
    if grep -q '^DMABUF_STREAM_READY' "$STREAM_LOG"; then
        break
    fi
    sleep 0.1
done
if ! grep -q '^DMABUF_STREAM_READY' "$STREAM_LOG"; then
    echo "错误：DMA-BUF 推流管线未进入 READY。" >&2
    exit 9
fi

BOARD_IP=$(hostname -I | awk '{print $1}')
echo "[m4c] READY playback=rtsp://$BOARD_IP:$PORT/$PATH_NAME"
# 同时运行结构探测与 5 秒全量解码；任一失败都不能报告整体验证 PASS。
ffprobe -v error -rtsp_transport tcp -read_intervals %+3 \
    -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,avg_frame_rate,time_base \
    -of default=noprint_wrappers=1 "$URL" >"$PROBE_LOG" 2>&1 &
PROBE_PID=$!
ffmpeg -v error -rtsp_transport tcp -i "$URL" -t 5 -f null - >"$DECODE_LOG" 2>&1 &
DECODE_PID=$!

set +e
wait "$STREAM_PID"
STREAM_STATUS=$?
STREAM_PID=""
wait "$PROBE_PID"
PROBE_STATUS=$?
PROBE_PID=""
wait "$DECODE_PID"
DECODE_STATUS=$?
DECODE_PID=""
set -e

cat "$PROBE_LOG"
SUMMARY=$(grep '^DMABUF_STREAM_SUMMARY' "$STREAM_LOG" | tail -1 || true)
echo "$SUMMARY"
if [[ "$STREAM_STATUS" -ne 0 || "$PROBE_STATUS" -ne 0 || "$DECODE_STATUS" -ne 0 ]]; then
    echo "错误：pipeline=$STREAM_STATUS ffprobe=$PROBE_STATUS decode=$DECODE_STATUS" >&2
    tail -n 40 "$DECODE_LOG" >&2
    exit 10
fi
if [[ "$SUMMARY" != *"result=PASS"* || "$SUMMARY" != *"lease_balance=true"* ]]; then
    echo "错误：DMA-BUF summary 未通过或 lease 不平衡。" >&2
    exit 11
fi
echo "[m4c] PASS playback=rtsp://$BOARD_IP:$PORT/$PATH_NAME"
