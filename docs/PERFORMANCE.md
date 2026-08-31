# 性能与验证报告

本文汇总项目在实际 RK3588 开发板上的性能数据、测试口径和结论边界。所有结果都来自短时有界实验，不外推为未经测量的分辨率、客户端数量、功耗或长期稳定性结论。

## 1. 测试平台

| 项目 | 配置 |
|---|---|
| SoC | Rockchip RK3588，aarch64 |
| Kernel | Linux 5.10.160 |
| 主摄像头 | OV8858 MIPI，经 RKISP mainpath |
| 输入 | NV12，1632×1224，source stride 1632 |
| MPP 输入布局 | horizontal stride 1632，vertical stride 1232 |
| 编码 | H.264 High，CBR 4 Mbps，24 FPS，GOP 48 |
| 推流 | RTSP/TCP，FFmpeg publisher + MediaMTX |

OV8858 的 V4L2 协商模式为 30 FPS，但应用和直接 `v4l2-ctl` 均实测约 23.9 FPS。本文使用实测值，不把协商值写成实际吞吐。

## 2. 完整 RTSP A/B 对照

共同条件：同一开发板、摄像头、分辨率、编码参数和 RTSP 服务；两条路径分别运行约 20 秒，并由板端客户端真实读取。

| 指标 | CPU-copy RTSP | DMA-BUF + RGA RTSP | 变化 |
|---|---:|---:|---:|
| captured / encoded / published | 478 / 478 / 478 | 478 / 478 / 478 | 相同 |
| 实际发布速率 | 23.702 FPS | 23.751 FPS | +0.2% |
| 进程 CPU 使用率 | 13.938% | 5.600% | **-59.8%** |
| 峰值 RSS | 38,228 KiB | 26,712 KiB | **-30.1%** |
| CPU encode / RGA+MPP 平均耗时 | 6.675 ms | 1.222 + 4.565 = 5.787 ms | **-13.3%** |
| 内部管线平均耗时 | 9.361 ms | 6.962 ms | **-25.6%** |
| 内部管线最大耗时 | 15.581 ms | 14.745 ms | -5.4% |
| frame / packet drop | 0 / 0 | 0 / 0 | 相同 |
| driver sequence gap / timeout | 0 / 0 | 0 / 0 | 相同 |
| DMA lease 回收 | 不适用 | 478 / 478，平衡 | PASS |
| ffprobe / FFmpeg 完整解码 | PASS | PASS | 相同 |

### 结论

两条路径都能跟上摄像头真实输出，所以 FPS 基本相同。DMA-BUF + RGA 的收益主要是减少用户态原始帧拷贝，从而降低 CPU、用户态 RSS 和应用内部耗时。

## 3. 原始帧路径差异

CPU-copy 基线：

```text
RKISP MMAP buffer
  → copy 到约 3 MB Frame::data
  → 编码线程逐行复制到 MPP 对齐 buffer
  → MPP H.264
```

优化路径：

```text
RKISP EXPBUF
  → DMA-BUF fd
  → RGA hardware layout copy
  → MPP input DMA-BUF
  → MPP H.264
```

相机 NV12 的 UV 起点为 `1632 × 1224`，MPP 按对齐后的 `1632 × 1232` 解释 UV 起点，因此不能在保持当前 MPP 配置时直接编码相机 buffer。RGA 执行一次硬件布局拷贝，所以本项目称为“DMA-BUF + RGA”，不称为纯零拷贝。

详细布局证据见 [DMA-BUF 可行性审计](DMABUF_FEASIBILITY.md)。

## 4. 持续文件编码 A/B 对照

该实验不经过 RTSP，每条路径连续编码 240 帧，用于隔离网络和 FFmpeg muxer 开销。

| 指标 | CPU copy | DMA-BUF + RGA | 变化 |
|---|---:|---:|---:|
| 完成帧数 | 240 | 240 | 相同 |
| 管线速率 | 23.610 FPS | 23.820 FPS | +0.9% |
| CPU 使用率 | 13.995% | 4.956% | **-64.6%** |
| 峰值 RSS | 19,340 KiB | 5,292 KiB | **-72.6%** |
| 平均处理阶段 | 7.156 ms | 5.927 ms | **-17.2%** |
| RGA 平均耗时 | 不适用 | 1.236 ms | — |
| MPP 平均耗时 | 合并在基线耗时中 | 4.691 ms | — |
| driver gap / timeout | 0 / 0 | 0 / 0 | 相同 |
| SPS / PPS / IDR | 1 / 1 / 5 | 1 / 1 / 5 | 相同 |
| FFmpeg 完整解码 | PASS | PASS | 相同 |

