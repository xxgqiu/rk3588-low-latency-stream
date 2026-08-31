# 类与函数源码参考手册

本文面向“打开一个类或函数时，想马上知道它做什么、谁拥有资源、应该在哪个线程调用”的阅读场景。概念学习顺序请先看 [《源码学习导读》](LEARNING_GUIDE.md)，完整程序如何把这些类组合起来请看 [《可执行程序与脚本走读》](PROGRAM_WALKTHROUGH.md)。

## 1. 阅读约定

### 1.1 返回值约定

本项目底层硬件封装主要使用以下形式：

```cpp
bool operation(..., std::string* error);
```

- 返回 `true`：操作成功；
- 返回 `false`：操作失败；
- `error != nullptr` 时写入可读错误；
- 调用者可以传 `nullptr`，表示只关心成功与否；
- 发生异常通常只用于命令行参数错误，V4L2/MPP/RGA/FFmpeg 运行错误不依赖异常传播。

### 1.2 所有权词汇

| 词汇 | 含义 |
|---|---|
| 拥有 | 对象负责最终释放资源，例如 `close(fd)`、`munmap`、`mpp_buffer_put` |
| 借用 | 只能临时访问，不得释放，且不能超过资源拥有者生命周期 |
| lease / 租约 | 应用暂时取得 V4L2 slot 的使用权，必须显式 `release()` 归还 |
| 独立副本 | 数据存于自己的 `std::vector`，离开原始驱动缓冲后仍有效 |
| 移动进队列 | 原对象内容转交队列，生产者原则上不再使用被移动对象 |

### 1.3 线程安全级别

不是所有类都设计成任意线程并发调用：

| 类型 | 线程约束 |
|---|---|
| `BoundedLatestQueue<T>` | `push/pop/close/drain/metrics` 内部加锁，可跨线程 |
| `V4L2DmabufCapture` | `capture()` 与 `release()` 的 ioctl 状态已串行化；初始化和 `close()` 不得并发 |
| `V4L2Capture` | 预期只由一个采集线程调用 |
| `MppH264Encoder` | 预期只由一个编码线程调用 |
| `RgaNv12Aligner` | 预期只由一个编码线程调用 |
| `RtspPublisher` | 预期只由一个网络线程调用 |
| `KeyframeResync` | 无锁状态机，只能由一个有序包流调用 |

## 2. 文件与模块总览

| 模块 | 公开头文件 | 实现 | 主要职责 |
|---|---|---|---|
| 帧数据 | [`frame.h`](../include/rkstream/frame.h) | 纯结构体 | CPU-copy 图像帧 |
| 像素格式 | [`pixel_format.h`](../include/rkstream/pixel_format.h) | [`pixel_format.cpp`](../src/pixel_format.cpp) | 名称与 V4L2 fourcc 转换 |
| V4L2 基线 | [`v4l2_capture.h`](../include/rkstream/v4l2_capture.h) | [`v4l2_capture.cpp`](../src/v4l2_capture.cpp) | MMAP + CPU-copy 采集 |
| V4L2 DMA | [`v4l2_dmabuf_capture.h`](../include/rkstream/v4l2_dmabuf_capture.h) | [`v4l2_dmabuf_capture.cpp`](../src/v4l2_dmabuf_capture.cpp) | EXPBUF + lease 采集 |
| 实时队列 | [`bounded_latest_queue.h`](../include/rkstream/bounded_latest_queue.h) | header-only | 有界、满时丢最旧 |
| DMA 队列策略 | [`dmabuf_queue_policy.h`](../include/rkstream/dmabuf_queue_policy.h) | header-only | 推导安全 lease queue 容量 |
| NV12 布局 | [`nv12_layout.h`](../include/rkstream/nv12_layout.h) | header-only | stride/offset/兼容性计算 |
| MPP 编码 | [`mpp_h264_encoder.h`](../include/rkstream/mpp_h264_encoder.h) | [`mpp_h264_encoder.cpp`](../src/mpp_h264_encoder.cpp) | NV12 → H.264 |
| RGA 整理 | [`rga_nv12_aligner.h`](../include/rkstream/rga_nv12_aligner.h) | [`rga_nv12_aligner.cpp`](../src/rga_nv12_aligner.cpp) | DMA-BUF 硬件布局 copy |
| Annex-B | [`annexb.h`](../include/rkstream/annexb.h) | [`annexb.cpp`](../src/annexb.cpp) | SPS/PPS/IDR 结构检查 |
| 丢包恢复 | [`keyframe_resync.h`](../include/rkstream/keyframe_resync.h) | header-only | 缺口后等待 IDR |
| RTSP 发布 | [`rtsp_publisher.h`](../include/rkstream/rtsp_publisher.h) | [`rtsp_publisher.cpp`](../src/rtsp_publisher.cpp) | FFmpeg mux + RTSP 网络发送 |

## 3. `Frame`：CPU 内存图像帧

位置：[`include/rkstream/frame.h`](../include/rkstream/frame.h)

```cpp
struct Frame;
```

