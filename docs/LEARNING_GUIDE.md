# 源码学习导读

这份文档不是运行手册，而是帮助你真正读懂本项目的学习路线。建议一边阅读，一边在 IDE 中跳转到对应类型和函数；每读完一个阶段，再运行对应测试或板端脚本验证自己的理解。

配套查询文档：遇到某个类型/函数不清楚时看 [《类与函数源码参考手册》](CODE_REFERENCE.md)；想知道它如何进入六个可执行程序、线程和脚本时看 [《可执行程序与脚本走读》](PROGRAM_WALKTHROUGH.md)。

项目的核心不是“调用摄像头和编码 API”，而是处理好四件事：

1. Linux 驱动缓冲区的所有权；
2. NV12 图像布局和硬件对齐；
3. 多线程之间的有界数据流；
4. 出错、丢帧和停机时资源仍能完整回收。

## 1. 先建立整体认识

最终优化链路如下：

```text
OV8858 / RKISP
      │
      │ V4L2 DQBUF：应用取得一个相机 buffer lease
      ▼
Capture Thread
      │
      │ DmabufFrame（只含 index/timestamp，不复制像素）
      ▼
Bounded Lease Queue（容量 1，满时丢最旧 lease 并立即 QBUF）
      │
      ▼
Encode Thread
      ├─ RGA：相机 DMA-BUF → MPP 对齐 DMA-BUF
      ├─ V4L2 QBUF：RGA 完成后立即归还源 lease
      └─ MPP：NV12 → H.264 access unit
      │
      ▼
Bounded Packet Queue（容量 8，存放自有压缩字节）
      │
      ▼
Network Thread
      ├─ 检查包序号；缺口后等待 IDR
      └─ FFmpeg RTSP Publisher → MediaMTX → VLC / ffplay
```

CPU-copy 基线保持同样的三线程结构，但原始帧传递方式不同：

```text
V4L2 buffer
   └─ CPU memcpy → Frame::data → CPU 逐行 copy → MPP input buffer
```

因此两条路径可以做相对公平的性能对照。

## 2. 推荐阅读顺序

不要直接从 592 行的完整主程序开始。按下面顺序阅读会容易很多。

### 第一阶段：数据结构和小工具

1. [`frame.h`](../include/rkstream/frame.h)
   - 理解普通 `Frame` 为什么拥有 `std::vector<std::byte>`；
   - 区分 driver timestamp 和 dequeue timestamp；
   - 理解 `width`、`height` 与 `bytes_per_line` 的区别。
2. [`pixel_format.h`](../include/rkstream/pixel_format.h) 和 [`pixel_format.cpp`](../src/pixel_format.cpp)
   - 理解 fourcc 只是 4 个字符编码成的整数。
3. [`annexb.h`](../include/rkstream/annexb.h) 和 [`annexb.cpp`](../src/annexb.cpp)
   - 认识 H.264 的 SPS、PPS、IDR；
   - 理解为什么该检查不是完整视频解码。
4. [`nv12_layout.h`](../include/rkstream/nv12_layout.h)
   - 这是读懂 RGA 优化的前提。

### 第二阶段：有界队列

阅读 [`bounded_latest_queue.h`](../include/rkstream/bounded_latest_queue.h) 和 [`test_bounded_latest_queue.cpp`](../tests/test_bounded_latest_queue.cpp)。重点回答：

- 为什么满时丢最旧帧，而不是拒绝新帧？
- `close()` 为什么不清空现有项？
- `pop()` 为什么返回 `std::optional<T>`？
- `dropped_value` 为什么对 DMA-BUF 路径必不可少？

### 第三阶段：CPU-copy V4L2 基线

阅读：

1. [`v4l2_capture.h`](../include/rkstream/v4l2_capture.h)
2. [`v4l2_capture.cpp`](../src/v4l2_capture.cpp)
3. [`capture_probe.cpp`](../apps/capture_probe.cpp)

先只理解下面这条生命周期：

