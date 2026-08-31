#include "rkstream/annexb.h"
#include "rkstream/bounded_latest_queue.h"
#include "rkstream/dmabuf_queue_policy.h"
#include "rkstream/keyframe_resync.h"
#include "rkstream/mpp_h264_encoder.h"
#include "rkstream/rga_nv12_aligner.h"
#include "rkstream/rtsp_publisher.h"
#include "rkstream/v4l2_dmabuf_capture.h"

#include <linux/videodev2.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>

/*
 * 本文件是项目的“优化版完整主链路”，建议读完各基础类后最后阅读。
 *
 * 数据流：
 *   capture thread                encoder thread                 network thread
 *   V4L2 DQBUF                    RGA DMA copy                    FFmpeg RTSP mux
 *        │                              │                              │
 *        └─ DmabufFrame lease ────────>├─ QBUF 归还相机              │
 *                                       ├─ MPP H.264 ─ EncodedPacket ─>┘
 *
 * 两个队列都有限长并采用 drop-oldest，但资源语义不同：
 *   1. frame_queue 中不是图像副本，而是相机 DMA-BUF 的 lease；任何退出/丢帧路径
 *      都必须 release，否则相机 slot 会永久减少，最终停止出帧。
 *   2. packet_queue 中是自有 vector 的压缩包，丢弃不会泄漏外部资源；但 H.264
 *      参考链会断裂，所以网络线程必须等下一张 IDR 再恢复发送。
 *
 * 这里保留较多统计字段，不只是为了展示 FPS，而是让“低延迟”“有界”“无 lease
 * 泄漏”都能由运行结果机械验证。
 */
