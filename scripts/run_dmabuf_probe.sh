#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-/home/cat/Build/rk3588-low-latency-stream}
DEVICE=${DEVICE:-/dev/video-camera0}

if [[ $(uname -m) != "aarch64" ]]; then
    echo "错误：该脚本必须在 RK3588 开发板上执行。" >&2
    exit 2
fi
if [[ ! -x "$BUILD_DIR/dmabuf_probe" ]]; then
    echo "错误：未找到 $BUILD_DIR/dmabuf_probe，请先执行 $SOURCE_DIR/scripts/build_board.sh。" >&2
    exit 2
fi
if [[ ! -e "$DEVICE" ]]; then
    echo "错误：摄像头设备不存在：$DEVICE" >&2
    exit 2
fi

echo "[dmabuf] device=$DEVICE"
echo "[dmabuf] 只采集一帧用于 DMA-BUF/RGA 布局审计，不保存画面。"
"$BUILD_DIR/dmabuf_probe" --device "$DEVICE"