```text
open device
  → QUERYCAP
  → S_FMT / 读回实际格式
  → S_PARM / G_PARM
  → REQBUFS
  → QUERYBUF + mmap
  → QBUF all
  → STREAMON
  → poll / DQBUF / memcpy / QBUF（循环）
  → STREAMOFF
  → munmap / close
```

### 第四阶段：MPP 编码

阅读：

1. [`mpp_h264_encoder.h`](../include/rkstream/mpp_h264_encoder.h)
2. [`mpp_h264_encoder.cpp`](../src/mpp_h264_encoder.cpp)
3. [`mpp_file_encode.cpp`](../apps/mpp_file_encode.cpp)

先掌握 CPU-copy 的 `encode_nv12()`，再看由 RGA 写入的 `encode_prepared_nv12()`。

### 第五阶段：CPU-copy RTSP 三线程基线

阅读：

1. [`rtsp_publisher.h`](../include/rkstream/rtsp_publisher.h)
2. [`rtsp_publisher.cpp`](../src/rtsp_publisher.cpp)
3. [`keyframe_resync.h`](../include/rkstream/keyframe_resync.h)
4. [`rtsp_stream.cpp`](../apps/rtsp_stream.cpp)

此时重点是线程边界和有序停机，不要急着研究 DMA-BUF。

### 第六阶段：DMA-BUF 与 RGA

阅读：

1. [`v4l2_dmabuf_capture.h`](../include/rkstream/v4l2_dmabuf_capture.h)
2. [`v4l2_dmabuf_capture.cpp`](../src/v4l2_dmabuf_capture.cpp)
3. [`rga_nv12_aligner.h`](../include/rkstream/rga_nv12_aligner.h)
4. [`rga_nv12_aligner.cpp`](../src/rga_nv12_aligner.cpp)
5. [`dmabuf_probe.cpp`](../apps/dmabuf_probe.cpp)
6. [`dmabuf_mpp_encode.cpp`](../apps/dmabuf_mpp_encode.cpp)

### 第七阶段：最终优化链路

最后阅读：

1. [`dmabuf_queue_policy.h`](../include/rkstream/dmabuf_queue_policy.h)
2. [`dmabuf_rtsp_stream.cpp`](../apps/dmabuf_rtsp_stream.cpp)
3. [`RTSP_VALIDATION.md`](RTSP_VALIDATION.md)

读完后应能独立解释每一种 lease 归还路径。

## 3. V4L2 到底在管理什么

### 3.1 Buffer 不是 Frame

V4L2 buffer 是一个可重复使用的内存 slot，frame 是某一时刻写入这个 slot 的图像内容。4 个 buffer 并不表示系统只能处理 4 帧，而是 4 个 slot 在驱动和应用之间循环：

```text
QBUF：slot 交给驱动，可写入新画面
  ↓
摄像头/ISP 写完
  ↓
DQBUF：slot 交给应用，驱动暂时不能覆盖
  ↓
应用读取完成
  ↓
QBUF：归还 slot
```

如果应用 DQBUF 后一直不 QBUF，可供摄像头使用的 slot 会越来越少，最终没有地方写新帧。

### 3.2 MMAP 与 EXPBUF 的区别

CPU-copy 路径：

- `VIDIOC_QUERYBUF` 取得 offset/length；
- `mmap()` 把驱动 buffer 映射到 CPU 虚拟地址；
- 每次 DQBUF 后把有效字节复制到 `Frame::data`；
- 复制结束即可立即 QBUF。

DMA-BUF 路径：

- 驱动仍通过 `V4L2_MEMORY_MMAP` 分配 buffer；
- `VIDIOC_EXPBUF` 将每个 buffer 导出为 fd；
- RGA 通过 fd 访问同一块 DMA 缓冲，不需要 CPU 复制像素；
- RGA 读取结束前不能 QBUF，否则摄像头可能同时覆盖它。

注意：这里是“导出 V4L2 分配的 buffer”，不是 `V4L2_MEMORY_DMABUF` 的“导入应用分配的 buffer”。两种模式名字相近，但所有权方向不同。

### 3.3 Single-planar 与 multi-planar

