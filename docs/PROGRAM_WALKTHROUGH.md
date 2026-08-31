# 可执行程序、线程、脚本与测试走读

本文从 `main()` 出发解释项目如何组合底层类。单个类和函数的参数、返回值、所有权请查 [《类与函数源码参考手册》](CODE_REFERENCE.md)，概念和学习顺序请查 [《源码学习导读》](LEARNING_GUIDE.md)。

## 1. 六个可执行程序分别解决什么问题

项目按“先分层证明，再组合”的方式演进，没有把所有逻辑都塞进一个大程序。

| Target | 源码 | 硬件路径 | 是否网络 | 目的 |
|---|---|---|---|---|
| `capture_probe` | [`capture_probe.cpp`](../apps/capture_probe.cpp) | V4L2 + CPU-copy | 否 | 证明采集格式、FPS、有界背压 |
| `mpp_file_encode` | [`mpp_file_encode.cpp`](../apps/mpp_file_encode.cpp) | V4L2 copy + MPP | 否 | 证明 H.264 文件编码基线 |
| `dmabuf_probe` | [`dmabuf_probe.cpp`](../apps/dmabuf_probe.cpp) | 原始 ioctl + EXPBUF + RGA | 否 | 单帧审计 fd 和布局可行性 |
| `dmabuf_mpp_encode` | [`dmabuf_mpp_encode.cpp`](../apps/dmabuf_mpp_encode.cpp) | V4L2 lease + RGA + MPP | 否 | 持续 DMA 编码和 CPU 基线对照 |
| `rtsp_stream` | [`rtsp_stream.cpp`](../apps/rtsp_stream.cpp) | CPU-copy 三线程 | 是 | RTSP 基线和 fallback |
| `dmabuf_rtsp_stream` | [`dmabuf_rtsp_stream.cpp`](../apps/dmabuf_rtsp_stream.cpp) | DMA-BUF/RGA 三线程 | 是 | 当前推荐优化主链路 |

理解顺序应与表格从上到下一致。

## 2. 多个程序共有的辅助写法

这些辅助结构放在各 `.cpp` 的匿名 namespace 中，只服务当前可执行文件，不属于公共 API。

### `Options`

每个程序定义自己的命令行配置结构，先设置可工作的板端默认值，再由 `parse_options()` 覆盖。

好处：

- 参数解析和业务流程分离；
- `main()` 拿到的是已经类型化、验证过的值；
- 测试脚本可以只覆盖需要变化的少数参数。

### `parse_u32`

典型签名：

```cpp
uint32_t parse_u32(const char* value, const char* option);
```

内部用 `std::stoul`，同时检查：

- 整个字符串是否都被消费；
- 是否允许 0；
- 是否超过 `uint32_t`；
- 错误时把选项名带入 `std::invalid_argument`。

异常只在最外层 `main()` 捕获并打印 usage。业务线程不会靠抛异常传硬件错误。

### `parse_options`

逐个读取 `argv`：

- `--help` 打印帮助并正常退出；
- 需要值的选项先检查后面是否还有参数；
- 未知选项直接失败；
- 解析完再做跨字段校验，例如 duration/queue capacity 必须大于 0。

### `monotonic_us`

将 `std::chrono::steady_clock` 转为微秒整数。它用于测量耗时，系统时间被 NTP 或用户调整时不会倒退。

### `update_max`

使用 `compare_exchange_weak` 更新原子最大值：

```text
读取 previous
while value 更大且 CAS 未成功
    previous 被 CAS 更新为最新值
```

统计字段采用 `memory_order_relaxed`，因为它们不承担业务同步，只要求单个数值原子读写。

### `timeval_seconds`

将 `getrusage()` 返回的秒+微秒转为 `double seconds`，用于计算进程 user/system CPU 时间。

### 信号处理

RTSP 和采集程序的 signal handler 只写：

```cpp
volatile std::sig_atomic_t g_stop_requested;
```

