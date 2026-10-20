#!/usr/bin/env bash
set -euo pipefail

# 运行电脑端客户端。第一个位置参数可覆盖默认RTSP地址：
#   ./scripts/run_qt_client.sh rtsp://10.42.0.81:8554/live
SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$SOURCE_DIR/build-qt-client}
BINARY="$BUILD_DIR/qt_rtsp_client"
URL=${1:-rtsp://10.42.0.81:8554/live}

if [[ ! -x "$BINARY" ]]; then
    echo "错误：未找到$BINARY，请先执行./scripts/build_qt_client.sh。" >&2
    exit 2
fi

echo "[qt-run] binary=$BINARY"
echo "[qt-run] url=$URL"

# 用户可能在Conda环境中启动程序。清掉两个Qt插件路径变量，避免Conda插件与系统Qt6
# 动态库版本不一致；这不会退出或修改用户当前Conda环境。
exec env -u QT_PLUGIN_PATH -u QT_QPA_PLATFORM_PLUGIN_PATH "$BINARY" "$URL"
