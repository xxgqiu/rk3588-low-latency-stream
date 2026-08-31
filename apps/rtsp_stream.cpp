#include "rkstream/annexb.h"
#include "rkstream/bounded_latest_queue.h"
#include "rkstream/frame.h"
#include "rkstream/keyframe_resync.h"
#include "rkstream/mpp_h264_encoder.h"
#include "rkstream/rtsp_publisher.h"
#include "rkstream/v4l2_capture.h"

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
 * CPU-copy RTSP 基线。
 *
 * 它与 dmabuf_rtsp_stream.cpp 保持相同的“采集 / 编码 / 网络”三线程结构，差异只在
 * 原始帧传递方式：V4L2Capture 在 DQBUF 后把 NV12 拷贝进 Frame::data，MPP 又把
 * Frame::data 逐行拷贝进对齐输入缓冲。保留这条可运行路径有三个价值：
 *
 *   1. USB/MIPI 采集和 RTSP 问题可先在简单 ownership 模型中定位；
 *   2. DMA-BUF 优化失效时仍有明确 fallback；
 *   3. CPU、RSS、内部延迟可以与优化路径做同条件对照，而不是凭感觉宣称优化。
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

struct Options {
    std::string device = "/dev/video-camera0";
    std::string url = "rtsp://127.0.0.1:8554/live";
    uint32_t duration_seconds = 20;
    uint32_t encoder_fps = 24;
    uint32_t bitrate = 4'000'000;
    uint32_t gop = 48;
    uint32_t frame_queue_capacity = 4;
    uint32_t packet_queue_capacity = 8;
};

uint32_t parse_u32(const char* value, const char* option) {
    const std::string text(value);
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(text, &consumed, 10);
    if (consumed != text.size() || parsed == 0 ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(std::string("invalid value for ") + option + ": " + text);
    }
    return static_cast<uint32_t>(parsed);
}

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --device PATH             NV12 V4L2 capture node\n"
        << "  --url URL                 RTSP publication URL\n"
        << "  --duration N              Run duration in seconds\n"
        << "  --fps N                   Encoder frame rate\n"
        << "  --bitrate N               Target bitrate in bits/s\n"
        << "  --gop N                   H.264 GOP length\n"
        << "  --frame-queue N           Capture-to-encoder queue capacity\n"
        << "  --packet-queue N          Encoder-to-network queue capacity\n"
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
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    return options;
}

struct Counters {
    std::atomic<uint64_t> captured{0};
    std::atomic<uint64_t> encoded{0};
    std::atomic<uint64_t> published{0};
    std::atomic<uint64_t> published_bytes{0};
    std::atomic<uint64_t> capture_timeouts{0};
    std::atomic<uint64_t> driver_sequence_gaps{0};
    std::atomic<uint64_t> encode_total_us{0};
    std::atomic<uint64_t> encode_max_us{0};
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

        // 初始化顺序：先确认真实相机格式，再把实际宽高/stride 传给 MPP。
        rkstream::V4L2Capture capture;
        std::string error;
        const rkstream::CaptureConfig capture_config{
            options.device, 1632, 1224, 30, V4L2_PIX_FMT_NV12, 4,
        };
        if (!capture.open(capture_config, &error) || !capture.start(&error)) {
            std::cerr << "V4L2 initialization failed: " << error << '\n';
            return 2;
        }
        const rkstream::CaptureInfo capture_info = capture.info();

        rkstream::MppH264Encoder encoder;
        const rkstream::MppEncoderConfig encoder_config{
            capture_info.width,
            capture_info.height,
            capture_info.bytes_per_line,
            options.encoder_fps,
            options.bitrate,
            options.gop,
        };
        if (!encoder.open(encoder_config, &error)) {
            std::cerr << "MPP initialization failed: " << error << '\n';
            return 3;
        }

        rkstream::BoundedLatestQueue<rkstream::Frame> frame_queue(
            options.frame_queue_capacity);
        rkstream::BoundedLatestQueue<rkstream::EncodedPacket> packet_queue(
            options.packet_queue_capacity);
        Counters counters;
        SharedError shared_error;
        std::atomic<bool> stop{false};
        std::atomic<bool> fatal{false};
        const auto started_at = std::chrono::steady_clock::now();
        const auto deadline = started_at + std::chrono::seconds(options.duration_seconds);

        // 网络初始化屏障，语义与优化版一致。
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

            rkstream::KeyframeResync keyframe_resync;
            while (true) {
                auto packet = packet_queue.pop();
                if (!packet.has_value()) {
                    break;
                }
                update_max(&counters.packet_queue_max_latency_us,
                           monotonic_us() - packet->encoded_timestamp_us);

                const rkstream::KeyframeResyncDecision resync =
                    keyframe_resync.observe(packet->encoded_sequence, packet->keyframe);
                if (resync.gap_detected) {
                    counters.network_gap_events.fetch_add(1, std::memory_order_relaxed);
                }
                if (!resync.accept) {
                    counters.packets_skipped_until_idr.fetch_add(1, std::memory_order_relaxed);
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
            publisher.close();
        });

        if (!publisher_ready.get()) {
            packet_queue.close();
            network_thread.join();
            capture.close();
            encoder.close();
            std::cerr << shared_error.get() << '\n';
            return 4;
        }

        std::cout << "STREAM_READY"
                  << " url=" << options.url
                  << " source=" << capture_info.width << 'x' << capture_info.height
                  << " source_stride=" << capture_info.bytes_per_line
                  << " fps=" << options.encoder_fps
                  << " bitrate=" << options.bitrate
                  << " gop=" << options.gop
                  << " frame_queue=" << options.frame_queue_capacity
                  << " packet_queue=" << options.packet_queue_capacity << '\n';

