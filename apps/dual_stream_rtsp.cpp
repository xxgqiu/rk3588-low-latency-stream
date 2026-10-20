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
#include <utility>

/*
 * M6 主/子码流程序。
 *
 * 这不是“两个观众看同一条流”，而是一张相机图像真正生成两条不同的码流：
 *
 *                       +-> RGA 主码流 -> MPP 主编码 -> /live/main
 *   OV8858 -> V4L2 ----+
 *                       +-> RGA 缩放   -> MPP 子编码 -> /live/sub
 *
 * 第一版故意保持一个容易证明正确的 ownership 模型：
 *
 *   1. Capture Thread DQBUF，把“相机 buffer 租约”放入容量为 1 的队列。
 *   2. Process Thread 先同步完成主、子两次 RGA copy。
 *   3. 两个 RGA 都不再读源 DMA-BUF 后，立即 QBUF 归还相机 slot。
 *   4. 两个 MPP 编码器从各自的 input DMA-BUF 编码。
 *   5. 主、子压缩包进入两个独立有界队列，由两个 Network Thread 发布。
 *
 * 这样的取舍是：RGA/MPP 处理仍在一个 Process Thread 中按顺序执行，代码和
 * buffer 生命周期很清楚；但网络发布已经分支隔离，子码流网络线程变慢时，
 * 只会填满子码流 packet queue，不会阻塞主码流 publisher。
 */

