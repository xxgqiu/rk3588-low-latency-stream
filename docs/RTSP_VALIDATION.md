# DMA-BUF + RGA 实时 RTSP 完整链路验证

## 目标与边界

本验证将持续编码实验中表现更好的 DMA-BUF + RGA 路径接入 Capture / Encode / Network 三线程 RTSP 管线，同时保留 `rtsp_stream` CPU-copy 可执行程序作为回退基线。

```text
OV8858 / RKISP
      ↓ V4L2 EXPBUF
DMA-BUF lease queue（容量 1，drop-oldest）
      ↓
RGA 同步布局整理
      ↓ 立即 QBUF 归还源 lease
MPP input DMA-BUF
      ↓ H.264
encoded packet queue（容量 8）
      ↓
FFmpeg RTSP publisher → MediaMTX → PC / VLC / FFmpeg
```

该路径仍包含一次 RGA 硬件拷贝，不称为纯零拷贝。

## 为什么 frame queue 只能为 1

当前 RKISP 分配 4 个 capture buffer。正常并发时：

- Encoder/RGA 短暂持有 1 个 source lease。
- Frame queue 最多持有 1 个待处理 lease。
- 至少保留 2 个 buffer 给 RKISP 继续采集。

如果把队列设为 2，慢编码压力下会出现：

```text
encoder 1 + queue 2 + 当前 DQBUF 1 = 4
```

所有相机 buffer 都被应用占用，驱动会产生 sequence 缺口。板端压力测试机械复现了该现象，因此优化路径对 4-buffer 相机强制 `frame_queue=1`，而不是简单照搬 CPU-copy 管线的队列容量。

## Lease 生命周期

每个成功 DQBUF 的 lease 必须且只能走向一种回收路径：

1. 正常路径：RGA 完成后立即 QBUF。
2. Queue 满：被 drop-oldest 淘汰后立即 QBUF。
3. Queue 已关闭：新帧 push 被拒绝，立即 QBUF。
4. 编码/网络异常：线程停止后 drain 队列，逐个 QBUF。

最终审计字段要求：

```text
captured == leases_released
outstanding_leases == 0
lease_release_failures == 0
lease_balance == true
```

V4L2 的 DQBUF 与 QBUF 可能分别由 Capture 和 Encode 线程调用，因此实现对同一 fd 的 ioctl 与 `queued_` 状态做了互斥保护。

## 正式 20 秒对照

共同条件：OV8858 NV12 1632×1224、H.264 High、4 Mbps、24 FPS、GOP 48、RTSP/TCP、MediaMTX，均由板端客户端读取。

| 指标 | CPU-copy RTSP | DMA-BUF + RGA RTSP | 变化 |
|---|---:|---:|---:|
| captured / encoded / published | 478 / 478 / 478 | 478 / 478 / 478 | 相同 |
| 实际发布速率 | 23.702 FPS | 23.751 FPS | +0.2% |
| CPU 使用率 | 13.938% | 5.600% | **-59.8%** |
| 峰值 RSS | 38,228 KiB | 26,712 KiB | **-30.1%** |
| CPU encode / RGA+MPP 平均耗时 | 6.675 ms | 1.222 + 4.565 = 5.787 ms | **-13.3%** |
| 内部管线平均耗时 | 9.361 ms | 6.962 ms | **-25.6%** |
| 内部管线最大耗时 | 15.581 ms | 14.745 ms | -5.4% |
| frame / packet drop | 0 / 0 | 0 / 0 | 相同 |
| 驱动 sequence 缺口 / timeout | 0 / 0 | 0 / 0 | 相同 |
| DMA lease 回收 | 不适用 | 478 / 478，平衡 | PASS |
| ffprobe / FFmpeg 解码 | PASS | PASS | 相同 |

FPS 几乎不变，因为两条路径都能跟上摄像头约 23.9 FPS 的真实输出。优化路径的主要收益是 CPU、RSS 和内部延迟。

## Drop-oldest 压力证明

仅用于诊断，将 RGA 后、MPP 前增加 80 ms 延迟，运行 8 秒：

```text
captured             = 191
encoded / published  = 93 / 93
frame drop-oldest    = 98
leases released      = 191
outstanding leases   = 0
driver sequence gaps = 0
capture timeouts     = 0
lease balance        = true
FFmpeg decode        = PASS
```

这证明处理速度下降时，管线会丢弃旧的未编码帧并保持最新画面，而不是积累延迟或泄漏相机 buffer。该延迟默认是 0，不参与正式性能成绩。

## 跨机器客户端

PC 在发布开始时通过局域网连接 `rtsp://10.42.0.81:8554/live`，使用 FFmpeg `-xerror` 连续解码 4 秒，无输出错误并以 0 退出。对应板端 10 秒运行完成 239/239/239 帧，lease 239/239 平衡。

如果客户端在流已经运行一段时间后从 P 帧中途加入，FFmpeg 可能在等到下一个 IDR 前报告一次临时宏块错误，随后恢复。当前 GOP 为 48（约 2 秒）；项目没有实现“客户端连接时立即请求 IDR”，因此不能把晚加入恢复时间宣称为 0。

## 编码包丢失后的恢复

Frame drop 发生在编码前，不会破坏 H.264 参考链。Packet queue 如果丢失已经编码的包，Network 线程仍沿用 M3 的 sequence-gap 检测：停止发送依赖缺失参考帧的 P 帧，等到下一个 IDR 才恢复。该逻辑由 `test_keyframe_resync` 离线覆盖；正式 20 秒运行没有自然发生 packet drop，不能伪称 live packet-drop 已触发。

## 运行

推荐的优化路径：

```bash
cd /home/cat/Projects/rk3588-low-latency-stream
./scripts/build_board.sh
./scripts/run_dmabuf_rtsp_stream.sh
```

CPU-copy 回退路径保持可用：

```bash
./scripts/run_rtsp_stream.sh
```

最终板端 `dmabuf_rtsp_stream` SHA256：

```text
65d5bb676a3cc523896d12364cf81afef3e751453eebe4fe5b57376b6d1625d5
```

该 binary 在最终队列策略单测提取后重新运行 8 秒，完成 191/191/191 帧、191/191 lease 平衡、驱动缺口与 timeout 均为 0。

精确的摄像头到显示器端到端延迟仍需光学时钟方法测量，不由内部软件时间戳替代。