USB UVC 常使用 `V4L2_BUF_TYPE_VIDEO_CAPTURE`；RKISP mainpath 使用 `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`。

`multi-planar` 是 V4L2 API 组织 buffer 的方式，不代表 NV12 一定返回两个 V4L2 plane。本项目的 OV8858 NV12 实际返回一个 V4L2 plane，其中先放 Y，再放交错 UV。

## 4. NV12、stride 和对齐

### 4.1 NV12 内存结构

NV12 是 YUV 4:2:0 semi-planar：

```text
Y plane：每个像素一个 Y，height 行
UV plane：U/V 交错，height / 2 行
```

在没有额外 padding 时，理论字节数为：

```text
stride × height × 3 / 2
```

代码不能简单写 `width × height × 3 / 2`，因为 stride 可能大于 width。

### 4.2 本机 OV8858 的具体数字

相机布局：

```text
可见尺寸          1632 × 1224
source stride     1632
source UV offset  1632 × 1224 = 1,997,568
source bytes      2,996,352
```

MPP 使用 16 对齐：

```text
horizontal stride 1632
vertical stride   align_up(1224, 16) = 1232
MPP UV offset     1632 × 1232 = 2,010,624
MPP bytes         3,015,936
```

两者 UV 起点相差 13,056 字节，总缓冲大小相差 19,584 字节。因此即使相机 DMA-BUF 容量看起来足够，也不能让 MPP 按 1232 高度直接解释 UV 起点。RGA 的作用是把可见区域从源布局整理到目标布局。

### 4.3 为什么不是“纯零拷贝”

优化路径没有 CPU memcpy，但 RGA 仍执行一次硬件 copy：

```text
camera DMA-BUF --RGA copy--> MPP DMA-BUF
```

所以准确表述是：

> 使用 DMA-BUF 在设备间共享缓冲句柄，并用 RGA 完成硬件布局拷贝，消除用户态原始帧 CPU 拷贝。

## 5. DMA-BUF lease 所有权

### 5.1 lease 状态机

每个相机 slot 只有两个合法状态：

```text
queued_[index] = true
    slot 属于 V4L2 驱动
        │ DQBUF
        ▼
queued_[index] = false
    slot 作为 lease 属于应用
        │ release / QBUF
        ▼
queued_[index] = true
```

不允许：

- 同一个 index 连续 release 两次；
- lease 尚在 RGA 读取时提前 release；
- 队列丢弃 lease 后忘记 release；
- 编码线程错误退出后不 drain 残留 lease。

### 5.2 所有归还路径

在 `dmabuf_rtsp_stream.cpp` 中逐一查找 `release_lease`：

1. RGA 成功后正常归还；
2. RGA 失败后仍归还；
3. frame queue 已关闭、push 被拒绝时归还当前 lease；
4. frame queue 满、drop-oldest 时归还被丢弃 lease；
5. encoder 提前退出，主线程 drain 时归还残留 lease。

最终运行必须满足：

```text
captured == leases_released
outstanding_leases == 0
lease_release_failures == 0
```

视频能够播放不等于 ownership 正确。短时间漏一个 lease 可能仍能运行，但长时间后会逐步耗尽相机 slot。

### 5.3 为什么 4 个相机 buffer 的 lease queue 只能设为 1

预算如下：

```text
1 个：可能正在 encoder/RGA 手中
2 个：至少留给驱动轮转
剩余：才允许放进跨线程 frame queue
```

所以：

```text
max_queue = camera_buffers - 1 - 2
          = 4 - 1 - 2
          = 1
```

这不是随意调小的参数，而是根据资源所有权推导出的上限。

## 6. 有界队列和实时性

假设摄像头每秒产生 24 帧，而编码器暂时只能处理 10 帧：

无界队列会不断积压，画面逐渐落后几秒甚至几十秒。实时系统更关心“现在发生什么”，所以本项目在队列满时丢最旧项：

```text
旧：A B C D（已满）
新帧 E 到来
结果：B C D E
```

必须区分两类丢帧：

- `driver_gaps`：V4L2 sequence 不连续，说明采集侧没有收到某些帧；
- `frame_drop_oldest`：应用主动丢旧项，是受控的背压策略。