`Frame` 是 CPU-copy 路径在线程之间传递的值对象。它拥有 `data`，所以从 `V4L2Capture::capture()` 返回后不再依赖驱动 buffer。

### 字段

| 字段 | 单位/类型 | 来源 | 用途 |
|---|---|---|---|
| `sequence` | `uint32_t` | `v4l2_buffer.sequence` | 判断驱动帧序号是否连续 |
| `driver_timestamp_us` | 微秒 | `v4l2_buffer.timestamp` | 作为编码 PTS，分析相机输出节奏 |
| `dequeue_timestamp_us` | 微秒 | `steady_clock` | 计算应用内部排队与处理耗时 |
| `timestamp_flags` | bit flags | `v4l2_buffer.flags` | 保留 timestamp clock/source 语义 |
| `pixel_format` | V4L2 fourcc | 协商结果 | 表示 NV12/YUYV/MJPEG 等格式 |
| `width` / `height` | 像素 | 协商结果 | 可见图像尺寸 |
| `bytes_per_line` | 字节 | 协商结果 | 源图像 stride |
| `data` | 字节 vector | MMAP buffer copy | 完整有效帧数据的独立所有权 |

### 生命周期

```text
V4L2 DQBUF
  → copy 到 Frame::data
  → 原始 V4L2 slot 立即 QBUF
  → Frame 移入 frame queue
  → encoder pop
  → 编码结束后 Frame 析构，vector 自动释放
```

## 4. 像素格式函数

位置：[`pixel_format.h`](../include/rkstream/pixel_format.h)、[`pixel_format.cpp`](../src/pixel_format.cpp)

### `parse_pixel_format`

```cpp
uint32_t parse_pixel_format(const std::string& name);
```

作用：把用户输入转成 V4L2 fourcc。

支持值（忽略大小写）：

| 输入 | 返回 |
|---|---|
| `nv12` | `V4L2_PIX_FMT_NV12` |
| `yuyv` / `yuy2` | `V4L2_PIX_FMT_YUYV` |
| `mjpeg` / `mjpg` | `V4L2_PIX_FMT_MJPEG` |

不支持时抛出 `std::invalid_argument`。它只解析名称，不检查具体摄像头是否支持该格式。

### `pixel_format_name`

```cpp
std::string pixel_format_name(uint32_t fourcc);
```

把 fourcc 低到高四个字节还原为长度 4 的字符串，例如 `NV12`。用于日志与格式协商错误，不负责判断格式有效性。

## 5. V4L2 配置与结果类型

位置：[`v4l2_capture.h`](../include/rkstream/v4l2_capture.h)

### `CaptureConfig`

调用者请求的参数，不代表驱动最终接受值。

| 字段 | 含义 | 当前约束 |
|---|---|---|
| `device` | `/dev/video...` 节点 | 非空 |
| `width` / `height` | 请求可见尺寸 | 非零 |
| `fps` | 请求帧率 | 非零 |
| `pixel_format` | 请求 fourcc | 非零 |
| `buffer_count` | 请求 V4L2 slot 数量 | 至少 2，默认 4 |

### `CaptureInfo`

驱动协商后的真实结果。`V4L2Capture::info()` 和 `V4L2DmabufCapture::info()` 返回它的只读借用引用。

| 字段 | 含义 |
|---|---|
| `driver` | 驱动名，例如 `rkisp_v6` / `uvcvideo` |
| `card` | V4L2 card 描述 |
| `bus` | 总线标识 |
| `width` / `height` | 实际可见尺寸 |
| `fps_numerator` / `fps_denominator` | 实际声明帧率为前者除以后者 |
| `pixel_format` | 实际 fourcc |
| `bytes_per_line` | 实际 stride |
| `size_image` | 驱动声明的一帧缓冲总字节数 |
| `plane_count` | V4L2 API plane 数 |
| `buffer_count` | 驱动实际分配 slot 数 |
| `multi_planar` | 是否使用 `VIDEO_CAPTURE_MPLANE` API |

注意：声明帧率仍不是实测帧率。实测需要用帧数/运行时间或相邻 driver timestamp 计算。

### `CaptureResult`

| 枚举值 | 含义 | 调用者动作 |
|---|---|---|
| `kFrame` | 成功取得一帧 | 处理输出对象 |
| `kTimeout` | `poll` 超时或 DQBUF 暂时 `EAGAIN` | 计数后继续 |
| `kInterrupted` | `poll` 被信号打断 | 检查停止条件后继续/退出 |
| `kFatal` | 状态、ioctl、buffer 边界等致命错误 | 记录 error 并停止 pipeline |

## 6. `V4L2Capture`：MMAP + CPU-copy 采集器

位置：[`v4l2_capture.h`](../include/rkstream/v4l2_capture.h)、[`v4l2_capture.cpp`](../src/v4l2_capture.cpp)

### 类职责

- 打开 single-planar 或 multi-planar V4L2 capture 节点；
- 协商格式和帧率；
- 请求并 mmap 驱动 buffer；
- 每帧执行 `poll → DQBUF → memcpy → QBUF`；
- 用 RAII 在析构时停流、解除映射、关闭 fd。