信号上下文不能安全调用 `std::cout`、加 mutex 或直接销毁复杂对象。真正停机由普通线程循环观察 flag 后完成。

### `SharedError`

三线程程序用 mutex 保护一个字符串，并且只保存第一个错误：

```cpp
void set(const std::string& value);
std::string get() const;
```

第一个错误通常是根因，后续的 queue close/network stop 多是连锁反应。

## 3. CMake 如何组织 target

位置：[`CMakeLists.txt`](../CMakeLists.txt)

### 基础目标

```text
rkstream_capture
  ├─ pixel_format.cpp
  ├─ v4l2_capture.cpp
  └─ v4l2_dmabuf_capture.cpp

capture_probe → rkstream_capture
```

这部分只依赖 Linux/Threads，主机也能配置和编译。

### MPP 条件目标

找到 `rk_mpi.h` 和 `librockchip_mpp` 后：

```text
rkstream_mpp
  ├─ mpp_h264_encoder.cpp
  └─ annexb.cpp

mpp_file_encode → rkstream_capture + rkstream_mpp
```

### FFmpeg 条件目标

PkgConfig 找到 `libavformat/libavcodec/libavutil` 后：

```text
rkstream_rtsp → rtsp_publisher.cpp
rtsp_stream   → capture + mpp + rtsp + threads
```

### RGA 条件目标

同时找到 MPP 和 RGA 后：

```text
rkstream_rga       → rga_nv12_aligner.cpp
dmabuf_probe       → MPP + RGA（底层审计工具）
dmabuf_mpp_encode  → capture + mpp + rga
dmabuf_rtsp_stream → capture + mpp + rga + rtsp + threads
```

主机缺少板端库时硬件 target 会明确 `skipped`；这不是硬件测试 PASS。RK3588 必须运行 `build_board.sh` 实际链接。

## 4. `capture_probe` 走读

位置：[`apps/capture_probe.cpp`](../apps/capture_probe.cpp)

### 4.1 目标

只验证采集与队列，不保存画面、不编码、不联网。它同时支持：

- MIPI NV12；
- USB YUYV；
- USB MJPEG 压缩帧采集。

### 4.2 `Options`

| 字段 | 默认值 | 说明 |
|---|---:|---|
| `device` | `/dev/video-camera0` | V4L2 节点 |
| `format` | `nv12` | 交给 `parse_pixel_format` |
| `width/height` | 1632×1224 | 请求尺寸 |
| `fps` | 30 | 请求帧率 |
| `duration_seconds` | 10 | 运行时长 |
| `queue_capacity` | 4 | CPU Frame 队列容量 |
| `consumer_delay_ms` | 0 | 人为减慢消费者的压力参数 |

### 4.3 `Counters`

| 字段 | 解释 |
|---|---|
| `captured` / `consumed` | 生产/消费帧数 |
| `driver_sequence_gaps` | V4L2 sequence 缺失帧数 |
| `poll_timeouts` | 500 ms capture timeout 次数 |
| `bytes_captured` | 所有 Frame data 总字节 |
| `max_queue_latency_us` | dequeue 到 consumer pop 的最大时间 |
| `checksum` | 三个采样字节异或，防止消费逻辑完全空操作 |
| first/last dequeue timestamp | 计算应用 active capture FPS |
| first/last driver timestamp | 计算驱动 timestamp FPS |

### 4.4 `main()` 调用顺序

```text
parse options/format
  → 安装 SIGINT/SIGTERM
  → V4L2Capture::open
  → V4L2Capture::start
  → 打印 CAPTURE_READY（实际协商值）
  → 创建 BoundedLatestQueue<Frame>
  → 启动 consumer
  → 启动 producer
  → 主线程每秒打印 STATUS
  → duration/signal/error
  → join producer
  → close queue
  → join consumer
  → capture.close
  → 打印 SUMMARY
```

### 4.5 Consumer thread

每次 `queue.pop()`：

