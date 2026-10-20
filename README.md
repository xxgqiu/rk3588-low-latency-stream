# RK3588 低延迟硬件视频流系统

[![CI](https://github.com/xxgqiu/rk3588-low-latency-stream/actions/workflows/ci.yml/badge.svg)](https://github.com/xxgqiu/rk3588-low-latency-stream/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](CMakeLists.txt)
[![Platform](https://img.shields.io/badge/Platform-RK3588-brightgreen.svg)](docs/BUILD_AND_RUN.md)

基于 C++17 和 Embedded Linux 的实时视频工程：从 RK3588 上的 OV8858 MIPI 或 USB UVC 摄像头采集视频，使用 RGA 整理 NV12 内存布局、Rockchip MPP 硬件编码 H.264，并通过 RTSP 在局域网发布。

项目重点不只是“画面能播放”，还包括 V4L2 buffer ownership、有界实时队列、DMA-BUF lease、丢帧后的 IDR 重同步、可复现性能指标和完整错误清理路径。

## 核心特性

- 原生 V4L2 MMAP 采集，兼容 single-planar 和 multi-planar API
- MIPI OV8858 NV12 主路径，以及 USB UVC YUYV/MJPEG 采集探针
- Rockchip MPP H.264 High Profile 硬件编码
- V4L2 `VIDIOC_EXPBUF` + DMA-BUF + RGA 硬件布局转换
- Capture / Encode / Network 三线程流水线
- 两级有界 `drop-oldest` 队列，优先保证实时画面新鲜度
- H.264 包序号缺口检测，丢包后等待下一张 IDR 再恢复
- DMA-BUF lease 全生命周期对账，防止相机 buffer 泄漏
- FFmpeg RTSP publisher + MediaMTX 服务端
- 同一摄像头生成 1632×1224 主码流与 640×480 子码流，分支独立排队和发布
- Qt6 + FFmpeg 桌面客户端，用于实时预览、客户端指标和拍照法端到端延迟测量
- FPS、CPU、RSS、队列水位、阶段耗时和内部管线耗时统计
- 主机离线 CI 与 RK3588 板端实际构建/解码验证

## 系统架构

```text
OV8858 / RKISP
      │
      │ V4L2 DQBUF：应用取得 DMA-BUF lease
      ▼
Capture Thread
      │
      ▼
Bounded Lease Queue（容量 1，满时丢最旧并立即 QBUF）
      │
      ▼
Encode Thread
      ├─ RGA：1632×1224 source → 1632×1232 MPP layout
      ├─ V4L2 QBUF：RGA 完成后立即归还源 lease
      └─ MPP：NV12 → H.264 access unit
      │
      ▼
Bounded Packet Queue（容量 8）
      │
      ▼
Network Thread
      ├─ sequence gap / IDR resync
      └─ FFmpeg RTSP publisher → MediaMTX → VLC / ffplay
```

CPU-copy 基线作为可对照、可回退路径保留：

```text
V4L2 MMAP → Frame vector → CPU row copy → MPP → RTSP
```

完整线程职责和资源所有权见 [架构与设计说明](docs/ARCHITECTURE.md)。

## 板端实测

测试条件：RK3588、OV8858 NV12 1632×1224、H.264 High、CBR 4 Mbps、24 FPS、GOP 48、RTSP/TCP。两条路径分别运行约 20 秒并由真实客户端读取。

| 指标 | CPU-copy RTSP | DMA-BUF + RGA RTSP | 变化 |
|---|---:|---:|---:|
| captured / encoded / published | 478 / 478 / 478 | 478 / 478 / 478 | 相同 |
| 实际发布速率 | 23.702 FPS | 23.751 FPS | +0.2% |
| 进程 CPU 使用率 | 13.938% | 5.600% | **-59.8%** |
| 峰值 RSS | 38,228 KiB | 26,712 KiB | **-30.1%** |
| 平均内部管线耗时 | 9.361 ms | 6.962 ms | **-25.6%** |
| frame / packet drop | 0 / 0 | 0 / 0 | 相同 |
| driver gap / timeout | 0 / 0 | 0 / 0 | 相同 |
| DMA lease 回收 | 不适用 | 478 / 478 | PASS |
| ffprobe / FFmpeg 解码 | PASS | PASS | 相同 |

优化路径中 RGA 平均耗时为 1.222 ms，MPP 平均耗时为 4.565 ms。80 ms 慢编码压力下，系统主动丢弃 98 张旧帧，但 191/191 leases 全部归还，driver gap 和 timeout 均为 0。

FPS 基本不变，因为当前摄像头真实输出约 23.9 FPS，两条编码路径都能及时处理。优化收益主要体现在 CPU、内存和内部处理耗时。

详细方法、完整数据与限制见 [性能与验证报告](docs/PERFORMANCE.md)。

双码流 20 秒板端基线中，主、子分支均发布 478/478 帧（约 23.75 FPS），
两条 packet queue 均无丢包，摄像头 DMA lease 478/478 全部归还。对子分支
单独注入 80 ms 网络延迟时，子分支出现受控丢包并等待 IDR 恢复，主分支仍保持
约 23.76 FPS 且 `packet_drop_oldest=0`。详见
[主/子码流设计与验证](docs/DUAL_STREAM.md)。

## 已验证硬件

| 摄像头 | 设备节点 | 格式 | 实测采集速率 | 状态 |
|---|---|---|---:|---|
| OV8858 MIPI + RKISP | `/dev/video-camera0` | NV12 1632×1224 | 约 23.89 FPS | 完整编码/RTSP 主路径 |
| RYS DF100-720P USB UVC | `/dev/video21` | YUYV 640×480 | 约 19.96 FPS | 采集验证 |
| RYS DF100-720P USB UVC | `/dev/video21` | MJPEG 1280×720 | 约 19.96 FPS | 压缩帧采集验证 |

设备节点会随系统 media topology 变化，请先运行摄像头探测脚本。

## 快速开始

### 1. 获取源码

```bash
git clone https://github.com/xxgqiu/rk3588-low-latency-stream.git
cd rk3588-low-latency-stream
```

### 2. 在 RK3588 板端构建

```bash
./scripts/build_board.sh
```

脚本默认把构建产物放在：

```text
/home/cat/Build/rk3588-low-latency-stream
```

可通过 `BUILD_DIR` 和 `JOBS` 环境变量覆盖。脚本会实际运行 CTest，不用 syntax-only 代替测试。

### 3. 安装 MediaMTX

```bash
./scripts/install_mediamtx.sh
```

脚本下载固定版本的 arm64 MediaMTX，并校验 SHA256。第三方二进制安装到板端 Build 目录，不进入源码仓库。

### 4. 启动推荐 RTSP 路径

```bash
./scripts/run_dmabuf_rtsp_stream.sh
```

脚本会自动：

1. 启动临时 MediaMTX；
2. 启动 DMA-BUF/RGA/MPP C++ pipeline；
3. 等待 `DMABUF_STREAM_READY`；
4. 使用 ffprobe 和 FFmpeg 客户端验证码流；
5. 退出时清理全部后台进程；
6. 检查 `lease_balance=true`。

客户端播放地址：

```text
rtsp://<BOARD_IP>:8554/live
```

例如：

```bash
ffplay -rtsp_transport tcp -fflags nobuffer rtsp://10.42.0.81:8554/live
```

普通浏览器通常不能直接播放 `rtsp://`，请使用 VLC、ffplay 或其他 RTSP 客户端。

### CPU-copy 回退路径

```bash
./scripts/run_rtsp_stream.sh
```

### 主/子码流

```bash
./scripts/run_dual_stream.sh
```

### 电脑端 Qt 客户端

```bash
./scripts/build_qt_client.sh
./scripts/run_qt_client.sh rtsp://<BOARD_IP>:8554/live/main
```

更多依赖、摄像头检查、文件编码和故障排查见 [构建与运行指南](docs/BUILD_AND_RUN.md)。

## 目录结构

```text
apps/       可执行程序入口
include/    公共 C++ 接口与详细中文注释
src/        V4L2、MPP、RGA、RTSP 实现
tests/      不依赖摄像头的离线单元测试
scripts/    板端构建、探测、基准和一键 RTSP 脚本
docs/       架构、性能、学习与源码参考文档
.github/    CI、Issue 和 Pull Request 模板
```

## 文档

- [架构与设计说明](docs/ARCHITECTURE.md)
- [构建与运行指南](docs/BUILD_AND_RUN.md)
- [性能与验证报告](docs/PERFORMANCE.md)
- [DMA-BUF 可行性审计](docs/DMABUF_FEASIBILITY.md)
- [持续编码 A/B 基准](docs/ENCODING_BENCHMARK.md)
- [RTSP 完整链路验证](docs/RTSP_VALIDATION.md)
- [主/子码流设计与验证](docs/DUAL_STREAM.md)
- [Qt 客户端与拍照法端到端延迟](docs/QT_E2E_LATENCY.md)
- [源码学习导读](docs/LEARNING_GUIDE.md)
- [类与函数源码参考](docs/CODE_REFERENCE.md)
- [程序、线程、脚本与测试走读](docs/PROGRAM_WALKTHROUGH.md)
- [后续路线](docs/ROADMAP.md)

## 设计边界

- 当前不是纯零拷贝：相机有效高度 1224 与 MPP vertical stride 1232 不同，需要一次 RGA 硬件布局拷贝。
- `6.962 ms` 是 V4L2 DQBUF 到 RTSP write 返回的应用内部耗时，不是摄像头到显示器的 glass-to-glass 延迟。
- 当前没有声称 1080p30、4K、多客户端容量或整板功耗；这些项目尚未做正式 benchmark。
- USB 1280×720 MJPEG 已验证采集，但尚未接入 JPEG 解码→MPP 主链路。
- MediaMTX、FFmpeg、Rockchip MPP 和 librga 是外部依赖，不随本仓库源码再分发。

## 参与和安全

- 提交 Issue 或改动前请阅读 [贡献指南](CONTRIBUTING.md)。
- 安全问题请按 [安全策略](SECURITY.md) 私下报告，不要在公开 Issue 中披露敏感细节。
- 第三方组件说明见 [第三方依赖](THIRD_PARTY.md)。

## License

本项目采用 [MIT License](LICENSE)。外部依赖继续遵循各自许可证。
