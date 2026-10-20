# 主/子码流设计与验证

## 目标

从同一个 OV8858 NV12 DMA-BUF 同时生成：

- 主码流：1632×1224，H.264，4 Mbps，`/live/main`；
- 子码流：640×480，H.264，800 kbps，`/live/sub`。

这不是让两个客户端读取同一条流，而是两套独立的 RGA 输出、MPP 编码器、
压缩包队列和 RTSP publisher。

## 数据与所有权

```text
Capture Thread
  DQBUF
    │
    ▼
Bounded lease queue (1)
    │
    ▼
Process Thread
  RGA main copy
  RGA sub resize
  QBUF source lease
  MPP main encode
  MPP sub encode
    │                │
    ▼                ▼
main packet queue   sub packet queue
    │                │
    ▼                ▼
main Network Thread sub Network Thread
```

只有两次同步 RGA 操作都完成后才归还相机 lease，保证 ISP 不会在 RGA 仍读取
源图像时覆盖该 slot。主、子 packet queue 容量各为 8，满时只丢本分支最旧包。

## 故障隔离

压缩包被丢弃后，后续 P 帧可能依赖缺失参考帧。每个 Network Thread 独立检测
`encoded_sequence` 缺口；出现缺口后跳过普通帧，等待下一张 IDR 再恢复。

测试命令：

```bash
SUB_NETWORK_DELAY_MS=80 DURATION=30 ./scripts/run_dual_stream.sh
```

实测慢子分支出现 15 次有界队列丢包，主分支仍为 717/717、23.762 FPS、
`packet_drop_oldest=0`。这证明网络慢分支不会反向阻塞主 publisher。

## 当前边界

- RGA 与两次 MPP 编码仍在一个 Process Thread 中顺序执行；
- 尚未完成 30 分钟以上稳定性测试；
- 未测试更多分辨率、更多分支或多客户端容量；
- 故障注入用于验证退化策略，不代表生产网络性能。