类禁用拷贝构造和拷贝赋值，因为 fd 与 mmap 地址不能被两个对象重复释放。

### `open`

```cpp
bool open(const CaptureConfig& config, std::string* error);
```

前置条件：配置字段有效，设备没有被不兼容方式占用。

内部步骤：

1. `close()` 清理可能存在的旧状态；
2. `open(O_RDWR | O_NONBLOCK | O_CLOEXEC)`；
3. `VIDIOC_QUERYCAP` 检查 capture + streaming 能力；
4. 在 single/multi-planar 中选择驱动支持的 API；
5. `VIDIOC_S_FMT` 请求格式并读回宽高、fourcc、stride、size；
6. 若驱动擅自改了 fourcc，fail-closed；
7. `VIDIOC_S_PARM` 请求帧率，`VIDIOC_G_PARM` 读回；
8. `VIDIOC_REQBUFS` 请求 MMAP buffers；
9. 每个 slot 执行 `VIDIOC_QUERYBUF` 和 `mmap`；
10. 写入 `CaptureInfo`。

成功后尚未开流，必须继续调用 `start()`。

失败时会调用 `close()` 清理部分创建的 fd/mmap，不要求调用者补做清理。

### `start`

```cpp
bool start(std::string* error);
```

把全部 slot 先 `VIDIOC_QBUF` 给驱动，再执行 `VIDIOC_STREAMON`。若对象未成功 open、没有 buffer 或已经 streaming，则失败。

### `capture`

```cpp
CaptureResult capture(Frame* frame, int timeout_ms, std::string* error);
```

参数：

- `frame`：成功时写入完整帧，不能为空；
- `timeout_ms`：等待可读帧的单次 `poll` 超时；
- `error`：致命错误文本输出。

关键行为：

- 保留 V4L2 sequence/timestamp/flags；
- 对每个 V4L2 plane 使用 `bytesused`，为 0 才退回映射长度；
- 检查 `bytesused <= mapped length`；
- 将每个 plane 追加到 `Frame::data`；
- copy 完成后立即 QBUF，因此调用者不需要显式归还 Frame。

### `close`

```cpp
void close();
```

按 `STREAMOFF → munmap all → close(fd) → reset info` 顺序清理。允许重复调用，析构函数也会调用它。

### `info`

```cpp
const CaptureInfo& info() const;
```

返回内部引用，不复制。只能在 capture 对象存活且没有被 `close()`/重新 `open()` 的期间使用。

### 私有结构与函数

| 符号 | 作用 |
|---|---|
| `PlaneMapping` | 保存一个 plane 的 mmap 地址和长度 |
| `BufferMapping` | 保存一个 V4L2 slot 的所有 plane |
| `ioctl_retry` | ioctl 遇到 `EINTR` 自动重试 |
| `buffer_type` | 根据 `multi_planar` 返回正确 V4L2 buffer type |
| `queue_buffer` | 构造 buffer/planes 并执行 `VIDIOC_QBUF` |
| `unmap_buffers` | 遍历所有映射并 `munmap` |

### 成员状态

| 成员 | 所有权/含义 |
|---|---|
| `fd_` | 拥有 V4L2 fd，`-1` 表示关闭 |
| `streaming_` | 是否已成功 STREAMON |
| `info_` | 当前协商结果 |
| `buffers_` | 拥有全部 mmap 映射描述 |

## 7. DMA-BUF 帧与导出信息

位置：[`v4l2_dmabuf_capture.h`](../include/rkstream/v4l2_dmabuf_capture.h)

### `DmabufBufferInfo`

一个长期存在的 V4L2 slot 导出描述：

| 字段 | 含义 | 所有权 |
|---|---|---|
| `index` | V4L2 slot 编号 | 值 |
| `fd` | `VIDIOC_EXPBUF` 导出的 DMA-BUF fd | `V4L2DmabufCapture` 拥有，外部只借用 |
| `length` | 完整驱动缓冲长度 | 值 |

### `DmabufFrame`

一次 DQBUF 产生的轻量 lease，不包含像素副本：

| 字段 | 含义 |
|---|---|
| `buffer_index` | 对应 `buffers()[index]` 的 slot |
| `sequence` | 驱动帧序号 |
| `driver_timestamp_us` | 驱动时间戳 |
| `dequeue_timestamp_us` | 应用 DQBUF 时间 |
| `timestamp_flags` | 时间戳语义 flags |
| `bytes_used` | 本帧驱动报告有效字节数 |

复制 `DmabufFrame` 只复制描述，不复制资源，也不会自动增加引用计数。无论描述复制几份，同一个 slot 仍只能 release 一次。

## 8. `V4L2DmabufCapture`：EXPBUF + lease 采集器

位置：[`v4l2_dmabuf_capture.h`](../include/rkstream/v4l2_dmabuf_capture.h)、[`v4l2_dmabuf_capture.cpp`](../src/v4l2_dmabuf_capture.cpp)

### 类职责