主动丢帧并非永远是错误。只要队列有界、延迟不持续增长、资源正确归还，它是实时系统的正常退化行为。

## 7. Rockchip MPP 编码流程

### 7.1 初始化

`MppH264Encoder::open()` 的主要步骤：

```text
mpp_create
  → mpp_init(MPP_CTX_ENC, AVC)
  → mpp_enc_cfg_init
  → 设置 prep / rc / gop / profile
  → MPP_ENC_SET_CFG
  → 分配 input/output MppBuffer
  → MPP_ENC_GET_HDR_SYNC 取得 SPS/PPS
```

项目使用：

- H.264 High Profile；
- CBR 目标 4 Mbps；
- 编码时间基准 24 FPS；
- GOP 48，即正常情况下约每 2 秒一个 IDR。

### 7.2 为什么 header packet 必须有真实 MppBuffer

`MPP_ENC_GET_HDR_SYNC` 不替调用者分配 packet storage。正确顺序是：

```text
mpp_buffer_get(output_buffer)
mpp_packet_init_with_buffer(packet, output_buffer)
mpp_packet_set_length(packet, 0)
MPP_ENC_GET_HDR_SYNC
```

传入没有底层 buffer 的空 packet，在某些板端 MPP 版本会直接崩溃。这是本项目通过官方 `mpi_enc_test` 对照定位过的真实问题。

### 7.3 编码一帧

```text
准备 input MppBuffer
  → 创建 MppFrame 并设置 width/height/stride/format/pts
  → 创建绑定 output MppBuffer 的 MppPacket
  → KEY_OUTPUT_PACKET 挂到 frame metadata
  → encode_put_frame
  → encode_get_packet
  → 复制压缩字节到 EncodedPacket::bytes
```

原始 NV12 每帧约 3 MB；H.264 压缩包通常只有数 KB 到数十 KB。因此 packet queue 复制压缩数据的成本，远低于复制原始图像。

## 8. H.264 丢包与 IDR 重同步

H.264 P 帧依赖之前参考帧。如果 packet queue 丢了序号 10，序号 11 即使成功发送，也可能无法正确解码。

`KeyframeResync` 的策略：

```text
发现 encoded_sequence gap
  → waiting_for_keyframe = true
  → 丢弃后续非 IDR 包
  → 遇到 IDR 后恢复发布
```

首次启动也等待 IDR，保证客户端从独立解码点开始。代价是晚加入客户端最坏需要等待一个 GOP；当前 GOP 48、约 24 FPS，理论等待上限接近 2 秒。

## 9. RTSP 中各组件的职责

不要把 FFmpeg、MediaMTX、播放器混成一个模块：

| 组件 | 职责 |
|---|---|
| `MppH264Encoder` | 将 NV12 编码为 H.264 |
| `RtspPublisher` | 用 FFmpeg API 把 H.264 封装并发布到 RTSP URL |
| MediaMTX | 监听 8554，接收 publisher，并向客户端提供 RTSP 服务 |
| VLC / ffplay | RTSP 客户端，接收、解码、显示 |

浏览器地址栏通常不能直接播放 `rtsp://`，它会把地址当搜索词。应使用 VLC、ffplay 或支持 RTSP 的客户端。

项目默认让 publisher 到 MediaMTX 使用 RTSP/TCP，主要为了局域网演示稳定和易诊断。UDP 可能降低部分场景延迟，但会引入丢包、乱序和 NAT 等额外变量，不应在没有对照测试时简单宣称更快。

## 10. 三线程为什么这样拆分

### Capture thread

只做：

- `poll/DQBUF`；
- 记录驱动 sequence/timestamp；
- 将 Frame 或 lease 推入有界队列；
- 处理 drop-oldest 的资源归还。

它不应执行网络写入或长耗时编码，否则相机 buffer 无法及时轮转。

### Encode thread

CPU 基线做逐行 copy + MPP；DMA 路径做 RGA + QBUF + MPP。它把原始图像转换成独立拥有字节的 `EncodedPacket`。