文件编码与 RTSP 的 RSS 降幅不能直接混用：完整 RTSP 进程额外包含 FFmpeg、网络队列和发布上下文，原始 Frame vector 在总 RSS 中所占比例更小。

完整日志边界和码流 SHA256 见 [持续编码基准](ENCODING_BENCHMARK.md)。

## 5. 过载与有界性

为验证消费者变慢时的行为，在 RGA 完成并归还源 lease 后、MPP 编码前人为加入 80 ms 延迟，运行 8 秒：

| 指标 | 结果 |
|---|---:|
| captured | 191 |
| encoded / published | 93 / 93 |
| frame drop-oldest | 98 |
| leases released | 191 / 191 |
| outstanding leases | 0 |
| driver sequence gap | 0 |
| capture timeout | 0 |
| FFmpeg decode | PASS |

该结果证明：

- frame queue 达到容量后丢弃最旧未编码帧；
- 处理速度下降不会导致队列和内存无限增长；
- drop/reject/drain 路径都会归还 V4L2 lease；
- 相机仍保留足够 QBUF slots，驱动 sequence 没有出现缺口。

诊断延迟默认值为 0，不计入正式性能成绩。

## 6. 摄像头采集基线

| 摄像头/格式 | 模式 | 实测速率 |
|---|---|---:|
| OV8858 MIPI NV12 | 1632×1224 | 约 23.89 FPS |
| USB UVC YUYV | 640×480 | 约 19.96 FPS |
| USB UVC MJPEG | 1280×720 | 约 19.96 FPS |

相同速率也通过直接 `v4l2-ctl` 观察到，因此当前吞吐上限不是应用队列或 MPP 造成的。

## 7. MPP 编码基线

96 帧 CPU-copy 文件编码：

| 指标 | 结果 |
|---|---:|
| 管线速率 | 23.18 FPS |
| MPP 调用平均 / 最大耗时 | 6.74 / 10.10 ms |
| 实测输出码率 | 3.61 Mbps |
| SPS / PPS / IDR | 1 / 1 / 2 |
| 采集 timeout | 0 |
| ffprobe / 全量解码 | PASS / PASS |

## 8. 跨机器客户端验证

PC 通过局域网连接 `rtsp://10.42.0.81:8554/live`，使用 FFmpeg `-xerror` 连续解码 4 秒，无输出错误并以 0 退出。对应板端运行保持 frame/packet 无丢弃、lease 平衡。

客户端若在码流中途从 P 帧加入，可能先等待下一张 IDR。当前 GOP 48、约 24 FPS，最坏等待接近 2 秒；项目尚未实现客户端加入时主动请求 IDR。

## 9. 指标口径

### CPU

```text
CPU% = (进程 user CPU time + system CPU time) / wall time × 100%
```

数据来自 `getrusage(RUSAGE_SELF)`。100% 表示持续占用一个 CPU core，并非整颗 SoC 的功耗百分比。

### RSS

`ru_maxrss` 在板端单位为 KiB，表示进程用户态峰值常驻内存。它不包含内核驱动、完整 DMA-BUF 物理内存和 MediaMTX 进程。

### 内部管线耗时

```text
RTSP write 完成时间 - V4L2 DQBUF 时间
```

它包含应用内部排队、RGA/CPU copy、MPP 和 publisher write 调用，不包含摄像头曝光、ISP 前段、网络在途、服务端缓存、客户端解码和显示器刷新。

## 10. 结论边界

当前数据不能证明：

- 摄像头到屏幕的 glass-to-glass 延迟为 6.962 ms；
- 1080p30、4K 或任意其他未测试模式；
- 多客户端容量；
- 整板功耗改善；
- 1 小时以上稳定性；
- 纯零拷贝；
- USB MJPEG 已接入 MPP 主链路。

这些项目需要单独固定方法并重新采集数据，不能从当前短时实验外推。

## 11. 复现入口

```bash
# 板端构建与单元测试
./scripts/build_board.sh

# CPU/DMA 文件编码 A/B
FRAMES=240 ./scripts/run_encode_benchmark.sh

# 推荐 RTSP 路径
./scripts/run_dmabuf_rtsp_stream.sh

# CPU-copy 回退路径
./scripts/run_rtsp_stream.sh
```

运行日志、H.264 文件和临时配置只写入板端 Build/runtime 目录，并由 `.gitignore` 排除。