- 仅接受当前已验证的 multi-planar、单 V4L2 plane 布局；
- 让 V4L2 驱动分配 MMAP slot；
- 对每个 slot 执行 `VIDIOC_EXPBUF` 导出 DMA-BUF fd；
- DQBUF 后把 slot 作为 lease 交给调用者；
- `release()` 时才 QBUF；
- 用 `queued_` 检查重复归还和非法状态。

### `open`

```cpp
bool open(const CaptureConfig& config, std::string* error);
```

与 `V4L2Capture::open()` 相似，但有额外限制：

- 设备必须支持 `V4L2_CAP_VIDEO_CAPTURE_MPLANE`；
- 实际 fourcc 必须等于请求格式；
- `plane_count` 必须为 1；
- `bytes_per_line >= width`；
- 每个 QUERYBUF slot 都必须能 EXPBUF。

成功后：

- `exported_buffers_` 拥有全部导出 fd；
- `queued_` 初始全部为 `false`，因为尚未 QBUF；
- `buffers()` 可供 RGA 初始化时借用 fd。

### `start`

将全部 slot QBUF，使 `queued_[i] = true`，再 STREAMON。

### `capture`

```cpp
CaptureResult capture(DmabufFrame* frame, int timeout_ms, std::string* error);
```

成功 DQBUF 后：

```text
queued_[index]: true → false
驱动所有权       → 应用 lease 所有权
```

函数只填充 `DmabufFrame` 元数据，不 mmap、不 memcpy 像素，也不会自动 QBUF。

### `release`

```cpp
bool release(const DmabufFrame& frame, std::string* error);
```

验证 streaming、index 范围和未重复归还后调用 `queue_buffer()`：

```text
queued_[index]: false → true
应用 lease 所有权     → 驱动所有权
```

每个成功 `capture(kFrame)` 最终必须对应一次成功 `release()`。

### `buffers`

```cpp
const std::vector<DmabufBufferInfo>& buffers() const;
```

返回所有导出 slot 的借用列表。RGA 在初始化时导入这些 fd；调用者不得 `close(fd)`。

### `close`

STREAMOFF 后关闭所有 EXPBUF fd，再关闭 V4L2 fd。调用前必须先停止工作线程并尽可能 drain/release lease；不能让 RGA 仍读取 fd 时并发 close。

### 私有状态与并发

| 成员 | 作用 |
|---|---|
| `exported_buffers_` | 拥有 DMA-BUF fd |
| `queued_` | slot 在驱动/应用哪一侧的状态表 |
| `ioctl_mutex_` | 串行化 capture 线程 DQBUF 与 encode 线程 QBUF |

`open/start/close` 仍是单线程生命周期操作，只有稳定运行期的 `capture/release` 设计为跨两个线程协作。

## 9. `BoundedLatestQueue<T>`：实时有界队列

位置：[`bounded_latest_queue.h`](../include/rkstream/bounded_latest_queue.h)

### 模板职责

- 队列容量构造后固定；
- 空队列 `pop()` 阻塞；
- 满队列 `push()` 自动移出最旧元素后插入最新元素；
- 记录累计丢弃数和最高水位；
- 支持 close 唤醒和 drain 所有权回收。

### 构造函数

```cpp
explicit BoundedLatestQueue(std::size_t capacity);
```

`capacity == 0` 抛出 `std::invalid_argument`。类不可复制，因为内部包含 mutex、condition variable 和唯一队列状态。

### `PushResult`

| 字段 | 含义 |
|---|---|
| `accepted` | 新值是否进入队列；已 close 时为 false |
| `dropped_oldest` | 是否因满队列移出了旧值 |
| `size` | push 完成后的队列大小 |
| `dropped_value` | 被移出的旧值及其所有权 |

对普通 `Frame`，`dropped_value` 析构即可。对 `DmabufFrame`，调用者必须取出并 `capture.release()`。

### `push`

```cpp
PushResult push(T value);
```

参数按值接收并移动入队。若队列已 close，返回 `accepted=false`。对于外部资源描述符，调用者应像主程序一样在 push 前保留可归还的轻量描述，因为 rejected input 不会作为单独字段返回。

满队列顺序：

```text
move front → dropped_value
pop_front
++dropped_count
push_back(new value)
更新 high_watermark
notify_one
```

### `pop`

```cpp
std::optional<T> pop();
```

- 队列非空：移动返回最旧值；
- 队列空且未 close：阻塞；
- 队列 close 但仍非空：继续返回残留项；
- 队列 close 且已空：返回 `std::nullopt`，消费者退出循环。

### `close`

```cpp
void close();
```

只设置 `closed_` 并 `notify_all()`：不清空已有元素，不释放外部资源，不等待线程结束。

### `drain`

```cpp
std::vector<T> drain();
```

将所有残留项移动到 vector 并返回。主要用于编码线程异常提前退出后，主线程集中 release 尚在 frame queue 的 DMA leases。

### 指标函数

| 函数 | 含义 |
|---|---|
| `size()` | 当前队列长度，内部加锁 |
| `capacity()` | 固定容量，无需加锁 |
| `dropped_count()` | 累计 drop-oldest 次数 |
| `high_watermark()` | 运行期间达到过的最大长度 |