### Network thread

处理包连续性和同步 RTSP 写入。网络暂时阻塞时不会直接卡住摄像头线程，而是由 packet queue 吸收小幅波动；超过容量后按 drop-oldest + IDR resync 退化。

## 11. 停机顺序

正确停机同样是数据流的一部分：

```text
设置 stop
  → join capture producer
  → close frame queue
  → join encoder
  → drain/release 残留 DMA lease
  → close packet queue
  → join network
  → close RGA / capture / encoder
```

如果先销毁 capture，再让 encoder 归还 lease，就会访问已经关闭的 V4L2 fd；如果网络线程先退出但 packet producer 不停，编码包会继续堆积或被无意义丢弃。

## 12. 如何读运行指标

### 吞吐

```text
stream_fps = published / elapsed_s
```

当前约 23.8 FPS 的上限来自实际摄像头输出约 23.9 FPS，不是 MPP 只能编码 24 FPS。

### 内部管线耗时

```text
internal_pipeline = RTSP write 完成时间 - V4L2 DQBUF 时间
```

它包含应用内部排队、RGA/CPU copy、MPP 和 publisher 写入调用，但不包含：

- 传感器曝光；
- ISP 在 DQBUF 前的处理；
- 网络在途；
- VLC/ffplay 解码和播放缓冲；
- 显示器刷新。

因此它不能冒充“摄像头到屏幕端到端延迟”。后者需要拍摄时钟/LED 等光学方法测量。

### CPU 与 RSS

```text
cpu_percent = (user_cpu_s + system_cpu_s) / elapsed_s × 100%
```

Linux `ru_maxrss` 在本平台单位为 KiB，表示进程运行期间的峰值常驻内存，不等于当前时刻内存。

### 队列健康

重点一起看：

- `high_watermark`：实际最高占了多少槽位；
- `drop_oldest`：消费者是否跟不上；
- `max_latency_ms`：项在队列里最久等了多久；
- `driver_gaps`：摄像头序号是否真的缺失。

## 13. 测试保护了什么

| 测试 | 保护的规则 |
|---|---|
| `test_pixel_format` | 命令行格式名称和 fourcc 转换正确 |
| `test_bounded_latest_queue` | 容量固定、满时丢最旧、close/drain 语义正确 |
| `test_annexb` | 3/4 字节起始码及 SPS/PPS/IDR 统计正确 |
| `test_keyframe_resync` | 首帧/丢包后等待 IDR，连续包正常通过 |
| `test_nv12_layout` | stride、UV offset、MPP 对齐和兼容性判断正确 |
| `test_dmabuf_queue_policy` | lease queue 不会占尽相机 buffer |

这些是离线单元测试。真正的 V4L2、RGA、MPP 和 RTSP 仍必须在 RK3588 板端实际运行，因为主机没有同样的驱动和硬件设备。

## 14. 脚本应该怎样读

脚本不是“藏住命令”，而是把一次验证固化为可重复流程：

| 脚本 | 目的 |
|---|---|
| `build_board.sh` | 板端本地 fresh configure/build + CTest |
| `probe_cameras.sh` | 查看两个摄像头真实驱动能力和格式 |
| `run_capture_probe.sh` | MIPI/USB 采集与背压测试 |
| `run_mpp_file_encode.sh` | CPU-copy 文件编码 + ffprobe + 全量解码 |
| `run_dmabuf_probe.sh` | 单帧 EXPBUF/RGA 布局审计 |
| `run_encode_benchmark.sh` | CPU 与 DMA/RGA 两条编码路径同条件对照 |
| `run_rtsp_stream.sh` | CPU-copy RTSP 回退路径 |
| `run_dmabuf_rtsp_stream.sh` | 推荐的 DMA-BUF/RGA RTSP 路径 |

构建目录放在 `/home/cat/Build`，避免把大量编译小文件写回 PC 的 NFS 共享源码目录。

## 15. 常见误区

