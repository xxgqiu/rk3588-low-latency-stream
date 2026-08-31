# DMA-BUF + RGA 持续编码性能对照

## 目标

本实验把一帧 DMA-BUF 可行性证明扩展为持续硬件编码，并与已经验证的 CPU-copy baseline 做相同参数、相同帧数的板端对照。此处先隔离文件编码变量，完整 RTSP 结果在另一份报告中验证。

两条路径分别为：

```text
CPU_COPY
RKISP MMAP → CPU vector copy → CPU row copy/alignment → MPP → H.264

DMABUF_RGA
RKISP EXPBUF → RGA hardware layout copy → MPP input DMA-BUF → H.264
```

`DMABUF_RGA` 中仍发生一次 RGA 硬件拷贝，只是 CPU 不再搬运整帧，所以不称为纯零拷贝。

## Buffer ownership

DMA-BUF 路径采用显式 frame lease：

1. `VIDIOC_DQBUF` 后，该相机 buffer 暂时归应用所有。
2. RGA 以同步模式读取相机 DMA-BUF，并写入独立的 MPP 输入 DMA-BUF。
3. RGA 完成后立即 `VIDIOC_QBUF`，把相机 buffer 归还 RKISP。
4. MPP 只读取自己的输入 DMA-BUF，不再依赖已归还的相机 buffer。

这样不会在 RGA 尚未读完时让 RKISP 覆盖源帧，也不会在 MPP 编码期间长期占用相机 buffer。当前实验使用 4 个固定导出 buffer 和 1 个固定 MPP 输入 buffer，资源数量不随运行时间增长。

## 正式板端对照

执行时间：2026-08-24。每种模式连续编码 240 帧；共同配置为 OV8858 NV12 1632×1224、H.264 High、CBR 4 Mbps、24 FPS、GOP 48。

| 指标 | CPU copy | DMA-BUF + RGA | 变化 |
|---|---:|---:|---:|
| 完成帧数 | 240 | 240 | 相同 |
| 实际管线速率 | 23.610 FPS | 23.820 FPS | +0.9% |
| CPU 使用率 | 13.995% | 4.956% | **-64.6%** |
| 峰值 RSS | 19,340 KiB | 5,292 KiB | **-72.6%** |
| CPU copy/encode 阶段平均耗时 | 7.156 ms | — | — |
| RGA 平均耗时 | — | 1.236 ms | — |
| MPP 平均耗时 | — | 4.691 ms | — |
| RGA + MPP 平均耗时 | — | 5.927 ms | **-17.2%** |
| 驱动 sequence 缺口 | 0 | 0 | 相同 |
| 采集 timeout | 0 | 0 | 相同 |
| SPS / PPS / IDR | 1 / 1 / 5 | 1 / 1 / 5 | 相同 |
| FFmpeg 完整解码 | PASS | PASS | 相同 |

输出码流 SHA256：

```text
CPU copy:
7ece28aaa98a7dfa80f8c8b934c24a9cc7e7e8adb7769a3ee2e92b429bb24162

DMA-BUF + RGA:
fa1a6ae7b23bcbc8e5b552ac1c7a778c1409c4a0f98a9e89663efab920566fda
```

FPS 几乎不变是合理结果：两条路径都远快于 OV8858 当前约 23.9 FPS 的实际出帧速率，吞吐量由摄像头决定。优化的主要收益是 CPU、内存和每帧内部处理时间，而不是把摄像头变成更高 FPS。

## 公平性边界

- 两种模式使用相同开发板、摄像头节点、分辨率、编码参数和 240 帧数量。
- 两段运行按顺序执行，真实场景内容和瞬时温度不可能完全相同；码率和极值不适合作为绝对等价对照。
- CPU 使用率按进程 user+system CPU time / wall time 计算。
- `CPU avg_encode_ms` 包含把 vector 数据逐行写入 MPP buffer 和 MPP 调用；DMA 路径分别测 RGA 与 MPP。
- 峰值 RSS 是各独立进程的 `ru_maxrss`，不包含内核 DMA-BUF/驱动内存。
- 两条 `.h264` 都经过 `ffprobe` 和 FFmpeg 全量解码，但本测试不评价主观画质。
- 这些结果不能代替摄像头到显示器的光学端到端延迟测量。

## RTSP 回归

编码器内部重构后，原 CPU-copy RTSP 路径额外运行 10 秒回归：

```text
captured / encoded / published = 239 / 239 / 239
stream_fps = 23.615
frame / packet drop = 0 / 0
driver gaps / timeout = 0 / 0
ffprobe = PASS
```

这说明编码器内部重构没有破坏 CPU-copy RTSP baseline。随后完成的 DMA-BUF + RGA 三线程接入、ownership 与 drop-oldest 验证见 [RTSP 完整链路验证](RTSP_VALIDATION.md)。

## 复现

```bash
cd /home/cat/Projects/rk3588-low-latency-stream
./scripts/build_board.sh
FRAMES=240 ./scripts/run_encode_benchmark.sh
```

码流、日志和 SHA256 只写入板端 `/home/cat/Build/rk3588-low-latency-stream/runtime`，不会进入 Git。