## 10. DMA-BUF 队列容量函数

位置：[`dmabuf_queue_policy.h`](../include/rkstream/dmabuf_queue_policy.h)

### `max_dmabuf_lease_queue_capacity`

```cpp
constexpr std::size_t max_dmabuf_lease_queue_capacity(std::size_t camera_buffers);
```

计算公式：

```text
camera buffers - 1 个 encoder lease - 2 个 driver reserve
```

若总数不大于 3，返回 0。当前 4 buffers 得到安全上限 1。

### `valid_dmabuf_lease_queue_capacity`

```cpp
constexpr bool valid_dmabuf_lease_queue_capacity(
    std::size_t camera_buffers,
    std::size_t queue_capacity);
```

要求 `queue_capacity > 0` 且不超过上面的安全上限。主程序在开流前验证，避免运行中才因相机 slot 耗尽停顿。

## 11. NV12 布局函数与审计结构

位置：[`nv12_layout.h`](../include/rkstream/nv12_layout.h)

### `align_up`

```cpp
constexpr uint32_t align_up(uint32_t value, uint32_t alignment);
```

将 value 向上取整到 alignment 倍数；`alignment == 0` 原样返回。纯函数、编译期可计算。

### `Nv12LayoutAudit`

| 字段 | 含义 |
|---|---|
| `width` / `height` | 可见区域 |
| `source_stride` | 相机每行跨度 |
| `mpp_horizontal_stride` | MPP 对齐后行跨度 |
| `mpp_vertical_stride` | MPP 对齐后行数 |
| `source_y_offset` | 历史命名；实际为源 UV 起点 `source_stride × height` |
| `mpp_y_offset` | 历史命名；实际为目标 UV 起点 `h_stride × v_stride` |
| `source_buffer_bytes` | 驱动声明或计算的源总字节数 |
| `mpp_buffer_bytes` | MPP 目标总字节数 |
| `alignment_padding_bytes` | 目标比源多出的对齐空间 |
| `direct_mpp_layout_compatible` | 是否可以不做布局转换直接按 MPP 布局读取 |

### `audit_nv12_layout`

```cpp
constexpr Nv12LayoutAudit audit_nv12_layout(
    uint32_t width,
    uint32_t height,
    uint32_t source_stride,
    std::size_t driver_size_image,
    uint32_t mpp_alignment = 16);
```

若 `driver_size_image == 0`，源大小按 `source_stride × height × 3 / 2` 计算。只有水平 stride、垂直 stride、UV offset 和容量全部兼容时，`direct_mpp_layout_compatible` 才为 true。

该函数只判断内存布局，不导入 fd、不操作 RGA/MPP。

## 12. H.264 Annex-B 结构检查

位置：[`annexb.h`](../include/rkstream/annexb.h)、[`annexb.cpp`](../src/annexb.cpp)

### `AnnexBStats`

| 字段 | NAL type | 含义 |
|---|---:|---|
| `nal_units` | 全部 | 找到的 NAL 数 |
| `sps` | 7 | Sequence Parameter Set 数量 |
| `pps` | 8 | Picture Parameter Set 数量 |
| `idr` | 5 | 独立解码关键图像数量 |

### `inspect_annexb`

```cpp
AnnexBStats inspect_annexb(const uint8_t* data, std::size_t size);
```

扫描 3 字节 `00 00 01` 和 4 字节 `00 00 00 01` 起始码，再读 NAL header 低 5 bit。`data == nullptr` 返回全零统计。

这是结构检查器，不验证码流语法、参考关系、宏块内容或图像质量；完整正确性仍由 FFmpeg 全量解码验证。

私有 `start_code_length()` 只负责在某个 offset 判断起始码长度并做边界保护。

## 13. MPP 配置与编码包类型

位置：[`mpp_h264_encoder.h`](../include/rkstream/mpp_h264_encoder.h)

### `MppEncoderConfig`

| 字段 | 含义 | 当前生产值 |
|---|---|---:|
| `width` / `height` | 可见输入尺寸 | 1632×1224 |
| `source_stride` | 相机源行跨度 | 1632 |
| `fps` | 编码时间基准/码控帧率 | 24 |
| `bitrate` | 目标 bit/s | 4,000,000 |
| `gop` | IDR 间隔（帧） | 48 |

### `EncodedPacket`

| 字段 | 谁填写 | 用途 |
|---|---|---|
| `bytes` | MPP wrapper | 一个 H.264 access unit 的独立副本 |
| `pts` | MPP 透传调用者输入 | RTSP 时间轴；当前为 driver timestamp 微秒 |
| `encoded_sequence` | 应用编码线程 | 检测 packet queue 丢包 |
| `source_sequence` | 应用编码线程 | 对照来源 V4L2 帧序号 |
| `source_dequeue_timestamp_us` | 应用编码线程 | 内部 pipeline 延迟起点 |
| `encoded_timestamp_us` | 应用编码线程 | packet queue 排队延迟起点 |
| `keyframe` | 应用通过 Annex-B 检查 | IDR 重同步决策 |

