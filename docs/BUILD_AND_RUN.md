# 构建与运行指南

本文给出可复现的板端操作。推荐先阅读 README，了解项目边界和已验证硬件。

## 1. 环境要求

- RK3588 / aarch64 Linux 开发板；
- 可用的 V4L2 摄像头；
- CMake 3.16+、支持 C++17 的编译器、pkg-config；
- Rockchip MPP 开发包；
- librga 开发包；
- FFmpeg 运行时和 `libavformat/libavcodec/libavutil` 开发包；
- `v4l2-ctl`、`ffprobe`、`ffmpeg`、`curl`。

不同板卡镜像的包名和头文件位置可能不同。项目会明确打印缺失的可选依赖；不要因为 CMake 跳过 MPP/RGA target 就把主机测试当成硬件验证。

## 2. 源码与构建目录

源码可以位于 NFS 共享目录，但建议把编译中间文件放在开发板本地磁盘：

```text
source: /home/cat/Projects/rk3588-low-latency-stream
build:  /home/cat/Build/rk3588-low-latency-stream
```

这样不会把大量小文件写回 NFS，也不会污染 Git 源码目录。

## 3. 探测摄像头

```bash
./scripts/probe_cameras.sh
```

默认检查 `/dev/video-camera0` 和 `/dev/video21`。设备不同可覆盖：

```bash
MIPI_DEVICE=/dev/video11 USB_DEVICE=/dev/video2 ./scripts/probe_cameras.sh
```

## 4. 板端构建和测试

```bash
./scripts/build_board.sh
```

自定义构建目录和并行度：

```bash
BUILD_DIR=/home/cat/Build/rkstream-public JOBS=4 ./scripts/build_board.sh
```

脚本执行 fresh configure、真实链接和 CTest。成功输出会列出当前环境实际生成的硬件 target。

## 5. 只验证采集

```bash
# OV8858 MIPI NV12
CAMERA=mipi DURATION=10 ./scripts/run_capture_probe.sh

# USB YUYV
CAMERA=usb DURATION=10 ./scripts/run_capture_probe.sh

# USB MJPEG
CAMERA=usb-mjpeg DURATION=10 ./scripts/run_capture_probe.sh
```

该探针不保存画面，重点观察实际 FPS、driver sequence gap、timeout、队列高水位和 drop-oldest。

## 6. 文件硬件编码

CPU-copy baseline：

```bash
FRAMES=96 ./scripts/run_mpp_file_encode.sh
```

DMA-BUF 能力审计：

```bash
./scripts/run_dmabuf_probe.sh
```

两条编码路径 A/B 对照：

```bash
FRAMES=240 ./scripts/run_encode_benchmark.sh
```

输出写入板端 Build 目录的 `runtime/`，并由 ffprobe、FFmpeg 解码和 SHA256 检查。视频和码流不会进入 Git。

## 7. 安装 RTSP 服务端

```bash
./scripts/install_mediamtx.sh
```

脚本下载固定的 arm64 MediaMTX 版本并校验 SHA256。也可通过 `MEDIAMTX_ARCHIVE` 指向提前下载的压缩包，用于离线板端。

## 8. 启动 RTSP

推荐 DMA-BUF + RGA 路径：

```bash
DURATION=20 ./scripts/run_dmabuf_rtsp_stream.sh
```

CPU-copy 回退路径：

```bash
DURATION=20 ./scripts/run_rtsp_stream.sh
```

常用覆盖项：

```bash
BUILD_DIR=/home/cat/Build/rk3588-low-latency-stream \
DEVICE=/dev/video-camera0 \
PORT=8554 \
DURATION=60 \
./scripts/run_dmabuf_rtsp_stream.sh
```

脚本自动启动临时 MediaMTX、启动 C++ pipeline、等待 ready、运行客户端校验，并在退出时清理后台进程。

## 9. 客户端播放

将 `<BOARD_IP>` 替换为开发板局域网地址：

```bash
ffplay -rtsp_transport tcp -fflags nobuffer \
  rtsp://<BOARD_IP>:8554/live
```

VLC 中选择“打开网络串流”，输入同一地址。Chrome、Edge 等普通浏览器通常不能直接播放 RTSP，把地址输入搜索栏不会出现视频。

## 10. 主/子码流

同一 OV8858 输入同时发布主码流和子码流：

```bash
DURATION=20 ./scripts/run_dual_stream.sh
```

播放地址：

```text
rtsp://<BOARD_IP>:8554/live/main
rtsp://<BOARD_IP>:8554/live/sub
```

验证子分支变慢不会拖住主分支：

```bash
SUB_NETWORK_DELAY_MS=80 DURATION=30 ./scripts/run_dual_stream.sh
```

该参数只用于故障注入，不是生产配置。正常模式要求两条流都可解码且无队列
丢包；故障模式要求子分支出现受控丢包、主分支仍保持至少 22 FPS。

## 11. 电脑端 Qt 客户端

在装有 Qt6 与 FFmpeg 开发包的电脑上：

```bash
./scripts/build_qt_client.sh
./scripts/run_qt_client.sh rtsp://<BOARD_IP>:8554/live/main
```

客户端在后台线程完成 RTSP demux、H.264 解码和 RGB 转换，GUI 线程只显示
容量为 1 的 latest-frame mailbox，防止窗口卡顿时积累旧画面。拍照法端到端
延迟的操作步骤见 [Qt 客户端与拍照法](QT_E2E_LATENCY.md)。

## 12. 常见问题

### CMake 只生成采集 target

MPP、RGA 或 FFmpeg 开发包没有被发现。查看 configure 输出，不要把 `skipped` 误认为完整板端构建成功。

### 摄像头被占用

停止其他采集、编码或推流进程后重试。同一设备通常不能被两个进程同时独占采集。

### RTSP 地址能发布但浏览器无画面

使用 VLC 或 ffplay。RTSP 不是普通网页协议。

### NFS 源码构建很慢

保持源码在共享目录，通过 `BUILD_DIR` 把 CMake 输出放到 `/home/cat/Build`。

### 客户端晚加入时短暂花屏

客户端可能从 P 帧中途加入，需要等待下一个 IDR。当前 GOP 48、约 2 秒；项目尚未实现“新客户端连接时立即请求 IDR”。

### 如何确认资源没有泄漏

检查最终 summary：`lease_balance=true`、`outstanding_leases=0`、`lease_release_failures=0`，并确认脚本退出后没有残留 MediaMTX 或项目进程。