1. **协商 30 FPS 就写“实测 30 FPS”**：错误。必须看实际帧数和 driver timestamp。
2. **`stride == width` 永远成立**：错误。很多硬件格式需要按 16/64 字节对齐。
3. **导出 DMA-BUF fd 就是纯零拷贝**：错误。还要检查两端布局是否兼容。
4. **队列越大越稳定**：实时视频中可能越大越延迟。
5. **丢 H.264 P 帧后继续发就行**：参考链已断，应等 IDR。
6. **视频能播就说明没有资源泄漏**：错误，必须审计 lease balance。
7. **RTSP 地址可以直接用普通浏览器打开**：多数浏览器不原生支持 RTSP。
8. **FFmpeg 在本项目里做软件编码**：错误，编码由 RKMPP 完成，FFmpeg 只 mux/publish。

## 16. 建议动手练习

按难度从低到高完成，先写下预期，再运行验证：

1. 把 `capture_probe` 的消费者延迟设为 100 ms，解释为什么 driver gap 为 0 但 drop-oldest 增加。
2. 手算 1920×1080、source stride 1920 在 16 对齐下的 UV offset 和总字节数。
3. 给 `BoundedLatestQueue<int>` 写容量 2 的手工状态表。
4. 画出一个 DMA lease 从 DQBUF 到 QBUF 的全部可能路径。
5. 假设 packet queue 丢了序号 20，列出 `KeyframeResync` 对 21P、22P、23IDR 的决策。
6. 对比 CPU 与 DMA 两份 summary，解释为什么 FPS 接近但 CPU/RSS 明显不同。
7. 将 GOP 从 48 改为 24 做独立实验，分析晚加入等待、码率和画质之间的取舍；不要把实验值直接改成生产默认值。

## 17. 学习自检问题

如果能不看代码清楚回答下面问题，说明你已经真正掌握了项目主干：

1. `VIDIOC_QBUF` 和 `VIDIOC_DQBUF` 分别转移了什么所有权？
2. 为什么 DMA-BUF 路径的 frame queue 容量是 1，而 CPU-copy 路径可以是 4？
3. NV12 的 UV plane 为什么在本机必须由 RGA 重新定位？
4. 什么是 stride？它为什么可能不等于 width？
5. 为什么实时队列选择 drop-oldest？
6. frame queue 和 packet queue 丢项后的处理为什么不同？
7. SPS、PPS、IDR 各自起什么作用？
8. MediaMTX 和 FFmpeg publisher 的职责有何区别？
9. 当前 `avg_internal_pipeline_ms` 为什么不是端到端显示延迟？
10. 如何证明优化确实有效，而不是场景变化造成的偶然结果？

## 18. 出错时的定位顺序

### 摄像头打不开或无帧

1. `probe_cameras.sh` 检查节点、格式和驱动；
2. `capture_probe` 单独验证 V4L2；
3. 看 `capture_timeouts` 和 `driver_gaps`；
4. 确认没有另一个进程占用摄像头。

### H.264 文件不能解码

1. 检查 SPS/PPS/IDR 统计；
2. 检查 source/MPP stride 和 UV offset；
3. 用 `ffprobe` 看宽高/profile；
4. 用 `ffmpeg -v error -f h264 -i ... -f null -` 做全量解码；
5. 再排查 MPP packet buffer 生命周期。

### RTSP 不能播放

1. 先确认文件编码路径通过；
2. 检查 8554 端口和 MediaMTX 日志；
3. 确认 C++ publisher 已输出 READY；
4. 板端 `ffprobe` 本机 URL；
5. PC 用 VLC/ffplay，不用浏览器地址栏；
6. 最后再检查网络路由和防火墙。

### 跑一段时间后停帧

优先查看：

```text
captured
leases_released
outstanding_leases
lease_release_failures
frame_queue_high/drop_oldest
driver_gaps/capture_timeouts
```

若 `outstanding_leases` 增长，先找漏掉的 QBUF 路径，不要通过增加 buffer 数掩盖问题。

---

建议第一次按顺序完整读完；第二次阅读时，从 `dmabuf_rtsp_stream.cpp` 的三个线程反向跳转到各组件。两遍之后，再尝试不看文档独立画出所有权和停机顺序。