`bytes` 自有内存，可安全跨线程。两个 sequence 分属不同层：frame queue 主动丢帧时 source sequence 可有缺口，但 encoded sequence 仍连续；packet queue 丢包时网络线程看到 encoded sequence 缺口。

### `MppEncoderInfo`

| 字段 | 含义 |
|---|---|
| `horizontal_stride` | MPP 输入水平 stride |
| `vertical_stride` | MPP 输入垂直 stride |
| `input_buffer_bytes` | MPP 输入 DMA buffer 大小 |
| `header_bytes` | SPS/PPS header 字节数 |

## 14. `MppH264Encoder`：Rockchip H.264 编码器

位置：[`mpp_h264_encoder.h`](../include/rkstream/mpp_h264_encoder.h)、[`mpp_h264_encoder.cpp`](../src/mpp_h264_encoder.cpp)

### 设计方式

类使用 PImpl：头文件只暴露 `class Impl; std::unique_ptr<Impl>`，避免调用者包含大量 Rockchip MPP C 头文件，也把第三方类型限制在 `.cpp`。

对象不可复制，预期由一个编码线程串行调用。

### `open`

```cpp
bool open(const MppEncoderConfig& config, std::string* error);
```

验证：尺寸、fps、bitrate、gop 非零，`source_stride >= width`。

内部步骤：

1. 水平/垂直 stride 向上 16 对齐；
2. 计算 `input_buffer_bytes = h_stride × v_stride × 3 / 2` 并检查溢出；
3. `mpp_create`；
4. `mpp_init(... MPP_CTX_ENC, MPP_VIDEO_CodingAVC)`；
5. `mpp_enc_cfg_init`；
6. 设置 prep、CBR、fps、GOP、H.264 High Profile/CABAC；
7. `MPP_ENC_SET_CFG`；
8. 分配长期复用的 input/output MppBuffer；
9. 用真实 output buffer 包装 header packet；
10. `MPP_ENC_GET_HDR_SYNC` 获取 SPS/PPS；
11. header 为空则失败。

码率配置采用 target 的 90%/110% 作为 min/max。`open()` 可重复调用，开头会先 `close()` 旧资源。

### `encode_nv12`

```cpp
bool encode_nv12(
    const std::byte* data,
    std::size_t size,
    int64_t pts,
    EncodedPacket* packet,
    std::string* error);
```

CPU-copy 路径：

1. 验证 encoder/data/output 状态；
2. 检查源至少有 `source_stride × height × 3/2` 字节；
3. `mpp_buffer_sync_begin`；
4. 清零整个目标对齐 buffer；
5. Y 平面逐行复制 `width` 字节；
6. 从源 `source_stride × height` 找 UV；
7. 从目标 `h_stride × v_stride` 找 UV；
8. UV 平面逐行复制 `width` 字节；
9. `mpp_buffer_sync_end`；
10. 调用 `encode_prepared_nv12()`。

为什么只复制 `width` 而不是 source stride：目标 padding 已清零，可见像素才是有效内容，不能把源 padding 当图像。

### `encode_prepared_nv12`

```cpp
bool encode_prepared_nv12(
    int64_t pts,
    EncodedPacket* packet,
    std::string* error);
```

DMA/RGA 路径入口。调用者必须保证 encoder 的 input MppBuffer 已由 RGA 同步写完且布局符合 `info()`。

内部流程：

1. 创建 `MppFrame`；
2. 设置 width/height/h_stride/v_stride/NV12/PTS；
3. 绑定长期 input buffer；
4. 用长期 output buffer 创建长度清零的 `MppPacket`；
5. 通过 `KEY_OUTPUT_PACKET` 放入 frame metadata；
6. `encode_put_frame`；
7. 释放 frame 描述符；
8. `encode_get_packet`；
9. 将压缩字节复制进 `EncodedPacket::bytes`；
10. 读取 packet PTS 并释放 MppPacket 描述符。

函数返回时 `EncodedPacket` 已不依赖 MPP output buffer。

### `header`

```cpp
const std::vector<uint8_t>& header() const;
```

返回 SPS/PPS 的借用引用。RTSP publisher 在 open 时复制它；不要在 encoder close 后继续持有引用。

### `info`

返回实际对齐布局的借用引用，RGA 用它构造目标 `rga_buffer_t`。

### `input_dmabuf_fd`

```cpp
int input_dmabuf_fd() const;
```

返回 MPP input buffer 导出的借用 fd；未初始化返回 `-1`。RGA 可导入但不得 close。

### `close`

清空 header/info，依次 `mpp_buffer_put(input/output)`、`mpp_enc_cfg_deinit`、`mpp_destroy`，最后重置 config。允许重复调用，`Impl` 析构也会调用。

### `.cpp` 内部辅助函数

| 函数 | 作用 |
|---|---|
| `align_up` | MPP 内部固定按 16 对齐 |
| `mpp_error` | 把 MPP_RET 组成可读错误字符串 |
| `set_config` | 统一 `mpp_enc_cfg_set_s32` 和错误处理 |