1. 计算队列等待时间；
2. 读取 data 首/中/尾三个字节形成轻量 checksum；
3. `consumed++`；
4. 若设置 `consumer_delay_ms`，sleep 模拟慢下游。

`pop()` 返回 nullopt 后退出。

### 4.6 Producer thread

循环调用 `capture.capture(frame, 500ms)`：

- timeout：只计数继续；
- interrupted：继续，让外层停止条件决定；
- fatal：设置失败并退出；
- frame：检查 sequence、记录时间/字节/数量，移动进队列。

生产结束后设置 `running=false` 并 `queue.close()` 唤醒消费者。

### 4.7 三个 FPS 字段

| 字段 | 公式 | 边界 |
|---|---|---|
| `capture_fps` | captured / 整个程序 elapsed | 包含初始化/收尾少量时间 |
| `active_capture_fps` | `(N-1)/(last dequeue-first dequeue)` | 更接近应用实际取帧节奏 |
| `driver_timestamp_fps` | `(N-1)/(last driver-first driver)` | 更接近驱动产生节奏 |

### 4.8 如何做背压实验

消费者延迟 100 ms 时，摄像头仍约 24 FPS，而消费者最多约 10 FPS：

```bash
./capture_probe ... --queue-capacity 4 --consumer-delay-ms 100
```

预期：`drop_oldest` 增加、queue 有界、driver gaps 仍可能为 0。这证明应用主动丢帧与驱动漏帧是两件事。

## 5. `mpp_file_encode` 走读

位置：[`apps/mpp_file_encode.cpp`](../apps/mpp_file_encode.cpp)

### 5.1 目标

CPU-copy 文件编码基线：

```text
OV8858 → V4L2Capture/Frame → MppH264Encoder::encode_nv12 → .h264
```

先证明裸码流可解析/全量解码，避免 RTSP 问题和编码问题互相干扰。

### 5.2 主要辅助函数

- `parse_u32`：要求所有数值参数大于 0；
- `parse_options`：处理 device/output/frames/fps/bitrate/gop；
- `merge_stats`：把每个 packet 的 NAL 统计累加到总统计；
- `timeval_seconds`：转换 CPU usage 时间。

### 5.3 主流程

```text
V4L2Capture open/start（固定请求 OV8858 NV12 1632×1224）
  → 用 CaptureInfo 构造 MppEncoderConfig
  → MppH264Encoder::open
  → 打开 output（二进制覆盖写）
  → 先写 encoder.header() 的 SPS/PPS
  → 循环直到 encoded_frames == requested frames
       capture Frame
       检查 driver sequence
       encode_nv12(Frame::data, driver timestamp)
       inspect_annexb(packet)
       写 packet.bytes
       统计 encode latency/bytes
  → close capture/encoder/output
  → 统计 FPS/bitrate/CPU/RSS/NAL
  → ENCODE_SUMMARY
```

### 5.4 PASS 判断

程序至少要求达到请求帧数、输出流有效，并汇报 SPS/PPS/IDR。脚本随后使用独立 `ffprobe` 和 FFmpeg 全量解码做更强验证。

### 5.5 为什么 PTS 使用 driver timestamp

传入 `encode_nv12` 的 PTS 来自 `frame.driver_timestamp_us`，保留真实采集时间节奏；MPP packet 再透传该值。文件 Annex-B 本身不保存容器时间轴，但同一个 encoder 类也服务 RTSP。

## 6. `dmabuf_probe` 走读

位置：[`apps/dmabuf_probe.cpp`](../apps/dmabuf_probe.cpp)

### 6.1 为什么这个程序没有复用全部封装类

它是“硬件能力审计探针”，刻意直接展示 ioctl、EXPBUF、RGA import 和 checksum，方便定位究竟是哪一层不可用。生产链路则使用封装后的类。

### 6.2 `ExportedBuffer`

| 字段 | 作用 |
|---|---|
| `index` | V4L2 slot |
| `length` | buffer 长度 |
| `fd` | EXPBUF fd |
| `rga_handle` | RGA import handle |

