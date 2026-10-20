# 架构与设计说明

本文解释系统为何采用三线程、有界队列和显式 DMA-BUF ownership。若希望逐个查看类、函数和调用位置，请继续阅读 [类与函数源码参考](CODE_REFERENCE.md) 和 [程序走读](PROGRAM_WALKTHROUGH.md)。

## 1. 目标与边界

项目的目标是在 RK3588 上稳定完成以下实时链路：

```text
Camera → V4L2 → RGA → MPP H.264 → RTSP → Client
```

设计优先级依次为：

1. 资源生命周期正确，退出和异常路径不泄漏 buffer；
2. 延迟有界，消费者变慢时不无限积压旧画面；
3. 数据可观测，性能结论能够由日志与测试复现；
4. 保留 CPU-copy 基线，使优化收益可以公平比较。

## 2. 模块分层

| 模块 | 主要类型 | 职责 |
|---|---|---|
| 采集 | `V4L2Capture` | MMAP 采集、格式协商、DQBUF/QBUF、时间戳与 sequence |
| DMA 采集 | `V4L2DmabufCapture`、`DmabufFrameLease` | EXPBUF、lease 交接、跨线程安全归还 |
| 队列 | `BoundedLatestQueue<T>` | 固定容量、关闭唤醒、满时 drop-oldest |
| 布局转换 | `RgaNv12Aligner` | 同尺寸时整理 NV12 stride；目标尺寸不同时由 RGA 缩放 |
| 编码 | `MppH264Encoder` | MPP 初始化、header、逐帧编码、H.264 packet 输出 |
| 网络 | `RtspPublisher` | FFmpeg mux、时间基转换、RTSP/TCP 写入 |
| 协议辅助 | Annex-B 与 IDR helpers | NAL 类型识别、SPS/PPS/IDR 统计、丢包后恢复 |

CMake 会在普通 Linux 主机上构建平台无关部分和离线测试；只有检测到 MPP、RGA、FFmpeg 开发包时才启用板端硬件目标。

## 3. 三线程流水线

```text
Capture Thread                 Encode Thread                 Network Thread
──────────────                 ─────────────                 ──────────────
poll + DQBUF
创建 frame/lease
      │
      ▼
frame_queue(capacity=1) ─────► RGA layout copy
                               归还 camera lease
                               MPP encode
                                      │
                                      ▼
                              packet_queue(capacity=8) ─────► gap 检测
                                                             等待 IDR 恢复
                                                             RTSP write
```

采集、编码和网络不放在同一个循环里，因为任何一次编码抖动或网络阻塞都不应直接阻塞摄像头 DQBUF。两个队列都有固定上限，因此运行时间变长不会导致内存持续增长。

### 主/子码流分支

`dual_stream_rtsp` 从同一个相机 lease 生成两条真正不同的 H.264 码流：

```text
                         ┌─ RGA layout → MPP main → queue → /live/main
V4L2 DMA-BUF lease ─────┤
                         └─ RGA resize → MPP sub  → queue → /live/sub
```

Process Thread 先同步完成两次 RGA 读取，再立即 QBUF 归还源 lease。主、子
MPP 输入缓冲、压缩包队列和 Network Thread 相互独立，因此慢子分支不会阻塞
主分支发布。第一版仍在一个 Process Thread 内顺序执行两次 RGA/MPP，以换取
清晰、可验证的 DMA-BUF 生命周期。

## 4. 为什么使用 drop-oldest

实时预览关心的是“最新画面”，不是把每一帧都延迟数秒后送达。当队列已满时：

```text
旧帧排队 + 新帧到达 → 回收最旧帧 → 保留更新帧
```

对 DMA-BUF 路径，丢弃一个 `DmabufFrameLease` 不只是删除队列元素，还必须立即执行 QBUF，把相机 buffer 归还驱动。压力测试要求：

```text
captured == leases_released
outstanding_leases == 0
lease_balance == true
```

## 5. DMA-BUF ownership 状态机

```text
IN_DRIVER
   │ DQBUF
   ▼
IN_CAPTURE_THREAD
   ├─ push 成功 ─────► IN_FRAME_QUEUE ─────► IN_ENCODER
   ├─ queue 已关闭 ──► QBUF
   └─ 异常 ──────────► QBUF

IN_ENCODER
   │ RGA 同步读取完成
   ▼
QBUF → IN_DRIVER
```

`DmabufFrameLease` 是 move-only 对象，避免两个对象误以为自己同时拥有同一个 V4L2 buffer。所有退出路径都会关闭队列、唤醒线程并 drain 剩余 lease。

## 6. 为什么还需要 RGA

OV8858 当前有效图像为 `1632×1224` NV12；MPP 输入使用 `ver_stride=1232`：

```text
camera UV offset = 1632 × 1224
MPP UV offset    = 1632 × 1232
```

二者的 UV plane 起点不同。即使相机 DMA-BUF 的总容量足够，也不能直接把错误布局交给编码器。RGA 在 DMA-BUF 之间执行一次硬件布局拷贝，避免 CPU 逐行 `memcpy`。因此本项目准确称为“DMA-BUF + RGA 硬件拷贝”，不宣称纯零拷贝。

## 7. 编码包丢失与 IDR 重同步

编码前丢帧不会破坏 H.264 参考关系；编码后的 packet 如果在网络队列中丢失，后续 P 帧可能依赖缺失参考帧。Network Thread 检测 packet sequence 缺口后：

1. 暂停发送普通 P 帧；
2. 等待下一个 IDR；
3. 从可独立解码的 IDR 恢复发布。

该策略由 `test_keyframe_resync` 离线测试覆盖。

## 8. 正常停止顺序

```text
请求停止采集
  → Capture Thread 退出并关闭 frame queue
  → Encode Thread 消费/回收剩余 lease，关闭 packet queue
  → Network Thread 完成并退出
  → 销毁 publisher、encoder、RGA、V4L2 资源
```

先关闭生产端、再由队列唤醒下游，能够避免线程永久等待。脚本层还使用 trap 清理 MediaMTX 和客户端进程。

## 9. 两条可运行路径

| 路径 | 数据移动 | 用途 |
|---|---|---|
| CPU-copy | MMAP → `std::vector` → MPP buffer | 可读基线、兼容回退、A/B 对照 |
| DMA-BUF + RGA | EXPBUF → RGA → MPP DMA-BUF | 推荐路径，降低 CPU/RSS/内部耗时 |

优化路径没有删除基线，是为了让性能优化具备可证伪、可回归的参照物。

## 10. 仍未覆盖的能力

- 30 组以上 glass-to-glass 光学延迟统计（工具与方法已具备）；
- 客户端连接时主动请求 IDR；
- USB MJPEG 解码后接入 MPP；
- 4K、多客户端容量与长时间稳定性；
- 相机直接进入 MPP 的纯零拷贝契约。

这些限制会在 README 和性能报告中明确保留，不用内部软件耗时替代尚未测量的端到端指标。