## 15. `RgaNv12Aligner`：RGA 布局整理器

位置：[`rga_nv12_aligner.h`](../include/rkstream/rga_nv12_aligner.h)、[`rga_nv12_aligner.cpp`](../src/rga_nv12_aligner.cpp)

### 类职责

把相机的 NV12 DMA-BUF 可见区域同步复制到 MPP 对齐 DMA-BUF。它消除 CPU 原始帧 copy，但仍有一次 RGA 硬件 copy。

同样使用 PImpl 隔离 librga 类型。

### `open`

```cpp
bool open(
    const CaptureInfo& capture_info,
    const std::vector<DmabufBufferInfo>& source_buffers,
    int destination_fd,
    const MppEncoderInfo& destination_info,
    std::string* error);
```

输入 fd 全部为借用。

内部步骤：

1. `importbuffer_fd` 导入 MPP 目标 fd；
2. 用可见宽高 + MPP h/v stride 包装目标；
3. 遍历所有相机 buffers，逐个 import fd；
4. 按 `source.index` 放入稠密数组；
5. 用可见宽高 + camera bytes_per_line/source height 包装源；
6. 检查 index 无缺失；
7. 对每个 source→destination 组合执行 `imcheck_t`。

把 import 放在初始化阶段，避免每帧重复导入 fd。

### `copy`

```cpp
bool copy(uint32_t source_index, std::string* error);
```

验证 index 后调用同步 `imcopy(source, destination, 1)`。成功返回时 RGA 已不再读取源 buffer，所以此后可以安全 `V4L2DmabufCapture::release()`。

若未来改为异步 RGA，必须引入 fence/synchronization，不能继续假设函数返回即完成。

### `close`

调用 `releasebuffer_handle` 撤销全部源/目标 RGA import。它不关闭 fd；源 fd 由 capture 拥有，目标 fd 由 encoder 拥有。

## 16. `KeyframeResync`：编码包缺口恢复

位置：[`keyframe_resync.h`](../include/rkstream/keyframe_resync.h)

### `KeyframeResyncDecision`

| 字段 | 含义 |
|---|---|
| `accept` | 当前包是否可发送 |
| `gap_detected` | 当前 sequence 与前一包是否不连续 |

### 初始状态

```text
have_previous_ = false
waiting_for_keyframe_ = true
previous_sequence_ = 0
```

首次包必须是关键帧才接收。

### `observe`

```cpp
KeyframeResyncDecision observe(uint64_t sequence, bool keyframe);
```

逻辑：

1. 已有前一包且 `sequence != previous + 1`，判定 gap；
2. gap 后进入 `waiting_for_keyframe_`；
3. 更新 previous sequence；
4. 等待期间非 keyframe 返回 `accept=false`；
5. keyframe 到来后清除等待并返回 `accept=true`。

### `waiting_for_keyframe`

只读返回当前状态，主要用于测试/诊断。类不检查 H.264 bytes；调用者必须先正确设置 `EncodedPacket::keyframe`。

## 17. RTSP 配置与 `RtspPublisher`

位置：[`rtsp_publisher.h`](../include/rkstream/rtsp_publisher.h)、[`rtsp_publisher.cpp`](../src/rtsp_publisher.cpp)

### `RtspPublisherConfig`

| 字段 | 含义 |
|---|---|
| `url` | 发布 URL，例如 `rtsp://127.0.0.1:8554/live` |
| `width` / `height` | H.264 可见尺寸 |
| `fps` | 时间轴每帧名义 duration |

### 类职责

接收已经编码好的 H.264 access units，通过 FFmpeg libavformat 的 RTSP muxer 发布。它不调用编码器，MediaMTX 才是监听客户端的 RTSP server。

类使用 PImpl 隔离 FFmpeg C 类型，不可复制，预期只在网络线程使用。

### `open`

```cpp
bool open(
    const RtspPublisherConfig& config,
    const std::vector<uint8_t>& h264_header,
    std::string* error);
```

要求 URL/尺寸/fps/header 有效。

内部步骤：

1. `avformat_alloc_output_context2(..., "rtsp", url)`；
2. `avformat_new_stream`；
3. 设置 `time_base = 1/90000`、平均帧率；
4. 填 H.264 codec parameters、宽高、YUV420P；
5. 分配 padded extradata 并复制 SPS/PPS header；
6. 设置 `rtsp_transport=tcp`、`muxdelay=0`、`rw_timeout=5s`；
7. 需要时 `avio_open2`；
8. `avformat_write_header`。

任一步失败都会 `close()` 部分状态。

### `write`

```cpp
bool write(const EncodedPacket& packet, std::string* error);
```

同步网络写入一个 access unit：

1. 第一包 PTS 记录为 `first_pts_us_`；
2. 后续 PTS 转为从 0 开始的相对微秒；
3. 分配 AVPacket 并复制压缩字节；
4. 微秒 rescale 到 90 kHz stream time base；
5. 设置 DTS、duration、key flag；
6. 若时间戳不递增，最小修正为 `last_pts + duration`；
7. `av_interleaved_write_frame`；
8. 释放 AVPacket。