### 6.3 辅助函数

| 函数 | 作用 |
|---|---|
| `ioctl_retry` | EINTR 重试 |
| `errno_message` | errno 文本化 |
| `fourcc` | fourcc 转四字符 |
| `update_checksum` | FNV-1a 风格逐字节 hash |
| `visible_nv12_checksum` | 按 stride 跳过 padding，只检查可见 Y/UV |
| `release_buffers` | 撤销 RGA import 并关闭 EXPBUF fd |
| `queue_buffer` | 直接构造 multi-planar QBUF |

### 6.4 主流程

1. 打开相机并用 scope cleanup 保证异常路径释放；
2. QUERYCAP、S_FMT NV12 1632×1224；
3. 用 `audit_nv12_layout()` 打印源/MPP offset 和兼容性；
4. REQBUFS/QUERYBUF/EXPBUF 全部 slot；
5. 每个 fd 导入 RGA；
6. 分配 MPP destination buffer 并导入 RGA；
7. QBUF all、STREAMON、poll/DQBUF 一帧；
8. mmap/checksum 源可见 NV12；
9. RGA 同步 copy；
10. checksum 目标可见 NV12；
11. 比较 checksum、导入数、布局结果并打印 `DMABUF_AUDIT`。

checksum 相同只能证明当前可见字节复制一致，不是色彩/画质测试。

## 7. `dmabuf_mpp_encode` 走读

位置：[`apps/dmabuf_mpp_encode.cpp`](../apps/dmabuf_mpp_encode.cpp)

### 7.1 目标

把单帧审计扩展为持续文件编码：

```text
V4L2DmabufCapture lease
  → RgaNv12Aligner::copy
  → capture.release
  → MppH264Encoder::encode_prepared_nv12
  → .h264
```

### 7.2 初始化顺序

1. `capture.open`：先取得真实 CaptureInfo 和导出 buffers；
2. `encoder.open`：产生目标对齐 buffer；
3. 检查 `encoder.input_dmabuf_fd() >= 0`；
4. `rga.open(source buffers, destination fd)`；
5. `capture.start`；
6. 打开输出文件并先写 header。

RGA 必须在 capture 和 encoder 都 open 后才能初始化，因为它同时需要两边 fd。

### 7.3 每帧顺序

```text
capture.capture → 得到 lease
  → RGA copy（计时）
  → 无论 RGA 成败都 release lease
  → RGA 成功才 MPP encode（计时）
  → inspect Annex-B
  → 写文件
```

为什么先 release 再 MPP：RGA 已经把像素复制到 encoder 自己的 input buffer，MPP 不再读取相机源 slot，尽早 QBUF 能让摄像头继续工作。

### 7.4 关键不变量

```text
captured leases == released leases
RGA completed frames == encoded frames（正常路径）
driver gaps == 0（稳定基线期望）
```

`DMABUF_ENCODE_SUMMARY` 同时报告 avg/max RGA、MPP、总处理时间、CPU、RSS、码流结构和 release 结果。

## 8. `rtsp_stream`：CPU-copy 三线程基线

位置：[`apps/rtsp_stream.cpp`](../apps/rtsp_stream.cpp)

### 8.1 Options

| 字段 | 默认值 |
|---|---:|
| device | `/dev/video-camera0` |
| url | `rtsp://127.0.0.1:8554/live` |
| duration | 20 s |
| encoder fps | 24 |
| bitrate | 4 Mbps |
| GOP | 48 |
| frame queue | 4 |
| packet queue | 8 |

### 8.2 Counters 分类

| 分类 | 字段 |
|---|---|
| 数量 | captured/encoded/published/published_bytes |
| 采集 | capture_timeouts/driver_sequence_gaps |
| 编码 | encode_total_us/encode_max_us |
| 网络 | network_total_us/network_max_us |
| 排队 | frame/packet queue max latency |
| 内部总延迟 | pipeline_total_us/pipeline_max_us |
| H.264 恢复 | network_gap_events/packets_skipped_until_idr |

