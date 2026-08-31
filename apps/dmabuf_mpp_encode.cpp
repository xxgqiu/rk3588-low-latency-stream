#include "rkstream/annexb.h"
#include "rkstream/mpp_h264_encoder.h"
#include "rkstream/rga_nv12_aligner.h"
#include "rkstream/v4l2_dmabuf_capture.h"

#include <linux/videodev2.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/resource.h>

/*
 * DMA-BUF/RGA 持续编码基准：V4L2 lease -> RGA -> MPP -> Annex-B 文件。
 *
 * 与 mpp_file_encode.cpp 输出同类码流，但原始像素不进入 std::vector，也不经过
 * CPU memcpy。它用于在引入 RTSP 三线程前单独验证：每帧 lease 都被归还、RGA/MPP
 * 可持续运行、输出可完整解码，以及 CPU/RSS/阶段耗时确实优于基线。
 */
namespace {

struct Options {
    std::string device = "/dev/video-camera0";
    std::string output = "/home/cat/Build/rk3588-low-latency-stream/ov8858_dmabuf_rga.h264";
    uint32_t frame_count = 240;
    uint32_t encoder_fps = 24;
    uint32_t bitrate = 4'000'000;
    uint32_t gop = 48;
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

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--help") {
            std::cout
                << "Usage: " << argv[0] << " [options]\n"
                << "  --device PATH     OV8858 NV12 capture device\n"
                << "  --output PATH     Annex-B H.264 output\n"
                << "  --frames N        Number of frames\n"
                << "  --fps N           Encoder frame rate\n"
                << "  --bitrate N       Target bitrate\n"
                << "  --gop N           GOP length\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + argument);
        }
        const char* value = argv[++index];
        if (argument == "--device") {
            options.device = value;
        } else if (argument == "--output") {
            options.output = value;
        } else if (argument == "--frames") {
            options.frame_count = parse_u32(value, "--frames");
        } else if (argument == "--fps") {
            options.encoder_fps = parse_u32(value, "--fps");
        } else if (argument == "--bitrate") {
            options.bitrate = parse_u32(value, "--bitrate");
        } else if (argument == "--gop") {
            options.gop = parse_u32(value, "--gop");
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    return options;
}

void merge_stats(rkstream::AnnexBStats* total, const rkstream::AnnexBStats& current) {
    total->nal_units += current.nal_units;
    total->sps += current.sps;
    total->pps += current.pps;
    total->idr += current.idr;
}

double timeval_seconds(const timeval& value) {
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::string error;

        rkstream::V4L2DmabufCapture capture;
        const rkstream::CaptureConfig capture_config{
            options.device, 1632, 1224, 30, V4L2_PIX_FMT_NV12, 4,
        };
        if (!capture.open(capture_config, &error)) {
            std::cerr << "V4L2 DMA-BUF initialization failed: " << error << '\n';
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
        if (encoder.input_dmabuf_fd() < 0) {
            std::cerr << "MPP input buffer does not expose a DMA-BUF fd\n";
            return 3;
        }

        rkstream::RgaNv12Aligner rga;
        if (!rga.open(capture_info, capture.buffers(), encoder.input_dmabuf_fd(),
                      encoder.info(), &error)) {
            std::cerr << "RGA initialization failed: " << error << '\n';
            return 4;
        }
        if (!capture.start(&error)) {
            std::cerr << "V4L2 stream start failed: " << error << '\n';
            return 2;
        }

        std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "cannot open output file: " << options.output << '\n';
            return 5;
        }
        output.write(reinterpret_cast<const char*>(encoder.header().data()),
                     static_cast<std::streamsize>(encoder.header().size()));
        rkstream::AnnexBStats stats =
            rkstream::inspect_annexb(encoder.header().data(), encoder.header().size());

        std::cout << "DMABUF_ENCODER_READY"
                  << " source=" << capture_info.width << 'x' << capture_info.height
                  << " source_stride=" << capture_info.bytes_per_line << 'x'
                  << capture_info.height
                  << " source_buffers=" << capture.buffers().size()
                  << " mpp_stride=" << encoder.info().horizontal_stride << 'x'
                  << encoder.info().vertical_stride
                  << " mpp_input_bytes=" << encoder.info().input_buffer_bytes
                  << " mpp_dma_fd=" << encoder.input_dmabuf_fd()
                  << " bitrate=" << options.bitrate
                  << " fps=" << options.encoder_fps
                  << " gop=" << options.gop << '\n';

        rusage usage_start{};
        (void)getrusage(RUSAGE_SELF, &usage_start);
        const auto run_started = std::chrono::steady_clock::now();
        uint64_t encoded_bytes = encoder.header().size();
        uint64_t capture_timeouts = 0;
        uint64_t driver_gaps = 0;
        uint64_t total_rga_us = 0;
        uint64_t max_rga_us = 0;
        uint64_t total_mpp_us = 0;
        uint64_t max_mpp_us = 0;
        uint32_t encoded_frames = 0;
        uint32_t previous_sequence = 0;
        bool have_previous_sequence = false;

        while (encoded_frames < options.frame_count) {
            rkstream::DmabufFrame frame;
            const rkstream::CaptureResult result = capture.capture(&frame, 1000, &error);
            if (result == rkstream::CaptureResult::kTimeout) {
                ++capture_timeouts;
                continue;
            }
            if (result == rkstream::CaptureResult::kInterrupted) {
                continue;
            }
            if (result == rkstream::CaptureResult::kFatal) {
                std::cerr << "capture failed: " << error << '\n';
                return 6;
            }
            if (have_previous_sequence) {
                const uint32_t delta = frame.sequence - previous_sequence;
                if (delta > 1) {
                    driver_gaps += delta - 1;
                }
            }
            previous_sequence = frame.sequence;
            have_previous_sequence = true;

            const auto rga_started = std::chrono::steady_clock::now();
            const bool copied = rga.copy(frame.buffer_index, &error);
            const uint64_t rga_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - rga_started).count());
            total_rga_us += rga_us;
            max_rga_us = rga_us > max_rga_us ? rga_us : max_rga_us;
            if (!copied) {
                (void)capture.release(frame, nullptr);
                std::cerr << error << '\n';
                return 7;
            }

