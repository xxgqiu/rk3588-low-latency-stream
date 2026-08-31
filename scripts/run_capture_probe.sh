#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-/home/cat/Build/rk3588-low-latency-stream}
BINARY="$BUILD_DIR/capture_probe"
CAMERA=${CAMERA:-mipi}
DURATION=${DURATION:-10}

if [[ ! -x "$BINARY" ]]; then
    echo "错误：未找到 $BINARY，请先执行 ./scripts/build_board.sh" >&2
    exit 2
fi

case "$CAMERA" in
    mipi)
        arguments=(
            --device /dev/video-camera0
            --format nv12
            --width 1632
            --height 1224
            --fps 30
        )
        ;;
    usb)
        arguments=(
            --device /dev/video21
            --format yuyv
            --width 640
            --height 480
            --fps 30
        )
        ;;
    usb-mjpeg)
        arguments=(
            --device /dev/video21
            --format mjpeg
            --width 1280
            --height 720
            --fps 30
        )
        ;;
    *)
        echo "错误：CAMERA 只能是 mipi、usb 或 usb-mjpeg。" >&2
        exit 2
        ;;
esac

echo "[probe] camera=$CAMERA duration=${DURATION}s"
exec "$BINARY" "${arguments[@]}" --duration "$DURATION" --queue-capacity 4 "$@"