namespace {

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
    uint64_t previous = target->load(std::memory_order_relaxed);
    while (previous < value &&
           !target->compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

double timeval_seconds(const timeval& value) {
    return static_cast<double>(value.tv_sec) +
           static_cast<double>(value.tv_usec) / 1'000'000.0;
}

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

struct BranchOptions {
    std::string name;
    std::string url;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t bitrate = 0;
    uint32_t gop = 0;
    uint32_t network_delay_ms = 0;
};

struct Options {
    std::string device = "/dev/video-camera0";
    uint32_t duration_seconds = 20;
    uint32_t fps = 24;
    uint32_t frame_queue_capacity = 1;
    uint32_t packet_queue_capacity = 8;
    BranchOptions main{"main", "rtsp://127.0.0.1:8554/live/main",
                       1632, 1224, 4'000'000, 48, 0};
    BranchOptions sub{"sub", "rtsp://127.0.0.1:8554/live/sub",
                      640, 480, 800'000, 48, 0};
};

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --device PATH                 OV8858 NV12 capture node\n"
        << "  --duration N                  Run duration in seconds\n"
        << "  --fps N                       Both encoders' nominal FPS\n"
        << "  --main-url URL                Main RTSP publication URL\n"
        << "  --main-bitrate N              Main target bitrate\n"
        << "  --sub-url URL                 Sub RTSP publication URL\n"
        << "  --sub-width N                 Sub visible width; even number\n"
        << "  --sub-height N                Sub visible height; even number\n"
        << "  --sub-bitrate N               Sub target bitrate\n"
        << "  --gop N                       GOP length for both branches\n"
        << "  --frame-queue N               Camera lease queue capacity\n"
        << "  --packet-queue N              Capacity of each branch packet queue\n"
        << "  --sub-network-delay-ms N      Diagnostic delay in sub publisher\n"
        << "  --help                         Show this help\n";
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
        } else if (argument == "--duration") {
            options.duration_seconds = parse_u32(value, "--duration");
        } else if (argument == "--fps") {
            options.fps = parse_u32(value, "--fps");
        } else if (argument == "--main-url") {
            options.main.url = value;
        } else if (argument == "--main-bitrate") {
            options.main.bitrate = parse_u32(value, "--main-bitrate");
        } else if (argument == "--sub-url") {
            options.sub.url = value;
        } else if (argument == "--sub-width") {
            options.sub.width = parse_u32(value, "--sub-width");
        } else if (argument == "--sub-height") {
            options.sub.height = parse_u32(value, "--sub-height");
        } else if (argument == "--sub-bitrate") {
            options.sub.bitrate = parse_u32(value, "--sub-bitrate");
        } else if (argument == "--gop") {
            options.main.gop = options.sub.gop = parse_u32(value, "--gop");
        } else if (argument == "--frame-queue") {
            options.frame_queue_capacity = parse_u32(value, "--frame-queue");
        } else if (argument == "--packet-queue") {
            options.packet_queue_capacity = parse_u32(value, "--packet-queue");
        } else if (argument == "--sub-network-delay-ms") {
            options.sub.network_delay_ms =
                parse_u32(value, "--sub-network-delay-ms", true);
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    return options;
}

bool valid_nv12_branch(const BranchOptions& branch,
                       const rkstream::CaptureInfo& source,
                       std::string* error) {
    // NV12 的 UV 是 2x2 采样单元，因此宽、高必须为偶数。
    if (branch.url.empty() || branch.width == 0 || branch.height == 0 ||
        branch.bitrate == 0 || branch.gop == 0 ||
        (branch.width % 2) != 0 || (branch.height % 2) != 0 ||
        branch.width > source.width || branch.height > source.height) {
        if (error != nullptr) {
            *error = branch.name +
                " branch must have URL, positive bitrate/GOP, even NV12 dimensions "
                "not larger than the camera source";
        }
        return false;
    }
    return true;
}

struct SharedError {
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

struct CommonCounters {
    std::atomic<uint64_t> captured{0};
    std::atomic<uint64_t> leases_released{0};
    std::atomic<uint64_t> leases_dropped_oldest{0};
    std::atomic<uint64_t> leases_drained{0};
    std::atomic<uint64_t> lease_release_failures{0};
    std::atomic<uint64_t> capture_timeouts{0};
    std::atomic<uint64_t> driver_sequence_gaps{0};
    std::atomic<uint64_t> frame_queue_max_latency_us{0};
};

struct BranchCounters {
    std::atomic<uint64_t> rga_completed{0};
    std::atomic<uint64_t> encoded{0};
    std::atomic<uint64_t> published{0};
    std::atomic<uint64_t> published_bytes{0};
    std::atomic<uint64_t> rga_total_us{0};
    std::atomic<uint64_t> rga_max_us{0};
    std::atomic<uint64_t> mpp_total_us{0};
    std::atomic<uint64_t> mpp_max_us{0};
    std::atomic<uint64_t> network_total_us{0};
    std::atomic<uint64_t> network_max_us{0};
    std::atomic<uint64_t> packet_queue_max_latency_us{0};
    std::atomic<uint64_t> pipeline_total_us{0};
    std::atomic<uint64_t> pipeline_max_us{0};
    std::atomic<uint64_t> network_gap_events{0};
    std::atomic<uint64_t> packets_skipped_until_idr{0};
};

struct BranchRuntime {
    BranchRuntime(BranchOptions branch_options, std::size_t packet_capacity)
        : options(std::move(branch_options)), packet_queue(packet_capacity) {}

    BranchOptions options;
    rkstream::MppH264Encoder encoder;
    rkstream::RgaNv12Aligner rga;
    rkstream::BoundedLatestQueue<rkstream::EncodedPacket> packet_queue;
    BranchCounters counters;
    uint64_t next_encoded_sequence = 0;
};

bool open_branch(BranchRuntime* branch,
                 uint32_t fps,
                 const rkstream::CaptureInfo& capture_info,
                 const std::vector<rkstream::DmabufBufferInfo>& camera_buffers,
                 std::string* error) {
    const rkstream::MppEncoderConfig encoder_config{
        branch->options.width,
        branch->options.height,
        branch->options.width,
        fps,
        branch->options.bitrate,
        branch->options.gop,
    };
    if (!branch->encoder.open(encoder_config, error) ||
        branch->encoder.input_dmabuf_fd() < 0) {
        if (error != nullptr && error->empty()) {
            *error = branch->options.name + " MPP input DMA-BUF fd unavailable";
        }
        return false;
    }

    // destination_width/height 是分支的可见分辨率。主码流只做布局对齐，
    // 主码流可见尺寸不变时走 imcopy；子码流 1632x1224 -> 640x480
    // 尺寸发生变化，由 RgaNv12Aligner 自动选择同步 imresize。
    return branch->rga.open(capture_info, camera_buffers,
                            branch->encoder.input_dmabuf_fd(),
                            branch->options.width, branch->options.height,
                            branch->encoder.info(), error);
}

bool copy_branch(BranchRuntime* branch,
                 uint32_t source_index,
                 std::string* error) {
    const uint64_t started_us = monotonic_us();
    const bool copied = branch->rga.copy(source_index, error);
    const uint64_t elapsed_us = monotonic_us() - started_us;
    branch->counters.rga_total_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    update_max(&branch->counters.rga_max_us, elapsed_us);
    if (copied) {
        branch->counters.rga_completed.fetch_add(1, std::memory_order_relaxed);
    }
    return copied;
}

bool encode_branch(BranchRuntime* branch,
                   const rkstream::DmabufFrame& source,
                   std::string* error) {
    rkstream::EncodedPacket packet;
    const uint64_t started_us = monotonic_us();
    if (!branch->encoder.encode_prepared_nv12(
            static_cast<int64_t>(source.driver_timestamp_us), &packet, error)) {
        return false;
    }
    const uint64_t elapsed_us = monotonic_us() - started_us;
    branch->counters.mpp_total_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    update_max(&branch->counters.mpp_max_us, elapsed_us);

    packet.encoded_sequence = branch->next_encoded_sequence++;
    packet.source_sequence = source.sequence;
    packet.source_dequeue_timestamp_us = source.dequeue_timestamp_us;
    packet.encoded_timestamp_us = monotonic_us();
    packet.keyframe = rkstream::inspect_annexb(
        packet.bytes.data(), packet.bytes.size()).idr > 0;
    branch->counters.encoded.fetch_add(1, std::memory_order_relaxed);

    // push 不会等网络。队列满时丢本分支最旧压缩包，不影响另一分支。
    return branch->packet_queue.push(std::move(packet)).accepted;
}

void network_loop(BranchRuntime* branch,
                  uint32_t fps,
                  std::atomic<bool>* stop,
                  std::atomic<bool>* fatal,
                  SharedError* shared_error,
                  std::promise<bool>* ready) {
    rkstream::RtspPublisher publisher;
    std::string error;
    const bool opened = publisher.open(
        {branch->options.url, branch->options.width, branch->options.height, fps},
        branch->encoder.header(), &error);
    if (!opened) {
        shared_error->set(branch->options.name + " RTSP initialization failed: " + error);
        fatal->store(true, std::memory_order_relaxed);
        stop->store(true, std::memory_order_relaxed);
    }
    ready->set_value(opened);
    if (!opened) {
        branch->packet_queue.close();
        return;
    }

    rkstream::KeyframeResync keyframe_resync;
    while (true) {
        auto packet = branch->packet_queue.pop();
        if (!packet.has_value()) {
            break;
        }
        update_max(&branch->counters.packet_queue_max_latency_us,
                   monotonic_us() - packet->encoded_timestamp_us);

        const auto decision =
            keyframe_resync.observe(packet->encoded_sequence, packet->keyframe);
        if (decision.gap_detected) {
            branch->counters.network_gap_events.fetch_add(1, std::memory_order_relaxed);
        }
        if (!decision.accept) {
            branch->counters.packets_skipped_until_idr.fetch_add(
                1, std::memory_order_relaxed);
            continue;
        }

        // 只向子码流注入的故障。它发生在 sub Network Thread，用来验证：
        // sub packet queue 可以自己溢出，main publisher 仍保持正常。
        if (branch->options.network_delay_ms > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(branch->options.network_delay_ms));
        }

        const uint64_t write_started_us = monotonic_us();
        if (!publisher.write(*packet, &error)) {
            shared_error->set(branch->options.name + " RTSP write failed: " + error);
            fatal->store(true, std::memory_order_relaxed);
            stop->store(true, std::memory_order_relaxed);
            break;
        }
        const uint64_t now_us = monotonic_us();
        const uint64_t write_us = now_us - write_started_us;
        const uint64_t pipeline_us = now_us - packet->source_dequeue_timestamp_us;
        branch->counters.network_total_us.fetch_add(write_us, std::memory_order_relaxed);
        update_max(&branch->counters.network_max_us, write_us);
        branch->counters.pipeline_total_us.fetch_add(pipeline_us, std::memory_order_relaxed);
        update_max(&branch->counters.pipeline_max_us, pipeline_us);
        branch->counters.published_bytes.fetch_add(packet->bytes.size(),
                                                    std::memory_order_relaxed);
        branch->counters.published.fetch_add(1, std::memory_order_relaxed);
    }
    publisher.close();
}

void print_branch_summary(const BranchRuntime& branch, double elapsed_s) {
    const uint64_t rga_completed =
        branch.counters.rga_completed.load(std::memory_order_relaxed);
    const uint64_t encoded = branch.counters.encoded.load(std::memory_order_relaxed);
    const uint64_t published = branch.counters.published.load(std::memory_order_relaxed);
    std::cout
        << "BRANCH_SUMMARY"
        << " name=" << branch.options.name
        << " url=" << branch.options.url
        << " size=" << branch.options.width << 'x' << branch.options.height
        << " bitrate_target=" << branch.options.bitrate
        << " encoded=" << encoded
        << " published=" << published
        << " stream_fps=" << (elapsed_s > 0.0 ? published / elapsed_s : 0.0)
        << " published_bytes="
        << branch.counters.published_bytes.load(std::memory_order_relaxed)
        << " avg_rga_ms="
        << (rga_completed > 0
                ? branch.counters.rga_total_us.load(std::memory_order_relaxed) /
                      rga_completed / 1000.0
                : 0.0)
        << " max_rga_ms="
        << branch.counters.rga_max_us.load(std::memory_order_relaxed) / 1000.0
        << " avg_mpp_ms="
        << (encoded > 0
                ? branch.counters.mpp_total_us.load(std::memory_order_relaxed) /
                      encoded / 1000.0
                : 0.0)
        << " max_mpp_ms="
        << branch.counters.mpp_max_us.load(std::memory_order_relaxed) / 1000.0
        << " avg_network_ms="
        << (published > 0
                ? branch.counters.network_total_us.load(std::memory_order_relaxed) /
                      published / 1000.0
                : 0.0)
        << " max_network_ms="
        << branch.counters.network_max_us.load(std::memory_order_relaxed) / 1000.0
        << " avg_internal_pipeline_ms="
        << (published > 0
                ? branch.counters.pipeline_total_us.load(std::memory_order_relaxed) /
                      published / 1000.0
                : 0.0)
        << " max_internal_pipeline_ms="
        << branch.counters.pipeline_max_us.load(std::memory_order_relaxed) / 1000.0
        << " packet_queue_high=" << branch.packet_queue.high_watermark()
        << " packet_drop_oldest=" << branch.packet_queue.dropped_count()
        << " packet_queue_max_latency_ms="
        << branch.counters.packet_queue_max_latency_us.load(std::memory_order_relaxed) /
              1000.0
        << " network_gap_events="
        << branch.counters.network_gap_events.load(std::memory_order_relaxed)
        << " skipped_until_idr="
        << branch.counters.packets_skipped_until_idr.load(std::memory_order_relaxed)
        << " injected_network_delay_ms=" << branch.options.network_delay_ms
        << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

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
        if (!rkstream::valid_dmabuf_lease_queue_capacity(
                capture_info.buffer_count, options.frame_queue_capacity)) {
            std::cerr << "invalid frame queue for camera buffer count\n";
            return 2;
        }
        if (!valid_nv12_branch(options.main, capture_info, &error) ||
            !valid_nv12_branch(options.sub, capture_info, &error) ||
            options.main.url == options.sub.url) {
            std::cerr << "invalid branch configuration: "
                      << (options.main.url == options.sub.url
                              ? "main and sub URLs must differ"
                              : error)
                      << '\n';
            return 2;
        }

        BranchRuntime main_branch(options.main, options.packet_queue_capacity);
        BranchRuntime sub_branch(options.sub, options.packet_queue_capacity);
        if (!open_branch(&main_branch, options.fps, capture_info,
                         capture.buffers(), &error)) {
            std::cerr << "main branch initialization failed: " << error << '\n';
            return 3;
        }
        if (!open_branch(&sub_branch, options.fps, capture_info,
                         capture.buffers(), &error)) {
            std::cerr << "sub branch initialization failed: " << error << '\n';
            return 3;
        }
        if (!capture.start(&error)) {
            std::cerr << "V4L2 stream start failed: " << error << '\n';
            return 2;
        }

        rkstream::BoundedLatestQueue<rkstream::DmabufFrame> frame_queue(
            options.frame_queue_capacity);
        CommonCounters common;
        SharedError shared_error;
        std::atomic<bool> stop{false};
        std::atomic<bool> fatal{false};
        const auto started_at = std::chrono::steady_clock::now();
        const auto deadline = started_at +
            std::chrono::seconds(options.duration_seconds);
        rusage usage_start{};
        (void)getrusage(RUSAGE_SELF, &usage_start);

        auto release_lease = [&](const rkstream::DmabufFrame& frame,
                                 const char* context) -> bool {
            std::string release_error;
            if (!capture.release(frame, &release_error)) {
                common.lease_release_failures.fetch_add(1, std::memory_order_relaxed);
                shared_error.set(std::string(context) + ": " + release_error);
                fatal.store(true, std::memory_order_relaxed);
                stop.store(true, std::memory_order_relaxed);
                return false;
            }
            common.leases_released.fetch_add(1, std::memory_order_relaxed);
            return true;
        };

        // 两个 publisher 先成功连接 MediaMTX，才允许摄像头线程开始生产。
        std::promise<bool> main_ready_promise;
        std::promise<bool> sub_ready_promise;
        auto main_ready = main_ready_promise.get_future();
        auto sub_ready = sub_ready_promise.get_future();
        std::thread main_network_thread(network_loop, &main_branch, options.fps,
                                        &stop, &fatal, &shared_error,
                                        &main_ready_promise);
        std::thread sub_network_thread(network_loop, &sub_branch, options.fps,
                                       &stop, &fatal, &shared_error,
                                       &sub_ready_promise);
        const bool main_opened = main_ready.get();
        const bool sub_opened = sub_ready.get();
        if (!main_opened || !sub_opened) {
            main_branch.packet_queue.close();
            sub_branch.packet_queue.close();
            main_network_thread.join();
            sub_network_thread.join();
            std::cerr << shared_error.get() << '\n';
            return 4;
        }

        std::cout
            << "DUAL_STREAM_READY"
            << " source=" << capture_info.width << 'x' << capture_info.height
            << " fps=" << options.fps
            << " main=" << main_branch.options.width << 'x'
            << main_branch.options.height << '@' << main_branch.options.url
            << " sub=" << sub_branch.options.width << 'x'
            << sub_branch.options.height << '@' << sub_branch.options.url
            << " v4l2_buffers=" << capture_info.buffer_count
            << " frame_queue=" << options.frame_queue_capacity
            << " packet_queue_per_branch=" << options.packet_queue_capacity
            << " sub_network_delay_ms=" << options.sub.network_delay_ms
            << '\n';

        std::thread process_thread([&] {
            std::string thread_error;
            while (true) {
                auto frame = frame_queue.pop();
                if (!frame.has_value()) {
                    break;
                }
                update_max(&common.frame_queue_max_latency_us,
                           monotonic_us() - frame->dequeue_timestamp_us);

                // 核心 ownership 顺序：两个同步 RGA 都完成以后，才能归还源 slot。
                // 若第一次 copy 后就 QBUF，ISP 可能在 sub RGA 还在读时覆盖它。
                const bool main_copied =
                    copy_branch(&main_branch, frame->buffer_index, &thread_error);
                const bool sub_copied = main_copied &&
                    copy_branch(&sub_branch, frame->buffer_index, &thread_error);
                const bool released = release_lease(
                    *frame, (main_copied && sub_copied)
                                ? "release after dual RGA"
                                : "release after dual RGA failure");
                if (!main_copied || !sub_copied || !released) {
                    if (!main_copied || !sub_copied) {
                        shared_error.set("dual RGA copy failed: " + thread_error);
                        fatal.store(true, std::memory_order_relaxed);
                        stop.store(true, std::memory_order_relaxed);
                    }
                    break;
                }

                // MPP 现在只读各自的 input DMA-BUF，与已 QBUF 的 camera slot 无关。
                if (!encode_branch(&main_branch, *frame, &thread_error)) {
                    shared_error.set("main MPP/queue failed: " + thread_error);
                    fatal.store(true, std::memory_order_relaxed);
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                if (!encode_branch(&sub_branch, *frame, &thread_error)) {
                    shared_error.set("sub MPP/queue failed: " + thread_error);
                    fatal.store(true, std::memory_order_relaxed);
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
            }
            main_branch.packet_queue.close();
            sub_branch.packet_queue.close();
        });

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
                    common.capture_timeouts.fetch_add(1, std::memory_order_relaxed);
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
                        common.driver_sequence_gaps.fetch_add(
                            delta - 1, std::memory_order_relaxed);
                    }
                }
                previous_sequence = frame.sequence;
                have_previous_sequence = true;
                common.captured.fetch_add(1, std::memory_order_relaxed);

                const rkstream::DmabufFrame current = frame;
                auto push_result = frame_queue.push(std::move(frame));
                if (!push_result.accepted) {
                    (void)release_lease(current, "release rejected frame");
                    break;
                }
                if (push_result.dropped_value.has_value()) {
                    common.leases_dropped_oldest.fetch_add(1,
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

        auto next_status = started_at + std::chrono::seconds(1);
        while (!stop.load(std::memory_order_relaxed) && !g_stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (std::chrono::steady_clock::now() < next_status) {
                continue;
            }
            std::cout
                << "DUAL_STREAM_STATUS"
                << " captured=" << common.captured.load(std::memory_order_relaxed)
                << " released=" << common.leases_released.load(std::memory_order_relaxed)
                << " frame_queue=" << frame_queue.size() << '/'
                << frame_queue.capacity()
                << " main_published="
                << main_branch.counters.published.load(std::memory_order_relaxed)
                << " main_queue=" << main_branch.packet_queue.size() << '/'
                << main_branch.packet_queue.capacity()
                << " main_drop=" << main_branch.packet_queue.dropped_count()
                << " sub_published="
                << sub_branch.counters.published.load(std::memory_order_relaxed)
                << " sub_queue=" << sub_branch.packet_queue.size() << '/'
                << sub_branch.packet_queue.capacity()
                << " sub_drop=" << sub_branch.packet_queue.dropped_count()
                << '\n';
            next_status += std::chrono::seconds(1);
        }

        stop.store(true, std::memory_order_relaxed);
        capture_thread.join();
        frame_queue.close();
        process_thread.join();

        auto remaining_frames = frame_queue.drain();
        for (const auto& frame : remaining_frames) {
            common.leases_drained.fetch_add(1, std::memory_order_relaxed);
            (void)release_lease(frame, "release drained frame");
        }

        main_branch.packet_queue.close();
        sub_branch.packet_queue.close();
        main_network_thread.join();
        sub_network_thread.join();
        const std::size_t main_packets_drained =
            main_branch.packet_queue.drain().size();
        const std::size_t sub_packets_drained =
            sub_branch.packet_queue.drain().size();

        main_branch.rga.close();
        sub_branch.rga.close();
        capture.close();
        main_branch.encoder.close();
        sub_branch.encoder.close();

        const double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started_at).count();
        rusage usage_end{};
        (void)getrusage(RUSAGE_SELF, &usage_end);
        const double user_cpu_s = timeval_seconds(usage_end.ru_utime) -
                                  timeval_seconds(usage_start.ru_utime);
        const double system_cpu_s = timeval_seconds(usage_end.ru_stime) -
                                    timeval_seconds(usage_start.ru_stime);
        const uint64_t captured = common.captured.load(std::memory_order_relaxed);
        const uint64_t released =
            common.leases_released.load(std::memory_order_relaxed);
        const int64_t outstanding_leases = static_cast<int64_t>(captured) -
                                           static_cast<int64_t>(released);
        const bool lease_balance = outstanding_leases == 0 &&
            common.lease_release_failures.load(std::memory_order_relaxed) == 0;
        const bool both_published =
            main_branch.counters.published.load(std::memory_order_relaxed) > 0 &&
            sub_branch.counters.published.load(std::memory_order_relaxed) > 0;
        const bool pass = !fatal.load(std::memory_order_relaxed) &&
                          lease_balance && both_published;

        std::cout << std::fixed << std::setprecision(3);
        print_branch_summary(main_branch, elapsed_s);
        print_branch_summary(sub_branch, elapsed_s);
        std::cout
            << "DUAL_STREAM_SUMMARY result=" << (pass ? "PASS" : "FAIL")
            << " elapsed_s=" << elapsed_s
            << " captured=" << captured
            << " leases_released=" << released
            << " outstanding_leases=" << outstanding_leases
            << " lease_balance=" << (lease_balance ? "true" : "false")
            << " lease_drop_oldest="
            << common.leases_dropped_oldest.load(std::memory_order_relaxed)
            << " leases_drained="
            << common.leases_drained.load(std::memory_order_relaxed)
            << " lease_release_failures="
            << common.lease_release_failures.load(std::memory_order_relaxed)
            << " frame_queue_high=" << frame_queue.high_watermark()
            << " frame_drop_oldest=" << frame_queue.dropped_count()
            << " frame_queue_max_latency_ms="
            << common.frame_queue_max_latency_us.load(std::memory_order_relaxed) /
                  1000.0
            << " main_packets_drained=" << main_packets_drained
            << " sub_packets_drained=" << sub_packets_drained
            << " driver_gaps="
            << common.driver_sequence_gaps.load(std::memory_order_relaxed)
            << " capture_timeouts="
            << common.capture_timeouts.load(std::memory_order_relaxed)
            << " user_cpu_s=" << user_cpu_s
            << " system_cpu_s=" << system_cpu_s
            << " cpu_percent="
            << (elapsed_s > 0.0
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
