#include "rkstream/bounded_latest_queue.h"
#include "rkstream/frame.h"
#include "rkstream/pixel_format.h"
#include "rkstream/v4l2_capture.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

/*
 * M1 采集与背压实验程序，不编码、不保存画面。
 *
 * 生产者线程持续 V4L2 DQBUF + CPU copy，消费者线程只计数；通过
 * --consumer-delay-ms 人为让消费者变慢，可以观察 queue high watermark 和
 * drop-oldest 是否按预期增长，同时 driver sequence 是否仍连续。它把“摄像头本身
 * 掉帧”和“应用为了新鲜度主动丢旧帧”区分开。
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

struct Options {
    std::string device = "/dev/video-camera0";
    std::string format = "nv12";
    uint32_t width = 1632;
    uint32_t height = 1224;
    uint32_t fps = 30;
    uint32_t duration_seconds = 10;
    uint32_t queue_capacity = 4;
    uint32_t consumer_delay_ms = 0;
};

uint32_t parse_u32(const char* value, const char* option) {
    const std::string text(value);
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(text, &consumed, 10);
    if (consumed != text.size() || parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(std::string("invalid value for ") + option + ": " + text);
    }
    return static_cast<uint32_t>(parsed);
}

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "  --device PATH             V4L2 capture node\n"
        << "  --format nv12|yuyv|mjpeg  Requested pixel format\n"
        << "  --width N                 Requested width\n"
        << "  --height N                Requested height\n"
        << "  --fps N                   Requested frame rate\n"
        << "  --duration N              Run duration in seconds\n"
        << "  --queue-capacity N        Bounded frame queue size\n"
        << "  --consumer-delay-ms N     Artificial sink delay for backpressure tests\n"
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
        } else if (argument == "--format") {
            options.format = value;
        } else if (argument == "--width") {
            options.width = parse_u32(value, "--width");
        } else if (argument == "--height") {
            options.height = parse_u32(value, "--height");
        } else if (argument == "--fps") {
            options.fps = parse_u32(value, "--fps");
        } else if (argument == "--duration") {
            options.duration_seconds = parse_u32(value, "--duration");
        } else if (argument == "--queue-capacity") {
            options.queue_capacity = parse_u32(value, "--queue-capacity");
        } else if (argument == "--consumer-delay-ms") {
            options.consumer_delay_ms = parse_u32(value, "--consumer-delay-ms");
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    if (options.duration_seconds == 0 || options.queue_capacity == 0) {
        throw std::invalid_argument("duration and queue capacity must be greater than zero");
    }
    return options;
}

struct Counters {
    std::atomic<uint64_t> captured{0};
    std::atomic<uint64_t> consumed{0};
    std::atomic<uint64_t> driver_sequence_gaps{0};
    std::atomic<uint64_t> poll_timeouts{0};
    std::atomic<uint64_t> bytes_captured{0};
    std::atomic<uint64_t> max_queue_latency_us{0};
    std::atomic<uint64_t> checksum{0};
    std::atomic<uint64_t> first_dequeue_timestamp_us{0};
    std::atomic<uint64_t> last_dequeue_timestamp_us{0};
    std::atomic<uint64_t> first_driver_timestamp_us{0};
    std::atomic<uint64_t> last_driver_timestamp_us{0};
};

void update_max(std::atomic<uint64_t>* target, uint64_t value) {
    uint64_t previous = target->load(std::memory_order_relaxed);
    while (previous < value &&
           !target->compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const uint32_t pixel_format = rkstream::parse_pixel_format(options.format);

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        rkstream::V4L2Capture capture;
        std::string error;
        const rkstream::CaptureConfig config{
            options.device,
            options.width,
            options.height,
            options.fps,
            pixel_format,
            4,
        };
        if (!capture.open(config, &error)) {
            std::cerr << "capture open failed: " << error << '\n';
            return 2;
        }
        if (!capture.start(&error)) {
            std::cerr << "capture start failed: " << error << '\n';
            return 3;
        }

        const auto& info = capture.info();
        std::cout << "CAPTURE_READY"
                  << " device=" << options.device
                  << " driver=" << info.driver
                  << " card=\"" << info.card << '"'
                  << " api=" << (info.multi_planar ? "multi-planar" : "single-planar")
                  << " format=" << rkstream::pixel_format_name(info.pixel_format)
                  << " size=" << info.width << 'x' << info.height
                  << " fps=" << info.fps_numerator << '/' << info.fps_denominator
                  << " stride=" << info.bytes_per_line
                  << " size_image=" << info.size_image
                  << " planes=" << info.plane_count
                  << " mmap_buffers=" << info.buffer_count << '\n';

        rkstream::BoundedLatestQueue<rkstream::Frame> queue(options.queue_capacity);
        Counters counters;
        std::atomic<bool> running{true};
        std::atomic<bool> fatal_error{false};
        const auto started_at = std::chrono::steady_clock::now();
        const auto deadline = started_at + std::chrono::seconds(options.duration_seconds);

        std::thread consumer([&] {
            while (true) {
                auto frame = queue.pop();
                if (!frame.has_value()) {
                    break;
                }
                const uint64_t latency = monotonic_us() - frame->dequeue_timestamp_us;
                update_max(&counters.max_queue_latency_us, latency);

                uint64_t sample = 0;
                if (!frame->data.empty()) {
                    sample = static_cast<uint64_t>(frame->data.front()) ^
                             static_cast<uint64_t>(frame->data[frame->data.size() / 2]) ^
                             static_cast<uint64_t>(frame->data.back());
                }
                counters.checksum.fetch_xor(sample, std::memory_order_relaxed);
                counters.consumed.fetch_add(1, std::memory_order_relaxed);
                if (options.consumer_delay_ms > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(options.consumer_delay_ms));
                }
            }
        });

        std::thread producer([&] {
            bool have_previous_sequence = false;
            uint32_t previous_sequence = 0;
            while (running.load(std::memory_order_relaxed) &&
                   !g_stop_requested && std::chrono::steady_clock::now() < deadline) {
                rkstream::Frame frame;
                const rkstream::CaptureResult result = capture.capture(&frame, 500, &error);
                if (result == rkstream::CaptureResult::kTimeout) {
                    counters.poll_timeouts.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (result == rkstream::CaptureResult::kInterrupted) {
                    continue;
                }
                if (result == rkstream::CaptureResult::kFatal) {
                    std::cerr << "capture failed: " << error << '\n';
                    fatal_error.store(true, std::memory_order_relaxed);
                    break;
                }

                if (have_previous_sequence && frame.sequence > previous_sequence + 1) {
                    counters.driver_sequence_gaps.fetch_add(
                        frame.sequence - previous_sequence - 1, std::memory_order_relaxed);
                }
                previous_sequence = frame.sequence;
                have_previous_sequence = true;
                uint64_t expected_zero = 0;
                (void)counters.first_dequeue_timestamp_us.compare_exchange_strong(
                    expected_zero, frame.dequeue_timestamp_us, std::memory_order_relaxed);
                counters.last_dequeue_timestamp_us.store(
                    frame.dequeue_timestamp_us, std::memory_order_relaxed);
                if (frame.driver_timestamp_us > 0) {
                    expected_zero = 0;
                    (void)counters.first_driver_timestamp_us.compare_exchange_strong(
                        expected_zero, frame.driver_timestamp_us, std::memory_order_relaxed);
                    counters.last_driver_timestamp_us.store(
                        frame.driver_timestamp_us, std::memory_order_relaxed);
                }
                counters.bytes_captured.fetch_add(frame.data.size(), std::memory_order_relaxed);
                counters.captured.fetch_add(1, std::memory_order_relaxed);
                if (!queue.push(std::move(frame)).accepted) {
                    break;
                }
            }
            running.store(false, std::memory_order_relaxed);
            queue.close();
        });

        auto next_status = started_at + std::chrono::seconds(1);
        while (running.load(std::memory_order_relaxed) && !g_stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (std::chrono::steady_clock::now() < next_status) {
                continue;
            }
            const double elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started_at).count();
            const uint64_t captured = counters.captured.load(std::memory_order_relaxed);
            std::cout << std::fixed << std::setprecision(2)
                      << "STATUS elapsed_s=" << elapsed
                      << " capture_fps=" << (elapsed > 0 ? captured / elapsed : 0.0)
                      << " captured=" << captured
                      << " consumed=" << counters.consumed.load(std::memory_order_relaxed)
                      << " queue=" << queue.size() << '/' << queue.capacity()
                      << " drop_oldest=" << queue.dropped_count()
                      << " driver_gaps="
                      << counters.driver_sequence_gaps.load(std::memory_order_relaxed)
                      << '\n';
            next_status += std::chrono::seconds(1);
        }

        running.store(false, std::memory_order_relaxed);
        producer.join();
        queue.close();
        consumer.join();
        capture.close();

        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started_at).count();
        const uint64_t captured = counters.captured.load(std::memory_order_relaxed);
        const uint64_t bytes = counters.bytes_captured.load(std::memory_order_relaxed);
        const uint64_t first_dequeue =
            counters.first_dequeue_timestamp_us.load(std::memory_order_relaxed);
        const uint64_t last_dequeue =
            counters.last_dequeue_timestamp_us.load(std::memory_order_relaxed);
        const uint64_t first_driver =
            counters.first_driver_timestamp_us.load(std::memory_order_relaxed);
        const uint64_t last_driver =
            counters.last_driver_timestamp_us.load(std::memory_order_relaxed);
        const double dequeue_span_s = last_dequeue > first_dequeue
            ? (last_dequeue - first_dequeue) / 1'000'000.0
            : 0.0;
        const double driver_span_s = last_driver > first_driver
            ? (last_driver - first_driver) / 1'000'000.0
            : 0.0;
        const double dequeue_fps = captured > 1 && dequeue_span_s > 0
            ? (captured - 1) / dequeue_span_s
            : 0.0;
        const double driver_fps = captured > 1 && driver_span_s > 0
            ? (captured - 1) / driver_span_s
            : 0.0;
        std::cout << std::fixed << std::setprecision(3)
                  << "SUMMARY elapsed_s=" << elapsed
                  << " captured=" << captured
                  << " consumed=" << counters.consumed.load(std::memory_order_relaxed)
                  << " capture_fps=" << (elapsed > 0 ? captured / elapsed : 0.0)
                  << " active_capture_fps=" << dequeue_fps
                  << " driver_timestamp_fps=" << driver_fps
                  << " data_mib_s=" << (elapsed > 0 ? bytes / elapsed / 1024.0 / 1024.0 : 0.0)
                  << " queue_high_watermark=" << queue.high_watermark()
                  << " drop_oldest=" << queue.dropped_count()
                  << " driver_sequence_gaps="
                  << counters.driver_sequence_gaps.load(std::memory_order_relaxed)
                  << " poll_timeouts=" << counters.poll_timeouts.load(std::memory_order_relaxed)
                  << " max_queue_latency_ms="
                  << counters.max_queue_latency_us.load(std::memory_order_relaxed) / 1000.0
                  << " checksum=" << counters.checksum.load(std::memory_order_relaxed)
                  << " result=" << (fatal_error.load(std::memory_order_relaxed) ? "FAIL" : "PASS")
                  << '\n';
        return fatal_error.load(std::memory_order_relaxed) ? 4 : 0;
    } catch (const std::exception& exception) {
        std::cerr << "argument error: " << exception.what() << '\n';
        print_usage(argv[0]);
        return 1;
    }
}