### 8.3 启动屏障

主线程先启动 network thread。网络线程尝试 `publisher.open()`，通过 `promise<bool>` 把结果交给主线程：

```text
network: open RTSP → promise.set_value(opened)
main: future.get()
```

publisher 未准备好时，不启动 capture/encode 热路径。

### 8.4 Network thread

```text
RtspPublisher::open(header)
  → packet_queue.pop loop
  → 计算 packet queue latency
  → KeyframeResync::observe
  → reject 非可解码 P frame 或 publisher.write
  → 统计 network/pipeline latency、bytes、published
  → publisher.close
```

### 8.5 Encode thread

```text
frame_queue.pop
  → 计算 frame queue latency
  → encoder.encode_nv12（包含 CPU layout copy + MPP）
  → 设置 encoded/source sequence、时间戳
  → inspect_annexb 判断 IDR
  → packet_queue.push
  → 退出时 packet_queue.close
```

### 8.6 Capture thread

```text
capture.capture(Frame)
  → 检查 driver sequence
  → captured++
  → frame_queue.push(move Frame)
  → 退出时 stop=true、frame_queue.close
```

Frame 自己拥有 data，drop-oldest 不需要显式 QBUF。

### 8.7 停机

```text
stop=true
join capture
close frame queue
join encoder
close packet queue
join network
capture.close
encoder.close
```

### 8.8 内部延迟边界

`avg_internal_pipeline_ms` 从 `V4L2Capture::capture()` 完成 copy 后的 dequeue timestamp 算到 RTSP write 完成。它不包括传感器曝光、网络在途、客户端缓存和显示。

## 9. `dmabuf_rtsp_stream`：推荐优化链路

位置：[`apps/dmabuf_rtsp_stream.cpp`](../apps/dmabuf_rtsp_stream.cpp)

### 9.1 与 CPU 基线的相同点

- 同样的 capture/encode/network 三线程；
- 同样的 MPP 参数；
- 同样的 packet queue 和 RTSP publisher；
- 同样的 KeyframeResync；
- 同样的时长、状态输出和资源指标。

这样对照差异主要来自原始帧路径。

### 9.2 与 CPU 基线的不同点

| CPU baseline | DMA optimized |
|---|---|
| `V4L2Capture` | `V4L2DmabufCapture` |
| queue 存 `Frame` | queue 存 `DmabufFrame lease` |
| frame queue 4 | lease queue 1（安全推导） |
| CPU 逐行复制到 MPP | RGA 硬件 copy 到 MPP |
| Frame 析构即可 | 每条路径必须 release/QBUF |

### 9.3 `Options::encoder_delay_ms`

仅用于背压诊断。延迟插在：

```text
RGA copy → release source lease → artificial sleep → MPP encode
```

故压力测试不会故意长期占住相机 buffer，而是模拟 RGA 后面的编码/下游变慢。

### 9.4 Counters 中新增的 lease/RGA 指标

| 字段 | 含义 |
|---|---|
| `leases_released` | 成功 QBUF 的全部 lease |
| `leases_dropped_oldest` | frame queue 满时被替换的 lease |
| `leases_drained` | 停机时从残留队列取回的 lease |
| `lease_release_failures` | QBUF/状态检查失败 |
| `rga_completed` | 成功硬件 copy 帧数 |
| `rga_total/max_us` | RGA 耗时 |
| `mpp_total/max_us` | 纯 MPP 调用耗时 |

### 9.5 `release_lease` lambda

所有 lease 归还统一进入该 lambda：

```cpp
bool release_lease(const DmabufFrame& frame, const char* context);
```

成功：`leases_released++`。

失败：

- `lease_release_failures++`；
- `SharedError` 保存上下文；
- `fatal=true`；
- `stop=true`。

