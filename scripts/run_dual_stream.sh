#!/usr/bin/env bash
set -euo pipefail

# M6 主/子码流一键验证。默认是正常双路模式；设置
# SUB_NETWORK_DELAY_MS=80 时，它变为“慢子分支故障注入”测试。
# 脚本只在板端 Build/runtime 写日志，不保存摄像头画面。
BUILD_DIR=${BUILD_DIR:-/home/cat/Build/rk3588-low-latency-stream}
MEDIAMTX_VERSION=${MEDIAMTX_VERSION:-v1.20.1}
MEDIAMTX=${MEDIAMTX:-/home/cat/Build/tools/mediamtx-$MEDIAMTX_VERSION/mediamtx}
BINARY="$BUILD_DIR/dual_stream_rtsp"
DURATION=${DURATION:-20}
PORT=${PORT:-8554}
SUB_NETWORK_DELAY_MS=${SUB_NETWORK_DELAY_MS:-0}
MAIN_PATH=${MAIN_PATH:-live/main}
SUB_PATH=${SUB_PATH:-live/sub}
MAIN_URL="rtsp://127.0.0.1:$PORT/$MAIN_PATH"
SUB_URL="rtsp://127.0.0.1:$PORT/$SUB_PATH"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG_DIR=${LOG_DIR:-$BUILD_DIR/runtime/m6_dual_$STAMP}

if [[ ! -x "$BINARY" ]]; then
    echo "错误：未找到 $BINARY，请先执行 ./scripts/build_board.sh。" >&2
    exit 2
fi
if [[ ! -x "$MEDIAMTX" ]]; then
    echo "错误：未找到 MediaMTX: $MEDIAMTX" >&2
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
CONFIG="$LOG_DIR/mediamtx.yml"
MEDIAMTX_LOG="$LOG_DIR/mediamtx.log"
PIPELINE_LOG="$LOG_DIR/dual_stream.log"
MAIN_PROBE_LOG="$LOG_DIR/main_ffprobe.log"
SUB_PROBE_LOG="$LOG_DIR/sub_ffprobe.log"
MAIN_DECODE_LOG="$LOG_DIR/main_decode.log"
SUB_DECODE_LOG="$LOG_DIR/sub_decode.log"

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
  $MAIN_PATH:
    source: publisher
  $SUB_PATH:
    source: publisher
EOF

MEDIAMTX_PID=""
PIPELINE_PID=""
MAIN_PROBE_PID=""
SUB_PROBE_PID=""
MAIN_DECODE_PID=""
SUB_DECODE_PID=""
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    for pid in "$PIPELINE_PID" "$MAIN_PROBE_PID" "$SUB_PROBE_PID" \
               "$MAIN_DECODE_PID" "$SUB_DECODE_PID" "$MEDIAMTX_PID"; do
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    echo "[m6] logs=$LOG_DIR status=$status"
    exit "$status"
}
trap cleanup EXIT INT TERM

echo "[m6] starting MediaMTX port=$PORT"
(cd "$LOG_DIR" && exec "$MEDIAMTX" "$CONFIG") >"$MEDIAMTX_LOG" 2>&1 &
MEDIAMTX_PID=$!
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

echo "[m6] starting dual stream duration=${DURATION}s sub_delay=${SUB_NETWORK_DELAY_MS}ms"
stdbuf -oL -eL "$BINARY" \
    --device /dev/video-camera0 \
    --duration "$DURATION" \
    --fps 24 \
    --main-url "$MAIN_URL" \
    --main-bitrate 4000000 \
    --sub-url "$SUB_URL" \
    --sub-width 640 \
    --sub-height 480 \
    --sub-bitrate 800000 \
    --gop 48 \
    --frame-queue 1 \
    --packet-queue 8 \
    --sub-network-delay-ms "$SUB_NETWORK_DELAY_MS" \
    >"$PIPELINE_LOG" 2>&1 &
PIPELINE_PID=$!

for _ in $(seq 1 150); do
    if ! kill -0 "$PIPELINE_PID" 2>/dev/null; then
        echo "错误：双码流 pipeline 在 READY 前退出。" >&2
        tail -n 100 "$PIPELINE_LOG" >&2
        exit 8
    fi
    if grep -q '^DUAL_STREAM_READY' "$PIPELINE_LOG"; then
        break
    fi
    sleep 0.1
done
if ! grep -q '^DUAL_STREAM_READY' "$PIPELINE_LOG"; then
    echo "错误：双码流 pipeline 未进入 READY。" >&2
    exit 9
fi

BOARD_IP=$(hostname -I | awk '{print $1}')
echo "[m6] READY main=rtsp://$BOARD_IP:$PORT/$MAIN_PATH"
echo "[m6] READY sub =rtsp://$BOARD_IP:$PORT/$SUB_PATH"

ffprobe -v error -rtsp_transport tcp -read_intervals %+3 \
    -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,avg_frame_rate,time_base \
    -of default=noprint_wrappers=1 "$MAIN_URL" >"$MAIN_PROBE_LOG" 2>&1 &
MAIN_PROBE_PID=$!
ffprobe -v error -rtsp_transport tcp -read_intervals %+3 \
    -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,avg_frame_rate,time_base \
    -of default=noprint_wrappers=1 "$SUB_URL" >"$SUB_PROBE_LOG" 2>&1 &