namespace {

// 信号处理函数中只写 sig_atomic_t；iostream、mutex 等都不是 async-signal-safe。
volatile std::sig_atomic_t g_stop_requested = 0;

void handle_signal(int) {
    g_stop_requested = 1;
}

uint64_t monotonic_us() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void update_max(std::atomic<uint64_t>* target, uint64_t value) {
    // 多个工作线程可能同时更新指标；CAS 循环避免为简单最大值引入全局 mutex。
    uint64_t previous = target->load(std::memory_order_relaxed);
    while (previous < value &&
           !target->compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

double timeval_seconds(const timeval& value) {
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
}

struct Options {
    std::string device = "/dev/video-camera0";
    std::string url = "rtsp://127.0.0.1:8554/live";
    uint32_t duration_seconds = 20;
    uint32_t encoder_fps = 24;
    uint32_t bitrate = 4'000'000;
    uint32_t gop = 48;
    uint32_t frame_queue_capacity = 1;
    uint32_t packet_queue_capacity = 8;
    uint32_t encoder_delay_ms = 0;
};

uint32_t parse_u32(const char* value, const char* option, bool allow_zero = false) {
    const std::string text(value);
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(text, &consumed, 10);
    if (consumed != text.size() || (!allow_zero && parsed == 0) ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(std::string("invalid value for ") + option + ": " + text);
    }
    return static_cast<uint32_t>(parsed);
}

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --device PATH             OV8858 NV12 capture node\n"
        << "  --url URL                 RTSP publication URL\n"
        << "  --duration N              Run duration in seconds\n"
        << "  --fps N                   Encoder frame rate\n"
        << "  --bitrate N               Target bitrate in bits/s\n"
        << "  --gop N                   H.264 GOP length\n"
        << "  --frame-queue N           Lease queue; must leave two V4L2 buffers free\n"
        << "  --packet-queue N          Encoded packet queue capacity\n"
        << "  --encoder-delay-ms N      Diagnostic backpressure delay; default 0\n"
        << "  --help                     Show this help\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + argument);
        }
        const char* value = argv[++index];
        if (argument == "--device") {
            options.device = value;
        } else if (argument == "--url") {
            options.url = value;
        } else if (argument == "--duration") {
            options.duration_seconds = parse_u32(value, "--duration");
        } else if (argument == "--fps") {
            options.encoder_fps = parse_u32(value, "--fps");
        } else if (argument == "--bitrate") {
            options.bitrate = parse_u32(value, "--bitrate");
        } else if (argument == "--gop") {
            options.gop = parse_u32(value, "--gop");
        } else if (argument == "--frame-queue") {
            options.frame_queue_capacity = parse_u32(value, "--frame-queue");
        } else if (argument == "--packet-queue") {
            options.packet_queue_capacity = parse_u32(value, "--packet-queue");
        } else if (argument == "--encoder-delay-ms") {
            options.encoder_delay_ms = parse_u32(value, "--encoder-delay-ms", true);
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    return options;
}

struct Counters {
    // 所有字段使用 relaxed：它们只做统计，不承担线程间业务状态同步。
    std::atomic<uint64_t> captured{0};
    std::atomic<uint64_t> leases_released{0};
    std::atomic<uint64_t> leases_dropped_oldest{0};
    std::atomic<uint64_t> leases_drained{0};
    std::atomic<uint64_t> lease_release_failures{0};
    std::atomic<uint64_t> rga_completed{0};
    std::atomic<uint64_t> encoded{0};
    std::atomic<uint64_t> published{0};
    std::atomic<uint64_t> published_bytes{0};
    std::atomic<uint64_t> capture_timeouts{0};
    std::atomic<uint64_t> driver_sequence_gaps{0};
    std::atomic<uint64_t> rga_total_us{0};
    std::atomic<uint64_t> rga_max_us{0};
    std::atomic<uint64_t> mpp_total_us{0};
    std::atomic<uint64_t> mpp_max_us{0};
    std::atomic<uint64_t> network_total_us{0};
    std::atomic<uint64_t> network_max_us{0};
    std::atomic<uint64_t> frame_queue_max_latency_us{0};
    std::atomic<uint64_t> packet_queue_max_latency_us{0};
    std::atomic<uint64_t> pipeline_total_us{0};
    std::atomic<uint64_t> pipeline_max_us{0};
    std::atomic<uint64_t> network_gap_events{0};
    std::atomic<uint64_t> packets_skipped_until_idr{0};
};

struct SharedError {
    // 只保留第一个错误。它通常最接近根因，后续错误多为停机连锁反应。
    void set(const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (message.empty()) {
            message = value;
        }
    }

    std::string get() const {
        std::lock_guard<std::mutex> lock(mutex);
        return message;
    }

    mutable std::mutex mutex;
    std::string message;
};

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        // ---------- 初始化阶段：V4L2 -> MPP -> RGA -> STREAMON ----------
        // 顺序很重要：RGA 需要 capture 导出的源 fd 和 encoder 的目标 fd。
        rkstream::V4L2DmabufCapture capture;
        std::string error;
        const rkstream::CaptureConfig capture_config{
            options.device, 1632, 1224, 30, V4L2_PIX_FMT_NV12, 4,
        };
        if (!capture.open(capture_config, &error)) {
            std::cerr << "V4L2 DMA-BUF initialization failed: " << error << '\n';
            return 2;
        }
        const rkstream::CaptureInfo capture_info = capture.info();
        // encoder 在短暂 RGA 阶段持有 1 帧。队列外必须再为 RKISP 保留至少 2 个
        // buffer；4-buffer 相机因此只能使用容量为 1 的 lease queue。
        if (!rkstream::valid_dmabuf_lease_queue_capacity(
                capture_info.buffer_count, options.frame_queue_capacity)) {
            std::cerr << "frame queue must reserve one encoder lease and two driver buffers: queue="
                      << options.frame_queue_capacity
                      << " buffers=" << capture_info.buffer_count
                      << " max_queue="
                      << rkstream::max_dmabuf_lease_queue_capacity(
                             capture_info.buffer_count) << '\n';
            return 2;
        }

        rkstream::MppH264Encoder encoder;
        const rkstream::MppEncoderConfig encoder_config{
            capture_info.width, capture_info.height, capture_info.bytes_per_line,
            options.encoder_fps, options.bitrate, options.gop,
        };
        if (!encoder.open(encoder_config, &error) || encoder.input_dmabuf_fd() < 0) {
            std::cerr << "MPP initialization failed: "
                      << (error.empty() ? "input DMA-BUF fd unavailable" : error) << '\n';
            return 3;
        }

        rkstream::RgaNv12Aligner rga;
        if (!rga.open(capture_info, capture.buffers(), encoder.input_dmabuf_fd(),
                      encoder.info(), &error)) {
            std::cerr << "RGA initialization failed: " << error << '\n';
            return 3;
        }
        if (!capture.start(&error)) {
            std::cerr << "V4L2 stream start failed: " << error << '\n';
            return 2;
        }

        // frame_queue 连接 capture/encoder；packet_queue 连接 encoder/network。
        // 容量是固定上限，所以无论运行 20 秒还是 20 小时都不会无限积压。
        rkstream::BoundedLatestQueue<rkstream::DmabufFrame> frame_queue(
            options.frame_queue_capacity);
        rkstream::BoundedLatestQueue<rkstream::EncodedPacket> packet_queue(
            options.packet_queue_capacity);
        Counters counters;
        SharedError shared_error;
        std::atomic<bool> stop{false};
        std::atomic<bool> fatal{false};
        const auto started_at = std::chrono::steady_clock::now();
        const auto deadline = started_at + std::chrono::seconds(options.duration_seconds);
        rusage usage_start{};
        (void)getrusage(RUSAGE_SELF, &usage_start);

        // 所有 lease 归还都走同一个入口，统一计数和故障升级。
        // 正常 RGA、drop-oldest、push 被拒绝、停机 drain 四条路径都要调用它。
        auto release_lease = [&](const rkstream::DmabufFrame& frame,
                                 const char* context) -> bool {
            std::string release_error;
            if (!capture.release(frame, &release_error)) {
                counters.lease_release_failures.fetch_add(1, std::memory_order_relaxed);
                shared_error.set(std::string(context) + ": " + release_error);
                fatal.store(true, std::memory_order_relaxed);
                stop.store(true, std::memory_order_relaxed);
                return false;
            }
            counters.leases_released.fetch_add(1, std::memory_order_relaxed);
            return true;
        };

        // ---------- 网络线程 ----------
        // promise/future 是启动屏障：只有 RTSP publisher 确认连上 MediaMTX，主线程
        // 才启动其余流水线，避免“编码了很多帧但网络端从未成功初始化”。
        std::promise<bool> publisher_ready_promise;
        std::future<bool> publisher_ready = publisher_ready_promise.get_future();
        std::thread network_thread([&] {
            rkstream::RtspPublisher publisher;
            std::string thread_error;
            const bool opened = publisher.open(
                {options.url, capture_info.width, capture_info.height, options.encoder_fps},
                encoder.header(), &thread_error);
            if (!opened) {
                shared_error.set("RTSP initialization failed: " + thread_error);
                fatal.store(true, std::memory_order_relaxed);
                stop.store(true, std::memory_order_relaxed);
            }
            publisher_ready_promise.set_value(opened);
            if (!opened) {
                packet_queue.close();
                return;
            }

            // 队列丢包后不发送失去参考的 P 帧，等待下一个 IDR 再恢复。
            rkstream::KeyframeResync keyframe_resync;
            while (true) {
                auto packet = packet_queue.pop();
                if (!packet.has_value()) {
                    break;
                }
                update_max(&counters.packet_queue_max_latency_us,
                           monotonic_us() - packet->encoded_timestamp_us);
                const auto decision =
                    keyframe_resync.observe(packet->encoded_sequence, packet->keyframe);
                if (decision.gap_detected) {
                    counters.network_gap_events.fetch_add(1, std::memory_order_relaxed);
                }
                if (!decision.accept) {
                    counters.packets_skipped_until_idr.fetch_add(1,
                                                                  std::memory_order_relaxed);
                    continue;
                }

                const uint64_t write_started_us = monotonic_us();
                if (!publisher.write(*packet, &thread_error)) {
                    shared_error.set("RTSP write failed: " + thread_error);
                    fatal.store(true, std::memory_order_relaxed);
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                const uint64_t write_us = monotonic_us() - write_started_us;
                const uint64_t pipeline_us = monotonic_us() -
                                             packet->source_dequeue_timestamp_us;
                counters.network_total_us.fetch_add(write_us, std::memory_order_relaxed);
                update_max(&counters.network_max_us, write_us);
                counters.pipeline_total_us.fetch_add(pipeline_us, std::memory_order_relaxed);
                update_max(&counters.pipeline_max_us, pipeline_us);
                counters.published_bytes.fetch_add(packet->bytes.size(),
                                                   std::memory_order_relaxed);
                counters.published.fetch_add(1, std::memory_order_relaxed);
            }
            packet_queue.close();
            publisher.close();
        });

        if (!publisher_ready.get()) {
            packet_queue.close();
            network_thread.join();
            rga.close();
            capture.close();
            std::cerr << shared_error.get() << '\n';
            return 4;
        }

        std::cout << "DMABUF_STREAM_READY"
                  << " mode=DMABUF_RGA"
                  << " url=" << options.url
                  << " source=" << capture_info.width << 'x' << capture_info.height
                  << " source_stride=" << capture_info.bytes_per_line << 'x'
                  << capture_info.height
                  << " mpp_stride=" << encoder.info().horizontal_stride << 'x'
                  << encoder.info().vertical_stride
                  << " v4l2_buffers=" << capture_info.buffer_count
                  << " frame_queue=" << options.frame_queue_capacity
                  << " packet_queue=" << options.packet_queue_capacity
                  << " encoder_delay_ms=" << options.encoder_delay_ms << '\n';

        // ---------- 编码线程 ----------
        // 一次循环的正确 ownership 顺序：
        //   取得 lease -> RGA 同步 copy -> release/QBUF -> MPP 编码 -> packet 入队。
        // 特别注意 release 在 MPP 前：MPP 读取的是自己的目标 buffer，不再依赖源相机 slot。
        std::thread encoder_thread([&] {
            uint64_t encoded_sequence = 0;
            std::string thread_error;
            while (true) {
                auto frame = frame_queue.pop();
                if (!frame.has_value()) {
                    break;
                }
                update_max(&counters.frame_queue_max_latency_us,
                           monotonic_us() - frame->dequeue_timestamp_us);

                const uint64_t rga_started_us = monotonic_us();
                const bool copied = rga.copy(frame->buffer_index, &thread_error);
                const uint64_t rga_us = monotonic_us() - rga_started_us;
                counters.rga_total_us.fetch_add(rga_us, std::memory_order_relaxed);
                update_max(&counters.rga_max_us, rga_us);
                if (copied) {
                    counters.rga_completed.fetch_add(1, std::memory_order_relaxed);
                }

                // 无论 RGA 成功与否，只要 DQBUF 成功过，就必须在退出前归还 lease。
                const bool released = release_lease(
                    *frame, copied ? "release after RGA" : "release after RGA failure");
                if (!copied || !released) {
                    if (!copied) {
                        shared_error.set("RGA copy failed: " + thread_error);
                        fatal.store(true, std::memory_order_relaxed);
                        stop.store(true, std::memory_order_relaxed);
                    }
                    break;
                }

                // 诊断延迟模拟 RGA 之后的 MPP/下游变慢。此时源 DMA-BUF 已经 QBUF，
                // 所以压力测试不会人为长期占用相机 buffer。
                if (options.encoder_delay_ms > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(options.encoder_delay_ms));
                }

                rkstream::EncodedPacket packet;
                const uint64_t mpp_started_us = monotonic_us();
                if (!encoder.encode_prepared_nv12(
                        static_cast<int64_t>(frame->driver_timestamp_us),
                        &packet, &thread_error)) {
                    shared_error.set("MPP encode failed: " + thread_error);
                    fatal.store(true, std::memory_order_relaxed);
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                const uint64_t mpp_us = monotonic_us() - mpp_started_us;
                counters.mpp_total_us.fetch_add(mpp_us, std::memory_order_relaxed);
                update_max(&counters.mpp_max_us, mpp_us);

                // encoded_sequence 跟踪“编码包队列”连续性；source_sequence 跟踪摄像头
                // 驱动连续性。两者不能混用，因为 frame queue 可以主动 drop-oldest。
                packet.encoded_sequence = encoded_sequence++;
                packet.source_sequence = frame->sequence;
                packet.source_dequeue_timestamp_us = frame->dequeue_timestamp_us;
                packet.encoded_timestamp_us = monotonic_us();
                packet.keyframe = rkstream::inspect_annexb(
                    packet.bytes.data(), packet.bytes.size()).idr > 0;
                counters.encoded.fetch_add(1, std::memory_order_relaxed);
                if (!packet_queue.push(std::move(packet)).accepted) {
                    break;
                }
            }
            packet_queue.close();
        });

        // ---------- 采集线程 ----------
        // 只负责尽快 DQBUF 并把 lease 交给队列，不执行 RGA/MPP/网络阻塞操作。
        std::thread capture_thread([&] {
            bool have_previous_sequence = false;
            uint32_t previous_sequence = 0;
            std::string thread_error;
            while (!stop.load(std::memory_order_relaxed) && !g_stop_requested &&
                   std::chrono::steady_clock::now() < deadline) {
                rkstream::DmabufFrame frame;
                const rkstream::CaptureResult result =
                    capture.capture(&frame, 500, &thread_error);
                if (result == rkstream::CaptureResult::kTimeout) {
                    counters.capture_timeouts.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (result == rkstream::CaptureResult::kInterrupted) {
                    continue;
                }
                if (result == rkstream::CaptureResult::kFatal) {
                    shared_error.set("V4L2 capture failed: " + thread_error);
                    fatal.store(true, std::memory_order_relaxed);
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }

                if (have_previous_sequence) {
                    const uint32_t delta = frame.sequence - previous_sequence;
                    if (delta > 1) {
                        counters.driver_sequence_gaps.fetch_add(delta - 1,
                                                                 std::memory_order_relaxed);
                    }
                }
                previous_sequence = frame.sequence;
                have_previous_sequence = true;
                counters.captured.fetch_add(1, std::memory_order_relaxed);

                // push 会移动 frame；先保留轻量描述，以便队列已关闭时仍能 release。
                const rkstream::DmabufFrame current = frame;
                auto push_result = frame_queue.push(std::move(frame));
                if (!push_result.accepted) {
                    (void)release_lease(current, "release rejected frame");
                    break;
                }
                // drop-oldest 返回的 lease 已从队列移出，但仍属于应用，必须立即 QBUF。
                if (push_result.dropped_value.has_value()) {
                    counters.leases_dropped_oldest.fetch_add(1,
                                                              std::memory_order_relaxed);
                    if (!release_lease(*push_result.dropped_value,
                                       "release drop-oldest frame")) {
                        break;
                    }
                }
            }
            stop.store(true, std::memory_order_relaxed);
            frame_queue.close();
        });

        // 主线程只负责定时状态输出、时限和信号；不参与像素热路径。
        auto next_status = started_at + std::chrono::seconds(1);
        while (!stop.load(std::memory_order_relaxed) && !g_stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (std::chrono::steady_clock::now() < next_status) {
                continue;
            }
            std::cout << "DMABUF_STATUS"
                      << " captured=" << counters.captured.load(std::memory_order_relaxed)
                      << " released="
                      << counters.leases_released.load(std::memory_order_relaxed)
                      << " encoded=" << counters.encoded.load(std::memory_order_relaxed)
                      << " published=" << counters.published.load(std::memory_order_relaxed)
                      << " frame_queue=" << frame_queue.size() << '/'
                      << frame_queue.capacity()
                      << " frame_drop=" << frame_queue.dropped_count()
                      << " packet_queue=" << packet_queue.size() << '/'
                      << packet_queue.capacity()
                      << " packet_drop=" << packet_queue.dropped_count() << '\n';
            next_status += std::chrono::seconds(1);
        }

        // ---------- 有序停机 ----------
        // 关闭顺序沿数据流从上游到下游：capture -> frame queue -> encoder -> packet
        // queue -> network。先 join 生产者再关下游，避免消费者退出后生产者继续塞数据。
        stop.store(true, std::memory_order_relaxed);
        capture_thread.join();
        frame_queue.close();
        encoder_thread.join();

        // encoder 可能因网络关闭或编码错误提前退出；此时队列中的 lease 必须人工归还。
        auto remaining_frames = frame_queue.drain();
        for (const auto& frame : remaining_frames) {
            counters.leases_drained.fetch_add(1, std::memory_order_relaxed);
            (void)release_lease(frame, "release drained frame");
        }

        packet_queue.close();
        network_thread.join();
        const std::size_t packets_drained = packet_queue.drain().size();
        rga.close();
        capture.close();
        encoder.close();

        const double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started_at).count();
        rusage usage_end{};
        (void)getrusage(RUSAGE_SELF, &usage_end);
        const double user_cpu_s = timeval_seconds(usage_end.ru_utime) -
                                  timeval_seconds(usage_start.ru_utime);
        const double system_cpu_s = timeval_seconds(usage_end.ru_stime) -
                                    timeval_seconds(usage_start.ru_stime);
        const uint64_t captured = counters.captured.load(std::memory_order_relaxed);
        const uint64_t released = counters.leases_released.load(std::memory_order_relaxed);
        const uint64_t rga_completed = counters.rga_completed.load(std::memory_order_relaxed);
        const uint64_t encoded = counters.encoded.load(std::memory_order_relaxed);
        const uint64_t published = counters.published.load(std::memory_order_relaxed);
        const int64_t outstanding_leases = static_cast<int64_t>(captured) -
                                           static_cast<int64_t>(released);
        // captured == released 是 DMA-BUF 路径最重要的不变量。即使视频可播放，只要
        // outstanding_leases 非零，也说明某条异常/停机路径存在资源所有权缺陷。
        const bool lease_balance = outstanding_leases == 0 &&
            counters.lease_release_failures.load(std::memory_order_relaxed) == 0;
        const bool pass = !fatal.load(std::memory_order_relaxed) && lease_balance;

        std::cout << std::fixed << std::setprecision(3)
                  << "DMABUF_STREAM_SUMMARY result=" << (pass ? "PASS" : "FAIL")
                  << " elapsed_s=" << elapsed_s
                  << " captured=" << captured
                  << " leases_released=" << released
                  << " outstanding_leases=" << outstanding_leases
                  << " lease_balance=" << (lease_balance ? "true" : "false")
                  << " lease_drop_oldest="
                  << counters.leases_dropped_oldest.load(std::memory_order_relaxed)
                  << " leases_drained="
                  << counters.leases_drained.load(std::memory_order_relaxed)
                  << " lease_release_failures="
                  << counters.lease_release_failures.load(std::memory_order_relaxed)
                  << " rga_completed=" << rga_completed
                  << " encoded=" << encoded
                  << " published=" << published
                  << " stream_fps=" << (elapsed_s > 0 ? published / elapsed_s : 0.0)
                  << " published_bytes="
                  << counters.published_bytes.load(std::memory_order_relaxed)
                  << " avg_rga_ms="
                  << (rga_completed > 0
                          ? counters.rga_total_us.load(std::memory_order_relaxed) /
                                rga_completed / 1000.0
                          : 0.0)
                  << " max_rga_ms="
                  << counters.rga_max_us.load(std::memory_order_relaxed) / 1000.0
                  << " avg_mpp_ms="
                  << (encoded > 0
                          ? counters.mpp_total_us.load(std::memory_order_relaxed) /
                                encoded / 1000.0
                          : 0.0)
                  << " max_mpp_ms="
                  << counters.mpp_max_us.load(std::memory_order_relaxed) / 1000.0
                  << " avg_network_ms="
                  << (published > 0
                          ? counters.network_total_us.load(std::memory_order_relaxed) /
                                published / 1000.0
                          : 0.0)
                  << " max_network_ms="
                  << counters.network_max_us.load(std::memory_order_relaxed) / 1000.0
                  << " avg_internal_pipeline_ms="
                  << (published > 0
                          ? counters.pipeline_total_us.load(std::memory_order_relaxed) /
                                published / 1000.0
                          : 0.0)
                  << " max_internal_pipeline_ms="
                  << counters.pipeline_max_us.load(std::memory_order_relaxed) / 1000.0
                  << " frame_queue_high=" << frame_queue.high_watermark()
                  << " frame_drop_oldest=" << frame_queue.dropped_count()
                  << " frame_queue_max_latency_ms="
                  << counters.frame_queue_max_latency_us.load(std::memory_order_relaxed) /
                        1000.0
                  << " packet_queue_high=" << packet_queue.high_watermark()
                  << " packet_drop_oldest=" << packet_queue.dropped_count()
                  << " packets_drained=" << packets_drained
                  << " packet_queue_max_latency_ms="
                  << counters.packet_queue_max_latency_us.load(std::memory_order_relaxed) /
                        1000.0
                  << " network_gap_events="
                  << counters.network_gap_events.load(std::memory_order_relaxed)
                  << " skipped_until_idr="
                  << counters.packets_skipped_until_idr.load(std::memory_order_relaxed)
                  << " driver_gaps="
                  << counters.driver_sequence_gaps.load(std::memory_order_relaxed)
                  << " capture_timeouts="
                  << counters.capture_timeouts.load(std::memory_order_relaxed)
                  << " encoder_delay_ms=" << options.encoder_delay_ms
                  << " user_cpu_s=" << user_cpu_s
                  << " system_cpu_s=" << system_cpu_s
                  << " cpu_percent="
                  << (elapsed_s > 0
                          ? (user_cpu_s + system_cpu_s) / elapsed_s * 100.0
                          : 0.0)
                  << " max_rss_kib=" << usage_end.ru_maxrss
                  << " error=\"" << shared_error.get() << "\"\n";
        return pass ? 0 : 5;
    } catch (const std::exception& exception) {
        std::cerr << "argument error: " << exception.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