上下文文本区分正常 RGA 后归还、RGA 失败、push reject、drop-oldest 和 drain。

### 9.6 初始化顺序

```text
capture.open
  → validate lease queue capacity
  → encoder.open / 检查 input DMA fd
  → rga.open
  → capture.start
  → 创建两个 bounded queues
  → 启动 network + 等 ready
  → 启动 encoder
  → 启动 capture
```

### 9.7 Encode thread ownership

一次循环的严格顺序：

1. pop lease；
2. 测量 frame queue latency；
3. `rga.copy(source_index)`；
4. 无论 copy 成败都 `release_lease()`；
5. copy/release 任一失败则停止；
6. 可选诊断 delay；
7. `encode_prepared_nv12`；
8. 设置 packet 元数据和 IDR；
9. push packet。

关键点：MPP 编码期间相机源 lease 已经归还，因为 MPP 读取自己的目标 buffer。

### 9.8 Capture thread ownership

成功 DQBUF 后当前线程立即拥有 lease。push 前复制一份轻量 `current` 描述：

- push accepted：所有权进入 queue；
- push rejected：使用 `current` 归还；
- push drop-oldest：返回的 `dropped_value` 立即归还；
- 新值仍留在 queue，之后由 encode thread 归还。

### 9.9 停机 drain

encoder 可能因网络/MPP/RGA 错误先退出，此时 frame queue 里仍可能有 lease。主线程：

```text
join encoder
  → frame_queue.drain()
  → 对每个 remaining frame release_lease
  → 再 close capture
```

不能只让 `DmabufFrame` 析构，因为它只是整数描述，不会自动 QBUF。

### 9.10 最终 PASS 条件

```text
fatal == false
outstanding_leases = captured - released == 0
lease_release_failures == 0
```

`lease_balance=true` 是优化路径的硬条件。视频可播放但 lease 不平衡仍判 FAIL。

## 10. 输出日志如何分层阅读

### READY 行

只表示初始化完成，可开始测试：

- `CAPTURE_READY`
- `ENCODER_READY`
- `DMABUF_ENCODER_READY`
- `STREAM_READY`
- `DMABUF_STREAM_READY`

不能用 READY 代替完整运行 PASS。

### STATUS 行

每秒可观测快照，用于看程序是否推进、队列是否持续增长。它不是最终精确统计。

### SUMMARY 行

程序所有线程已结束并完成资源清理后输出：

- `SUMMARY`
- `ENCODE_SUMMARY`
- `DMABUF_ENCODE_SUMMARY`
- `STREAM_SUMMARY`
- `DMABUF_STREAM_SUMMARY`

自动化脚本应解析 SUMMARY，而不是页面或中途 STATUS。

## 11. 九个脚本逐一说明

### `build_board.sh`

位置：[`scripts/build_board.sh`](../scripts/build_board.sh)

1. 计算共享 source 根目录；
2. 固定板端本地 build 目录；
3. 拒绝非 aarch64；
4. CMake configure `RelWithDebInfo + BUILD_TESTING`；
5. 并行编译；
6. 实际运行 CTest；
7. 枚举成功生成的硬件 targets。

它不会在 NFS source tree 中生成构建文件。

### `probe_cameras.sh`

位置：[`scripts/probe_cameras.sh`](../scripts/probe_cameras.sh)

依次对 MIPI 和 USB 节点执行：

```text
v4l2-ctl --list-devices
v4l2-ctl -D
v4l2-ctl --list-formats-ext
```

用于确认当前节点映射、driver/API 和声明格式，不做应用 pipeline 测试。

### `run_capture_probe.sh`

位置：[`scripts/run_capture_probe.sh`](../scripts/run_capture_probe.sh)

通过 `CAMERA` 环境变量选择参数组：

| CAMERA | 节点/格式/尺寸 |
|---|---|
| `mipi` | `/dev/video-camera0` NV12 1632×1224 |
| `usb` | `/dev/video21` YUYV 640×480 |
| `usb-mjpeg` | `/dev/video21` MJPEG 1280×720 |

