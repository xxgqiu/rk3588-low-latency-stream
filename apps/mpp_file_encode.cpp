#include "rkstream/annexb.h"
#include "rkstream/mpp_h264_encoder.h"
#include "rkstream/v4l2_capture.h"

#include <linux/videodev2.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>

/*
 * M2 CPU-copy 硬编码基线：V4L2 MMAP -> Frame::data -> MPP -> Annex-B .h264。
 *
 * 先输出文件而不是直接做网络，是为了把问题分层：若 ffprobe/ffmpeg 能完整解析和
 * 解码该文件，就证明摄像头格式、NV12 stride、MPP 参数以及 SPS/PPS/IDR 均正确；
 * 后续 RTSP 故障即可优先定位到 mux/network，而不是重新怀疑编码器。
 */
namespace {

struct Options {
    std::string device = "/dev/video-camera0";
    std::string output = "/home/cat/Build/rk3588-low-latency-stream/ov8858_mpp.h264";
    uint32_t frame_count = 96;
    uint32_t encoder_fps = 24;
    uint32_t bitrate = 4'000'000;
    uint32_t gop = 48;
};

uint32_t parse_u32(const char* value, const char* option) {
    const std::string text(value);
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(text, &consumed, 10);
    if (consumed != text.size() || parsed == 0 || parsed > UINT32_MAX) {
        throw std::invalid_argument(std::string("invalid value for ") + option + ": " + text);
    }
    return static_cast<uint32_t>(parsed);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
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

        rkstream::V4L2Capture capture;
        const rkstream::CaptureConfig capture_config{
            options.device,
            1632,
            1224,
            30,
            V4L2_PIX_FMT_NV12,
            4,
        };
        if (!capture.open(capture_config, &error) || !capture.start(&error)) {
            std::cerr << "V4L2 initialization failed: " << error << '\n';
            return 2;
        }
        const auto capture_info = capture.info();

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

        std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "cannot open output file: " << options.output << '\n';
            return 4;
        }

        output.write(reinterpret_cast<const char*>(encoder.header().data()),
                     static_cast<std::streamsize>(encoder.header().size()));
        rkstream::AnnexBStats stats = rkstream::inspect_annexb(
            encoder.header().data(), encoder.header().size());

        const auto& encoder_info = encoder.info();
        std::cout << "ENCODER_READY"
                  << " source=" << capture_info.width << 'x' << capture_info.height
                  << " source_stride=" << capture_info.bytes_per_line
                  << " mpp_stride=" << encoder_info.horizontal_stride << 'x'
                  << encoder_info.vertical_stride
                  << " input_bytes=" << encoder_info.input_buffer_bytes
                  << " header_bytes=" << encoder_info.header_bytes
                  << " bitrate=" << options.bitrate
                  << " fps=" << options.encoder_fps
                  << " gop=" << options.gop << '\n';

        uint64_t encoded_bytes = encoder.header().size();
        uint64_t capture_timeouts = 0;
        double total_encode_ms = 0.0;
        double max_encode_ms = 0.0;
        uint32_t encoded_frames = 0;
        uint64_t driver_gaps = 0;
        uint32_t previous_sequence = 0;
        bool have_previous_sequence = false;
        rusage usage_start{};
        (void)getrusage(RUSAGE_SELF, &usage_start);
        const auto run_started = std::chrono::steady_clock::now();

        while (encoded_frames < options.frame_count) {
            rkstream::Frame frame;
            const rkstream::CaptureResult capture_result = capture.capture(&frame, 1000, &error);
            if (capture_result == rkstream::CaptureResult::kTimeout) {
                ++capture_timeouts;
                continue;
            }
            if (capture_result == rkstream::CaptureResult::kInterrupted) {
                continue;
            }
            if (capture_result == rkstream::CaptureResult::kFatal) {
                std::cerr << "capture failed: " << error << '\n';
                return 5;
            }
            if (have_previous_sequence) {
                const uint32_t delta = frame.sequence - previous_sequence;
                if (delta > 1) {
                    driver_gaps += delta - 1;
                }
            }
            previous_sequence = frame.sequence;
            have_previous_sequence = true;

            rkstream::EncodedPacket packet;
            const auto encode_started = std::chrono::steady_clock::now();
            if (!encoder.encode_nv12(frame.data.data(), frame.data.size(),
                                     static_cast<int64_t>(frame.driver_timestamp_us),
                                     &packet, &error)) {
                std::cerr << "encode failed: " << error << '\n';
                return 6;
            }
            const double encode_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - encode_started).count();
            total_encode_ms += encode_ms;
            max_encode_ms = encode_ms > max_encode_ms ? encode_ms : max_encode_ms;

            output.write(reinterpret_cast<const char*>(packet.bytes.data()),
                         static_cast<std::streamsize>(packet.bytes.size()));
            if (!output) {
                std::cerr << "failed to write H.264 output\n";
                return 7;
            }
            encoded_bytes += packet.bytes.size();
            merge_stats(&stats, rkstream::inspect_annexb(packet.bytes.data(), packet.bytes.size()));
            ++encoded_frames;
        }

        output.flush();
        output.close();
        capture.close();
        encoder.close();

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
        const double bitrate = elapsed_s > 0 ? encoded_bytes * 8.0 / elapsed_s : 0.0;
        std::cout << std::fixed << std::setprecision(3)
                  << "ENCODE_SUMMARY result="
                  << ((stats.sps > 0 && stats.pps > 0 && stats.idr > 0) ? "PASS" : "FAIL")
                  << " frames=" << encoded_frames
                  << " elapsed_s=" << elapsed_s
                  << " pipeline_fps=" << (elapsed_s > 0 ? encoded_frames / elapsed_s : 0.0)
                  << " avg_encode_ms="
                  << (encoded_frames > 0 ? total_encode_ms / encoded_frames : 0.0)
                  << " max_encode_ms=" << max_encode_ms
                  << " output_bytes=" << encoded_bytes
                  << " measured_bitrate=" << bitrate
                  << " nal_units=" << stats.nal_units
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
        return (stats.sps > 0 && stats.pps > 0 && stats.idr > 0) ? 0 : 8;
    } catch (const std::exception& exception) {
        std::cerr << "argument error: " << exception.what() << '\n';
        return 1;
    }
}