SUB_PROBE_PID=$!
ffmpeg -v error -rtsp_transport tcp -i "$MAIN_URL" -t 5 -f null - \
    >"$MAIN_DECODE_LOG" 2>&1 &
MAIN_DECODE_PID=$!
ffmpeg -v error -rtsp_transport tcp -i "$SUB_URL" -t 5 -f null - \
    >"$SUB_DECODE_LOG" 2>&1 &
SUB_DECODE_PID=$!

set +e
wait "$PIPELINE_PID"; PIPELINE_STATUS=$?; PIPELINE_PID=""
wait "$MAIN_PROBE_PID"; MAIN_PROBE_STATUS=$?; MAIN_PROBE_PID=""
wait "$SUB_PROBE_PID"; SUB_PROBE_STATUS=$?; SUB_PROBE_PID=""
wait "$MAIN_DECODE_PID"; MAIN_DECODE_STATUS=$?; MAIN_DECODE_PID=""
wait "$SUB_DECODE_PID"; SUB_DECODE_STATUS=$?; SUB_DECODE_PID=""
set -e

echo "[m6] main stream"
cat "$MAIN_PROBE_LOG"
echo "[m6] sub stream"
cat "$SUB_PROBE_LOG"
MAIN_SUMMARY=$(grep '^BRANCH_SUMMARY name=main ' "$PIPELINE_LOG" | tail -1 || true)
SUB_SUMMARY=$(grep '^BRANCH_SUMMARY name=sub ' "$PIPELINE_LOG" | tail -1 || true)
TOTAL_SUMMARY=$(grep '^DUAL_STREAM_SUMMARY' "$PIPELINE_LOG" | tail -1 || true)
echo "$MAIN_SUMMARY"
echo "$SUB_SUMMARY"
echo "$TOTAL_SUMMARY"

if [[ "$PIPELINE_STATUS" -ne 0 || "$MAIN_PROBE_STATUS" -ne 0 || \
      "$SUB_PROBE_STATUS" -ne 0 || "$MAIN_DECODE_STATUS" -ne 0 ]]; then
    echo "错误：pipeline=$PIPELINE_STATUS main_probe=$MAIN_PROBE_STATUS sub_probe=$SUB_PROBE_STATUS main_decode=$MAIN_DECODE_STATUS sub_decode=$SUB_DECODE_STATUS" >&2
    exit 10
fi
if [[ "$TOTAL_SUMMARY" != *"result=PASS"* || \
      "$TOTAL_SUMMARY" != *"lease_balance=true"* ]]; then
    echo "错误：双码流 summary 未通过或 lease 不守恒。" >&2
    exit 11
fi
if ! grep -q '^width=1632$' "$MAIN_PROBE_LOG" ||
   ! grep -q '^height=1224$' "$MAIN_PROBE_LOG" ||
   ! grep -q '^width=640$' "$SUB_PROBE_LOG" ||
   ! grep -q '^height=480$' "$SUB_PROBE_LOG"; then
    echo "错误：ffprobe 显示的主/子分辨率与契约不符。" >&2
    exit 12
fi

MAIN_DROP=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^packet_drop_oldest=/){split($i,a,"="); print a[2]}}' <<<"$MAIN_SUMMARY")
SUB_DROP=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^packet_drop_oldest=/){split($i,a,"="); print a[2]}}' <<<"$SUB_SUMMARY")
MAIN_FPS=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^stream_fps=/){split($i,a,"="); print a[2]}}' <<<"$MAIN_SUMMARY")

if [[ "$SUB_NETWORK_DELAY_MS" -eq 0 ]]; then
    # 正常模式要求两条流都可完整解码。
    if [[ "$SUB_DECODE_STATUS" -ne 0 || "${MAIN_DROP:-}" != "0" ||
          "${SUB_DROP:-}" != "0" ]]; then
        echo "错误：正常双路模式出现解码失败或队列丢包：main_drop=${MAIN_DROP:-?} sub_drop=${SUB_DROP:-?}。" >&2
        tail -n 40 "$SUB_DECODE_LOG" >&2
        exit 13
    fi
    echo "[m6] PASS mode=dual_baseline"
else
    # 故障注入模式的目标不是让 sub 画面仍完美，而是机械证明：
    # sub 自己出现积压/丢包时，main 没有跟着丢队列包。
    if [[ "${MAIN_DROP:-}" != "0" || -z "${SUB_DROP:-}" || "$SUB_DROP" -le 0 ]]; then
        echo "错误：未观察到预期的分支隔离：main_drop=${MAIN_DROP:-?} sub_drop=${SUB_DROP:-?}" >&2
        exit 14
    fi
    # OV8858 实测源速率约 23.9 FPS。子分支被故意拖慢时，主分支仍应
    # 保持至少 22 FPS；否则只有“主队列没丢”，不足以证明吞吐未被拖慢。
    if [[ -z "${MAIN_FPS:-}" ]] ||
       ! awk -v fps="$MAIN_FPS" 'BEGIN { exit !(fps >= 22.0) }'; then
        echo "错误：慢子分支期间主码流吞吐不足：main_fps=${MAIN_FPS:-?}" >&2
        exit 15
    fi
    echo "[m6] PASS mode=sub_slow_fault_isolation main_drop=$MAIN_DROP sub_drop=$SUB_DROP"
fi

echo "[m6] playback main=rtsp://$BOARD_IP:$PORT/$MAIN_PATH"
echo "[m6] playback sub =rtsp://$BOARD_IP:$PORT/$SUB_PATH"