最后用 `exec` 替换 shell 为 `capture_probe`，信号和退出码直接透传。

### `run_mpp_file_encode.sh`

位置：[`scripts/run_mpp_file_encode.sh`](../scripts/run_mpp_file_encode.sh)

1. 检查 binary；
2. 编码默认 96 帧；
3. `ffprobe` 检查 codec/profile/width/height/pix_fmt；
4. FFmpeg 全量解码到 null；
5. 计算 H.264 SHA256。

输出位于 Build，不进入 Git。

### `run_dmabuf_probe.sh`

位置：[`scripts/run_dmabuf_probe.sh`](../scripts/run_dmabuf_probe.sh)

拒绝非 aarch64、检查 binary 和 camera node，然后只采一帧运行底层 DMA/RGA 审计，不保存画面。

### `run_encode_benchmark.sh`

位置：[`scripts/run_encode_benchmark.sh`](../scripts/run_encode_benchmark.sh)

同条件串行执行：

```text
mpp_file_encode（CPU）
  → ffprobe/decode/SHA
dmabuf_mpp_encode（DMA/RGA）
  → ffprobe/decode/SHA
  → 从两条 SUMMARY 提取指标并计算对照
```

运行前检查没有项目内其他采集/编码进程占用资源。

### `install_mediamtx.sh`

位置：[`scripts/install_mediamtx.sh`](../scripts/install_mediamtx.sh)

- 固定 MediaMTX 版本和 arm64 archive；
- 若已安装则直接返回；
- 缓存下载包；
- 校验固定 SHA256；
- 用临时目录解压；
- 只安装 binary/example config 到 `/home/cat/Build/tools`。

第三方 binary 不进入源码仓库。

### `run_rtsp_stream.sh`

位置：[`scripts/run_rtsp_stream.sh`](../scripts/run_rtsp_stream.sh)

CPU-copy 一键运行：

1. 检查 binary/MediaMTX/ffprobe/ffmpeg/端口；
2. 生成只启用 RTSP/TCP 的临时配置；
3. trap 注册所有子进程清理；
4. 启动 MediaMTX 并等待端口 LISTEN；
5. 启动 `rtsp_stream`；
6. 等待 `STREAM_READY`；
7. 并行启动 ffprobe 客户端；
8. 等 pipeline/probe；
9. 打印 SUMMARY 并传播失败状态。

### `run_dmabuf_rtsp_stream.sh`

位置：[`scripts/run_dmabuf_rtsp_stream.sh`](../scripts/run_dmabuf_rtsp_stream.sh)

当前推荐脚本。与 CPU 脚本相比：

- 启动 `dmabuf_rtsp_stream`；
- frame queue 固定为 1；
- 支持 `ENCODER_DELAY_MS` 压力参数；
- 同时运行 ffprobe 和 5 秒 FFmpeg decode；
- 最终必须同时满足进程状态、客户端状态、`result=PASS` 和 `lease_balance=true`。

## 12. 六个单元测试逐一说明

### `test_pixel_format`

位置：[`tests/test_pixel_format.cpp`](../tests/test_pixel_format.cpp)

验证名称大小写、别名、fourcc 字符串和非法格式异常。

### `test_bounded_latest_queue`

位置：[`tests/test_bounded_latest_queue.cpp`](../tests/test_bounded_latest_queue.cpp)

验证：

- 构造容量 0 失败；
- push/pop FIFO；
- 满时返回正确 dropped oldest；
- dropped/high watermark 指标；
- close 后 pop 行为；
- drain 转移残留项。

### `test_annexb`

位置：[`tests/test_annexb.cpp`](../tests/test_annexb.cpp)

构造小型字节数组验证 3/4 字节起始码及 SPS/PPS/IDR 计数，不需要真实编码器。

### `test_keyframe_resync`

位置：[`tests/test_keyframe_resync.cpp`](../tests/test_keyframe_resync.cpp)

