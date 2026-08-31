#!/usr/bin/env bash
set -euo pipefail

# 分层验证 MPP：先生成 Annex-B 文件，再用独立 FFmpeg 工具探测并全量解码。
# 码流只写入板端 Build 目录，不进入 NFS 源码或 Git。
BUILD_DIR=${BUILD_DIR:-/home/cat/Build/rk3588-low-latency-stream}
BINARY="$BUILD_DIR/mpp_file_encode"
OUTPUT=${OUTPUT:-$BUILD_DIR/ov8858_mpp.h264}
FRAMES=${FRAMES:-96}

if [[ ! -x "$BINARY" ]]; then
    echo "错误：未找到 $BINARY，请先执行 ./scripts/build_board.sh" >&2
    exit 2
fi

mkdir -p "$(dirname "$OUTPUT")"

echo "[mpp] output=$OUTPUT frames=$FRAMES"
"$BINARY" \
    --device /dev/video-camera0 \
    --output "$OUTPUT" \
    --frames "$FRAMES" \
    --fps 24 \
    --bitrate 4000000 \
    --gop 48

echo "[mpp] ffprobe"
ffprobe -v error -f h264 \
    -show_entries stream=codec_name,profile,width,height,pix_fmt,level \
    -show_entries format=size \
    -of default=noprint_wrappers=1 "$OUTPUT"

echo "[mpp] decode verification"
ffmpeg -v error -f h264 -i "$OUTPUT" -f null -
sha256sum "$OUTPUT"