这里复制的是压缩包，不是原始约 3 MB NV12。网络阻塞会停在该函数，所以它必须位于 network thread。

### `close`

有 header 时先 `av_write_trailer`，再 `avio_closep`，最后 `avformat_free_context`，并重置时间状态。允许重复调用。

### 内部状态

| 成员 | 含义 |
|---|---|
| `format_context_` | FFmpeg 输出上下文所有权 |
| `stream_` | context 内部借用 stream 指针 |
| `header_written_` | 是否应写 trailer |
| `have_first_pts_` / `first_pts_us_` | PTS 归零基准 |
| `last_pts_` | 强制单调递增检查 |

私有 `ffmpeg_error()` 使用 `av_strerror` 把负错误码转为可读文本。

## 18. 典型调用组合

### CPU-copy 编码

```cpp
V4L2Capture capture;
capture.open(config, &error);
capture.start(&error);

MppH264Encoder encoder;
encoder.open(mpp_config, &error);

Frame frame;
capture.capture(&frame, 500, &error);
EncodedPacket packet;
encoder.encode_nv12(frame.data.data(), frame.data.size(),
                    frame.driver_timestamp_us, &packet, &error);
```

### DMA-BUF + RGA 编码

```cpp
V4L2DmabufCapture capture;
capture.open(config, &error);

MppH264Encoder encoder;
encoder.open(mpp_config, &error);

RgaNv12Aligner rga;
rga.open(capture.info(), capture.buffers(),
         encoder.input_dmabuf_fd(), encoder.info(), &error);

capture.start(&error);
DmabufFrame lease;
capture.capture(&lease, 500, &error);
rga.copy(lease.buffer_index, &error);
capture.release(lease, &error);  // RGA 同步完成后立即归还源

EncodedPacket packet;
encoder.encode_prepared_nv12(lease.driver_timestamp_us, &packet, &error);
```

实际程序必须在每个失败分支也 release lease，不能只照搬上面的成功路径示例。

### RTSP 发布

```cpp
RtspPublisher publisher;
publisher.open(rtsp_config, encoder.header(), &error);

KeyframeResync resync;
auto decision = resync.observe(packet.encoded_sequence, packet.keyframe);
if (decision.accept) {
    publisher.write(packet, &error);
}
```

## 19. 资源释放总表

| 资源 | 创建者 | 释放函数 | 释放线程/阶段 |
|---|---|---|---|
| V4L2 device fd | capture `open` | `close(fd)` | pipeline 全部线程结束后 |
| V4L2 MMAP | `QUERYBUF/mmap` | `munmap` | `V4L2Capture::close` |
| EXPBUF fd | `VIDIOC_EXPBUF` | `close(fd)` | `V4L2DmabufCapture::close` |
| DMA lease | `capture/DQBUF` | `release/QBUF` | encode、drop、reject 或 drain 路径 |
| MPP context | `mpp_create/init` | `mpp_destroy` | encoder close |
| MPP buffers | `mpp_buffer_get` | `mpp_buffer_put` | encoder close |
| RGA handles | `importbuffer_fd` | `releasebuffer_handle` | RGA close |
| FFmpeg context | alloc output context | free context | publisher close |
| AVPacket | `av_packet_alloc` | `av_packet_free` | 每次 write 内部 |
| Frame bytes | `std::vector` | 析构自动释放 | frame 消费/丢弃后 |
| Encoded bytes | `std::vector` | 析构自动释放 | packet 消费/丢弃后 |

## 20. 调试时从哪个函数开始

| 症状 | 第一检查函数 | 后续检查 |
|---|---|---|
| 摄像头节点打不开 | `V4L2Capture::open` | QUERYCAP、格式、设备占用 |
| 格式被改 | `open` 的 S_FMT 返回检查 | `CaptureInfo` 日志 |
| 采集超时 | `capture` 的 poll/DQBUF | 摄像头、slot 是否被 lease 占尽 |
| DMA 跑一会停帧 | `V4L2DmabufCapture::release` | `captured == released` |
| 颜色/下半画面错误 | `audit_nv12_layout` | RGA source/destination stride 和 UV offset |
| MPP 初始化失败 | `MppH264Encoder::open` | cfg key、buffer、GET_HDR_SYNC |
| H.264 无 SPS/PPS | `open` header 获取 | output packet 是否绑定真实 buffer |
| RTSP 客户端花屏 | `KeyframeResync::observe` | packet gap、IDR 周期 |
| RTSP 无法连接 | `RtspPublisher::open` | MediaMTX 端口与日志 |
| 延迟持续增长 | `BoundedLatestQueue` 指标 | queue capacity、drop-oldest、消费者耗时 |

---

这份手册解释单个类型与函数。下一步请打开 [《可执行程序与脚本走读》](PROGRAM_WALKTHROUGH.md)，把这些 API 放回六个可执行程序的真实调用顺序中理解。