覆盖：初始等待 IDR、连续包、sequence gap、gap 后跳过 P frame、IDR 恢复。

### `test_nv12_layout`

位置：[`tests/test_nv12_layout.cpp`](../tests/test_nv12_layout.cpp)

验证对齐函数、1632×1224 真实布局、兼容/不兼容条件和 buffer bytes。

### `test_dmabuf_queue_policy`

位置：[`tests/test_dmabuf_queue_policy.cpp`](../tests/test_dmabuf_queue_policy.cpp)

验证不同 camera buffer 数量对应的安全 queue 上限，以及 4-buffer/queue-1 合法、queue-2 非法。

### 单元测试边界

这些测试不调用：

- 真实 V4L2 ioctl；
- RGA hardware；
- MPP VPU；
- RTSP network。

所以 CTest 6/6 PASS 证明纯逻辑契约，通过板端脚本才能证明硬件集成。

## 13. 从日志反查源码

| 日志字段/错误 | 产生位置 |
|---|---|
| `driver_gaps` | capture thread 的 sequence delta |
| `frame_drop_oldest` | `BoundedLatestQueue<DmabufFrame/Frame>::push` |
| `packet_drop_oldest` | `BoundedLatestQueue<EncodedPacket>::push` |
| `skipped_until_idr` | network thread 的 `KeyframeResync` reject |
| `avg_rga_ms` | encode thread 包围 `rga.copy` |
| `avg_mpp_ms` | encode thread 包围 `encode_prepared_nv12` |
| `avg_network_ms` | network thread 包围 `publisher.write` |
| `avg_internal_pipeline_ms` | source dequeue 到 publisher.write 完成 |
| `lease_balance=false` | captured/released 总账不平 |
| `input DMA-BUF fd unavailable` | encoder 未提供可导入目标 fd |
| `RGA layout check failed` | `RgaNv12Aligner::open` 的 `imcheck_t` |
| `waiting/skipped IDR` | 编码包队列发生序号缺口 |

## 14. 一次帧在优化链路中的完整调用栈

```text
capture_thread
  V4L2DmabufCapture::capture
    poll
    VIDIOC_DQBUF
    queued_[index] = false
  frame_queue.push(DmabufFrame)

encoder_thread
  frame_queue.pop
  RgaNv12Aligner::copy(index)
    imcopy(sync=1)
  V4L2DmabufCapture::release
    VIDIOC_QBUF
    queued_[index] = true
  MppH264Encoder::encode_prepared_nv12
    mpp_frame_init/set
    mpp_packet_init_with_buffer
    encode_put_frame
    encode_get_packet
    copy compressed bytes
  inspect_annexb
  packet_queue.push(EncodedPacket)

network_thread
  packet_queue.pop
  KeyframeResync::observe
  RtspPublisher::write
    allocate AVPacket
    rescale PTS
    av_interleaved_write_frame
```

## 15. 建议你的实际阅读练习

### 第一遍：顺着成功路径

1. 从 `dmabuf_rtsp_stream::main` 初始化开始；
2. 跳到三个线程 lambda；
3. 每遇到类方法就查 `CODE_REFERENCE.md`；
4. 在纸上记录资源从谁转给谁。

### 第二遍：只找失败路径

对每一个 `return false`、`fatal=true`、`break` 回答：

- 当前有没有未归还 lease？
- 哪个 queue 会被 close？
- 哪个线程会被唤醒？
- 最终谁 join 谁？
- summary 是否能看到根因？

### 第三遍：对照 CPU 和 DMA 主程序

把 `rtsp_stream.cpp` 与 `dmabuf_rtsp_stream.cpp` 并排：

- 相同逻辑说明它们是公平基线；
- 不同逻辑集中在 Frame ownership、RGA 和 lease accounting；
- 用实测 CPU/RSS/latency 解释优化收益。

完成这三遍后，你应能脱离文档独立解释整个项目，而不仅会执行脚本。
