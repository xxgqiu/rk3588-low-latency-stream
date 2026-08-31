#!/usr/bin/env bash
set -euo pipefail

MIPI_DEVICE=${MIPI_DEVICE:-/dev/video-camera0}
USB_DEVICE=${USB_DEVICE:-/dev/video21}

command -v v4l2-ctl >/dev/null || {
    echo "错误：未安装 v4l2-ctl。" >&2
    exit 2
}

echo "=== V4L2 设备 ==="
v4l2-ctl --list-devices

for device in "$MIPI_DEVICE" "$USB_DEVICE"; do
    echo
    echo "=== $device 驱动信息 ==="
    v4l2-ctl -d "$device" -D
    echo "=== $device 支持格式 ==="
    v4l2-ctl -d "$device" --list-formats-ext
done
