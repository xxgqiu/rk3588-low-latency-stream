#!/usr/bin/env bash
set -euo pipefail

# 电脑端 Qt/FFmpeg 客户端统一构建入口。
# 项目仍然只使用 CMake；不生成 qmake .pro 文件。构建目录独立于板端
# /home/cat/Build/rk3588-low-latency-stream，也不会把 x86_64 产物误给 RK3588 使用。
SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$SOURCE_DIR/build-qt-client}
JOBS=${JOBS:-4}

if [[ $(uname -m) == "aarch64" ]]; then
    echo "错误：Qt客户端应在电脑端构建，不要在RK3588板端执行本脚本。" >&2
    exit 2
fi

# 与CMake中的find_package/pkg_check_modules保持一致。这里提前给出缺少哪一个依赖，
# 比配置结束后只看到“target不存在”更容易排查。
required_modules=(
    Qt6Core Qt6Gui Qt6Widgets
    libavformat libavcodec libavutil libswscale
)
for module in "${required_modules[@]}"; do
    if ! pkg-config --exists "$module"; then
        echo "错误：pkg-config找不到 $module，请先安装Qt6/FFmpeg开发包。" >&2
        exit 3
    fi
done

echo "[qt-build] source=$SOURCE_DIR"
echo "[qt-build] build=$BUILD_DIR"
echo "[qt-build] jobs=$JOBS"

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=ON
cmake --build "$BUILD_DIR" --parallel "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure

if [[ ! -x "$BUILD_DIR/qt_rtsp_client" ]]; then
    echo "错误：构建结束但未生成qt_rtsp_client。请检查CMake的Qt6/FFmpeg探测输出。" >&2
    exit 4
fi

echo "[qt-build] PASS binary=$BUILD_DIR/qt_rtsp_client"
