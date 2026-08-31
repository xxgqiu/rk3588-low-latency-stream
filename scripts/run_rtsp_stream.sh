#!/usr/bin/env bash
set -euo pipefail

# CPU-copy 回退/对照路径的一键验证。整体编排与 DMA-BUF 脚本保持一致，使性能差异主要
# 来自原始帧传递方式，而不是不同的服务端、编码参数或测试时长。
BUILD_DIR=${BUILD_DIR:-/home/cat/Build/rk3588-low-latency-stream}
MEDIAMTX_VERSION=${MEDIAMTX_VERSION:-v1.20.1}
MEDIAMTX=${MEDIAMTX:-/home/cat/Build/tools/mediamtx-$MEDIAMTX_VERSION/mediamtx}
BINARY="$BUILD_DIR/rtsp_stream"
DURATION=${DURATION:-20}
PORT=${PORT:-8554}
PATH_NAME=${PATH_NAME:-live}
URL=${URL:-rtsp://127.0.0.1:$PORT/$PATH_NAME}
LOG_DIR=${LOG_DIR:-$BUILD_DIR/runtime}

if [[ ! -x "$BINARY" ]]; then
    echo "错误：未找到 $BINARY，请先执行 ./scripts/build_board.sh" >&2
    exit 2
fi
if [[ ! -x "$MEDIAMTX" ]]; then
    echo "错误：未找到 MediaMTX，请先执行 ./scripts/install_mediamtx.sh" >&2
    exit 3
fi
if ! command -v ffprobe >/dev/null || ! command -v ffmpeg >/dev/null; then
    echo "错误：板端需要 ffprobe 和 ffmpeg" >&2
    exit 4
fi
if ss -ltn "sport = :$PORT" | tail -n +2 | grep -q .; then
    echo "错误：端口 $PORT 已被占用" >&2
    exit 5
fi

mkdir -p "$LOG_DIR"
BINARY=$(realpath "$BINARY")
MEDIAMTX=$(realpath "$MEDIAMTX")
LOG_DIR=$(realpath "$LOG_DIR")
MEDIAMTX_LOG="$LOG_DIR/mediamtx.log"
STREAM_LOG="$LOG_DIR/rtsp_stream.log"
PROBE_LOG="$LOG_DIR/ffprobe.log"
CONFIG="$LOG_DIR/mediamtx.yml"
: >"$MEDIAMTX_LOG"
: >"$STREAM_LOG"
: >"$PROBE_LOG"

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
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [[ -n "$STREAM_PID" ]] && kill -0 "$STREAM_PID" 2>/dev/null; then
        kill -TERM "$STREAM_PID" 2>/dev/null || true
        wait "$STREAM_PID" 2>/dev/null || true
    fi
    if [[ -n "$PROBE_PID" ]] && kill -0 "$PROBE_PID" 2>/dev/null; then
        kill -TERM "$PROBE_PID" 2>/dev/null || true
        wait "$PROBE_PID" 2>/dev/null || true
    fi
    if [[ -n "$MEDIAMTX_PID" ]] && kill -0 "$MEDIAMTX_PID" 2>/dev/null; then
        kill -TERM "$MEDIAMTX_PID" 2>/dev/null || true
        wait "$MEDIAMTX_PID" 2>/dev/null || true
    fi
    echo "[rtsp] logs=$LOG_DIR status=$status"
    exit "$status"
}
trap cleanup EXIT INT TERM

echo "[rtsp] starting MediaMTX port=$PORT"
(
    cd "$LOG_DIR"
    exec "$MEDIAMTX" "$CONFIG"
) >"$MEDIAMTX_LOG" 2>&1 &
MEDIAMTX_PID=$!
for _ in $(seq 1 50); do
    if ! kill -0 "$MEDIAMTX_PID" 2>/dev/null; then
        echo "错误：MediaMTX 启动失败" >&2
        tail -n 40 "$MEDIAMTX_LOG" >&2
        exit 6
    fi
    if ss -ltn "sport = :$PORT" | tail -n +2 | grep -q .; then
        break
    fi
    sleep 0.1
done
if ! ss -ltn "sport = :$PORT" | tail -n +2 | grep -q .; then
    echo "错误：MediaMTX 未在 $PORT 监听" >&2
    exit 7
fi

echo "[rtsp] starting pipeline url=$URL duration=${DURATION}s"
stdbuf -oL -eL "$BINARY" \
    --device /dev/video-camera0 \
    --url "$URL" \
    --duration "$DURATION" \
    --fps 24 \
    --bitrate 4000000 \
    --gop 48 \
    --frame-queue 4 \
    --packet-queue 8 >"$STREAM_LOG" 2>&1 &
STREAM_PID=$!

for _ in $(seq 1 100); do
    if ! kill -0 "$STREAM_PID" 2>/dev/null; then
        echo "错误：推流管线在 READY 前退出" >&2
        tail -n 60 "$STREAM_LOG" >&2
        exit 8
    fi
    if grep -q '^STREAM_READY' "$STREAM_LOG"; then
        break
    fi
    sleep 0.1
done
if ! grep -q '^STREAM_READY' "$STREAM_LOG"; then
    echo "错误：推流管线未进入 READY" >&2
    exit 9
fi

BOARD_IP=$(hostname -I | awk '{print $1}')
echo "[rtsp] READY playback=rtsp://$BOARD_IP:$PORT/$PATH_NAME"
ffprobe -v error -rtsp_transport tcp -read_intervals %+3 \
    -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,avg_frame_rate,time_base \
    -of default=noprint_wrappers=1 "$URL" >"$PROBE_LOG" 2>&1 &
PROBE_PID=$!

set +e
wait "$STREAM_PID"
STREAM_STATUS=$?
STREAM_PID=""
wait "$PROBE_PID"
PROBE_STATUS=$?
PROBE_PID=""
set -e

cat "$PROBE_LOG"
grep '^STREAM_SUMMARY' "$STREAM_LOG" || true
if [[ "$STREAM_STATUS" -ne 0 || "$PROBE_STATUS" -ne 0 ]]; then
    echo "错误：pipeline=$STREAM_STATUS ffprobe=$PROBE_STATUS" >&2
    exit 10
fi
echo "[rtsp] PASS playback=rtsp://$BOARD_IP:$PORT/$PATH_NAME"
