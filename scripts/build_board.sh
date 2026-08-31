#!/usr/bin/env bash
set -euo pipefail

# 板端统一构建入口：源码可位于 NFS，但所有 CMake 中间文件和 binary 都写入
# /home/cat/Build 的板端存储。set -euo pipefail 保证任一配置/编译/测试失败即退出。
SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-/home/cat/Build/rk3588-low-latency-stream}
JOBS=${JOBS:-4}

if [[ $(uname -m) != "aarch64" ]]; then
    echo "错误：该脚本必须在 RK3588 开发板上执行。" >&2
    exit 2
fi

echo "[build] source=$SOURCE_DIR"
echo "[build] build=$BUILD_DIR"
echo "[build] jobs=$JOBS"

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=ON
cmake --build "$BUILD_DIR" --parallel "$JOBS"
# 实际运行注册的 test binary，不用“编译通过”代替行为验证。
(
    cd "$BUILD_DIR"
    ctest --output-on-failure
)

echo "[build] PASS binary=$BUILD_DIR/capture_probe"
if [[ -x "$BUILD_DIR/mpp_file_encode" ]]; then
    echo "[build] MPP binary=$BUILD_DIR/mpp_file_encode"
fi
if [[ -x "$BUILD_DIR/rtsp_stream" ]]; then
    echo "[build] RTSP binary=$BUILD_DIR/rtsp_stream"
fi
if [[ -x "$BUILD_DIR/dmabuf_probe" ]]; then
    echo "[build] DMA-BUF audit binary=$BUILD_DIR/dmabuf_probe"
fi
if [[ -x "$BUILD_DIR/dmabuf_mpp_encode" ]]; then
    echo "[build] DMA-BUF/RGA encoder=$BUILD_DIR/dmabuf_mpp_encode"
fi
if [[ -x "$BUILD_DIR/dmabuf_rtsp_stream" ]]; then
    echo "[build] DMA-BUF/RGA RTSP=$BUILD_DIR/dmabuf_rtsp_stream"
fi
