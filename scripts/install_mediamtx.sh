#!/usr/bin/env bash
set -euo pipefail

VERSION=${MEDIAMTX_VERSION:-v1.20.1}
EXPECTED_SHA256=${MEDIAMTX_SHA256:-d1689f0bfefb1864e5ed3dcc8495eb2d7ec0a654f90bf3cd48980cb3bd08718a}
TOOLS_DIR=${TOOLS_DIR:-/home/cat/Build/tools/mediamtx-$VERSION}
ARCHIVE="mediamtx_${VERSION}_linux_arm64.tar.gz"
URL="https://github.com/bluenviron/mediamtx/releases/download/$VERSION/$ARCHIVE"
DOWNLOAD_DIR=${DOWNLOAD_DIR:-/home/cat/Build/downloads}
ARCHIVE_PATH=${MEDIAMTX_ARCHIVE:-$DOWNLOAD_DIR/$ARCHIVE}

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "错误：该脚本只安装 MediaMTX Linux arm64 版本" >&2
    exit 2
fi

if [[ -x "$TOOLS_DIR/mediamtx" ]]; then
    echo "[mediamtx] READY binary=$TOOLS_DIR/mediamtx"
    exit 0
fi

mkdir -p "$TOOLS_DIR" "$DOWNLOAD_DIR"
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT

if [[ ! -f "$ARCHIVE_PATH" ]]; then
    echo "[mediamtx] downloading $URL"
    curl --fail --location --retry 3 --output "$ARCHIVE_PATH" "$URL"
else
    echo "[mediamtx] using cache $ARCHIVE_PATH"
fi
echo "$EXPECTED_SHA256  $ARCHIVE_PATH" | sha256sum --check --status || {
    echo "错误：MediaMTX SHA256 校验失败" >&2
    exit 3
}

tar -xzf "$ARCHIVE_PATH" -C "$TEMP_DIR"
install -m 0755 "$TEMP_DIR/mediamtx" "$TOOLS_DIR/mediamtx"
if [[ -f "$TEMP_DIR/mediamtx.yml" ]]; then
    install -m 0644 "$TEMP_DIR/mediamtx.yml" "$TOOLS_DIR/mediamtx.example.yml"
fi
echo "[mediamtx] READY binary=$TOOLS_DIR/mediamtx version=$VERSION"