            // RGA 同步拷贝已完成，编码器只会读取独立的 MPP buffer；尽早归还相机 buffer。
            if (!capture.release(frame, &error)) {
                std::cerr << "camera buffer release failed: " << error << '\n';
                return 8;
            }

            rkstream::EncodedPacket packet;
            const auto mpp_started = std::chrono::steady_clock::now();
            if (!encoder.encode_prepared_nv12(static_cast<int64_t>(frame.driver_timestamp_us),
                                              &packet, &error)) {
                std::cerr << "encode failed: " << error << '\n';
                return 9;
            }
            const uint64_t mpp_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - mpp_started).count());
            total_mpp_us += mpp_us;
            max_mpp_us = mpp_us > max_mpp_us ? mpp_us : max_mpp_us;

            output.write(reinterpret_cast<const char*>(packet.bytes.data()),
                         static_cast<std::streamsize>(packet.bytes.size()));
            if (!output) {
                std::cerr << "failed to write H.264 output\n";
                return 10;
            }
            encoded_bytes += packet.bytes.size();
            merge_stats(&stats,
                        rkstream::inspect_annexb(packet.bytes.data(), packet.bytes.size()));
            ++encoded_frames;
        }

        output.flush();
        output.close();
        const double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - run_started).count();
        rusage usage_end{};
        (void)getrusage(RUSAGE_SELF, &usage_end);
        const double user_cpu_s = timeval_seconds(usage_end.ru_utime) -
                                  timeval_seconds(usage_start.ru_utime);
        const double system_cpu_s = timeval_seconds(usage_end.ru_stime) -
                                    timeval_seconds(usage_start.ru_stime);
        const double cpu_percent = elapsed_s > 0
                                       ? (user_cpu_s + system_cpu_s) / elapsed_s * 100.0
                                       : 0.0;
        const double measured_bitrate = elapsed_s > 0 ? encoded_bytes * 8.0 / elapsed_s : 0.0;
        const bool pass = stats.sps > 0 && stats.pps > 0 && stats.idr > 0 &&
                          encoded_frames == options.frame_count && driver_gaps == 0;

        std::cout << std::fixed << std::setprecision(3)
                  << "DMABUF_ENCODE_SUMMARY result=" << (pass ? "PASS" : "FAIL")
                  << " frames=" << encoded_frames
                  << " elapsed_s=" << elapsed_s
                  << " pipeline_fps=" << (elapsed_s > 0 ? encoded_frames / elapsed_s : 0.0)
                  << " avg_rga_ms="
                  << (encoded_frames > 0 ? total_rga_us / 1000.0 / encoded_frames : 0.0)
                  << " max_rga_ms=" << max_rga_us / 1000.0
                  << " avg_mpp_ms="
                  << (encoded_frames > 0 ? total_mpp_us / 1000.0 / encoded_frames : 0.0)
                  << " max_mpp_ms=" << max_mpp_us / 1000.0
                  << " avg_rga_plus_mpp_ms="
                  << (encoded_frames > 0
                          ? (total_rga_us + total_mpp_us) / 1000.0 / encoded_frames
                          : 0.0)
                  << " output_bytes=" << encoded_bytes
                  << " measured_bitrate=" << measured_bitrate
                  << " sps=" << stats.sps
                  << " pps=" << stats.pps
                  << " idr=" << stats.idr
                  << " driver_gaps=" << driver_gaps
                  << " capture_timeouts=" << capture_timeouts
                  << " user_cpu_s=" << user_cpu_s
                  << " system_cpu_s=" << system_cpu_s
                  << " cpu_percent=" << cpu_percent
                  << " max_rss_kib=" << usage_end.ru_maxrss
                  << " output=" << options.output << '\n';
        return pass ? 0 : 11;
    } catch (const std::exception& exception) {
        std::cerr << "argument error: " << exception.what() << '\n';
        return 1;
    }
}