        // 编码线程拥有从 frame_queue pop 出来的 Frame；离开循环后 vector 自动释放。
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

                rkstream::EncodedPacket packet;
                const uint64_t encode_started_us = monotonic_us();
                if (!encoder.encode_nv12(frame->data.data(), frame->data.size(),
                                         static_cast<int64_t>(frame->driver_timestamp_us),
                                         &packet, &thread_error)) {
                    shared_error.set("MPP encode failed: " + thread_error);
                    fatal.store(true, std::memory_order_relaxed);
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                const uint64_t encode_us = monotonic_us() - encode_started_us;
                counters.encode_total_us.fetch_add(encode_us, std::memory_order_relaxed);
                update_max(&counters.encode_max_us, encode_us);

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

        // capture() 已在内部完成 memcpy 和 QBUF，因此队列丢 Frame 只会释放 vector，
        // 不需要像 DMA-BUF lease 那样显式归还相机资源。
        std::thread capture_thread([&] {
            bool have_previous_sequence = false;
            uint32_t previous_sequence = 0;
            std::string thread_error;
            while (!stop.load(std::memory_order_relaxed) && !g_stop_requested &&
                   std::chrono::steady_clock::now() < deadline) {
                rkstream::Frame frame;
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

                if (have_previous_sequence && frame.sequence > previous_sequence + 1) {
                    counters.driver_sequence_gaps.fetch_add(
                        frame.sequence - previous_sequence - 1, std::memory_order_relaxed);
                }
                previous_sequence = frame.sequence;
                have_previous_sequence = true;
                counters.captured.fetch_add(1, std::memory_order_relaxed);
                if (!frame_queue.push(std::move(frame)).accepted) {
                    break;
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
            const double elapsed_s = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started_at).count();
            std::cout << std::fixed << std::setprecision(2)
                      << "STATUS elapsed_s=" << elapsed_s
                      << " captured=" << counters.captured.load(std::memory_order_relaxed)
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

        // 有序关闭上游、两个队列和下游线程，确保 summary 是完整运行结果。
        stop.store(true, std::memory_order_relaxed);
        capture_thread.join();
        frame_queue.close();
        encoder_thread.join();
        packet_queue.close();
        network_thread.join();
        capture.close();
        encoder.close();

        const double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started_at).count();
        const uint64_t encoded = counters.encoded.load(std::memory_order_relaxed);
        const uint64_t published = counters.published.load(std::memory_order_relaxed);
        rusage usage{};
        (void)getrusage(RUSAGE_SELF, &usage);
        const double user_cpu_s = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1'000'000.0;
        const double system_cpu_s = usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1'000'000.0;
        std::cout << std::fixed << std::setprecision(3)
                  << "STREAM_SUMMARY result="
                  << (fatal.load(std::memory_order_relaxed) ? "FAIL" : "PASS")
                  << " elapsed_s=" << elapsed_s
                  << " captured=" << counters.captured.load(std::memory_order_relaxed)
                  << " encoded=" << encoded
                  << " published=" << published
                  << " stream_fps=" << (elapsed_s > 0 ? published / elapsed_s : 0.0)
                  << " published_bytes="
                  << counters.published_bytes.load(std::memory_order_relaxed)
                  << " avg_encode_ms="
                  << (encoded > 0 ? counters.encode_total_us.load(std::memory_order_relaxed) /
                                        encoded / 1000.0
                                  : 0.0)
                  << " max_encode_ms="
                  << counters.encode_max_us.load(std::memory_order_relaxed) / 1000.0
                  << " avg_network_ms="
                  << (published > 0 ? counters.network_total_us.load(std::memory_order_relaxed) /
                                          published / 1000.0
                                    : 0.0)
                  << " max_network_ms="
                  << counters.network_max_us.load(std::memory_order_relaxed) / 1000.0
                  << " avg_internal_pipeline_ms="
                  << (published > 0 ? counters.pipeline_total_us.load(std::memory_order_relaxed) /
                                          published / 1000.0
                                    : 0.0)
                  << " max_internal_pipeline_ms="
                  << counters.pipeline_max_us.load(std::memory_order_relaxed) / 1000.0
                  << " frame_queue_high=" << frame_queue.high_watermark()
                  << " frame_drop_oldest=" << frame_queue.dropped_count()
                  << " frame_queue_max_latency_ms="
                  << counters.frame_queue_max_latency_us.load(std::memory_order_relaxed) / 1000.0
                  << " packet_queue_high=" << packet_queue.high_watermark()
                  << " packet_drop_oldest=" << packet_queue.dropped_count()
                  << " packet_queue_max_latency_ms="
                  << counters.packet_queue_max_latency_us.load(std::memory_order_relaxed) / 1000.0
                  << " network_gap_events="
                  << counters.network_gap_events.load(std::memory_order_relaxed)
                  << " skipped_until_idr="
                  << counters.packets_skipped_until_idr.load(std::memory_order_relaxed)
                  << " driver_gaps="
                  << counters.driver_sequence_gaps.load(std::memory_order_relaxed)
                  << " capture_timeouts="
                  << counters.capture_timeouts.load(std::memory_order_relaxed)
                  << " user_cpu_s=" << user_cpu_s
                  << " system_cpu_s=" << system_cpu_s
                  << " cpu_percent="
                  << (elapsed_s > 0 ? (user_cpu_s + system_cpu_s) / elapsed_s * 100.0 : 0.0)
                  << " max_rss_kib=" << usage.ru_maxrss
                  << " error=\"" << shared_error.get() << "\"\n";
        return fatal.load(std::memory_order_relaxed) ? 5 : 0;
    } catch (const std::exception& exception) {
        std::cerr << "argument error: " << exception.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
