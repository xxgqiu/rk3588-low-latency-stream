#!/usr/bin/env bash
set -euo pipefail

# 同一摄像头、帧数、MPP 参数下依次运行 CPU-copy 与 DMA-BUF/RGA 文件编码，
# 分别解码验证后再比较指标。串行执行可避免两个进程争用摄像头/MPP。
BUILD_DIR=${BUILD_DIR:-/home/cat/Build/rk3588-low-latency-stream}
FRAMES=${FRAMES:-240}
DEVICE=${DEVICE:-/dev/video-camera0}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
RESULT_DIR=${RESULT_DIR:-$BUILD_DIR/runtime/encode_benchmark_$STAMP}
CPU_BINARY="$BUILD_DIR/mpp_file_encode"
DMA_BINARY="$BUILD_DIR/dmabuf_mpp_encode"
CPU_OUTPUT="$RESULT_DIR/cpu_copy.h264"
DMA_OUTPUT="$RESULT_DIR/dmabuf_rga.h264"
CPU_LOG="$RESULT_DIR/cpu_copy.log"
DMA_LOG="$RESULT_DIR/dmabuf_rga.log"

if [[ $(uname -m) != "aarch64" ]]; then
    echo "错误：该脚本必须在 RK3588 开发板上执行。" >&2
    exit 2
fi
for dependency in "$CPU_BINARY" "$DMA_BINARY" ffprobe ffmpeg; do
    if [[ "$dependency" == /* ]]; then
        [[ -x "$dependency" ]] || {
            echo "错误：缺少可执行文件 $dependency，请先运行 ./scripts/build_board.sh。" >&2
            exit 2
        }
    else
        command -v "$dependency" >/dev/null || {
            echo "错误：缺少命令 $dependency。" >&2
            exit 2
        }
    fi
done
if pgrep -x rtsp_stream >/dev/null || pgrep -x capture_probe >/dev/null ||
   pgrep -x mpp_file_encode >/dev/null || pgrep -x dmabuf_mpp_encode >/dev/null; then
    echo "错误：摄像头或 MPP 正被项目中的其他程序占用，请先停止已有运行。" >&2
    exit 2
fi

mkdir -p "$RESULT_DIR"

validate_stream() {
    local input=$1
    local label=$2
    echo "[benchmark] validating $label"
    ffprobe -v error -f h264 \
        -show_entries stream=codec_name,profile,width,height,pix_fmt \
        -of default=noprint_wrappers=1 "$input" >"$RESULT_DIR/${label}_ffprobe.txt"
    ffmpeg -v error -f h264 -i "$input" -f null -
    sha256sum "$input" | tee "$RESULT_DIR/${label}_sha256.txt"
}

extract_value() {
    local line=$1
    local key=$2
    awk -v wanted="$key" '{
        for (i = 1; i <= NF; ++i) {
            split($i, pair, "=")
            if (pair[1] == wanted) {
                print pair[2]
                exit
            }
        }
    }' <<<"$line"
}

echo "[benchmark] result_dir=$RESULT_DIR"
echo "[benchmark] frames_per_mode=$FRAMES device=$DEVICE"
echo "[benchmark] mode=CPU_COPY start"
"$CPU_BINARY" \
    --device "$DEVICE" --output "$CPU_OUTPUT" --frames "$FRAMES" \
    --fps 24 --bitrate 4000000 --gop 48 | tee "$CPU_LOG"
validate_stream "$CPU_OUTPUT" cpu_copy

echo "[benchmark] mode=DMABUF_RGA start"
"$DMA_BINARY" \
    --device "$DEVICE" --output "$DMA_OUTPUT" --frames "$FRAMES" \
    --fps 24 --bitrate 4000000 --gop 48 | tee "$DMA_LOG"
validate_stream "$DMA_OUTPUT" dmabuf_rga

CPU_SUMMARY=$(grep '^ENCODE_SUMMARY ' "$CPU_LOG" | tail -1)
DMA_SUMMARY=$(grep '^DMABUF_ENCODE_SUMMARY ' "$DMA_LOG" | tail -1)
[[ -n "$CPU_SUMMARY" && -n "$DMA_SUMMARY" ]] || {
    echo "错误：缺少 benchmark summary。" >&2
    exit 3
}

CPU_FPS=$(extract_value "$CPU_SUMMARY" pipeline_fps)
DMA_FPS=$(extract_value "$DMA_SUMMARY" pipeline_fps)
CPU_PERCENT=$(extract_value "$CPU_SUMMARY" cpu_percent)
DMA_PERCENT=$(extract_value "$DMA_SUMMARY" cpu_percent)
CPU_RSS=$(extract_value "$CPU_SUMMARY" max_rss_kib)
DMA_RSS=$(extract_value "$DMA_SUMMARY" max_rss_kib)
CPU_ENCODE_MS=$(extract_value "$CPU_SUMMARY" avg_encode_ms)
DMA_RGA_MPP_MS=$(extract_value "$DMA_SUMMARY" avg_rga_plus_mpp_ms)
DMA_RGA_MS=$(extract_value "$DMA_SUMMARY" avg_rga_ms)
DMA_MPP_MS=$(extract_value "$DMA_SUMMARY" avg_mpp_ms)

COMPARISON="ENCODE_COMPARISON result=PASS frames_per_mode=$FRAMES cpu_fps=$CPU_FPS dmabuf_fps=$DMA_FPS cpu_percent=$CPU_PERCENT dmabuf_percent=$DMA_PERCENT cpu_max_rss_kib=$CPU_RSS dmabuf_max_rss_kib=$DMA_RSS cpu_avg_encode_ms=$CPU_ENCODE_MS dmabuf_avg_rga_ms=$DMA_RGA_MS dmabuf_avg_mpp_ms=$DMA_MPP_MS dmabuf_avg_rga_plus_mpp_ms=$DMA_RGA_MPP_MS"
echo "$COMPARISON" | tee "$RESULT_DIR/comparison.txt"
echo "[benchmark] PASS artifacts=$RESULT_DIR"
