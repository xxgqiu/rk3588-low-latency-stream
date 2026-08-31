#include "rkstream/rtsp_publisher.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

namespace rkstream {
namespace {

std::string ffmpeg_error(const char* operation, int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    std::ostringstream stream;
    stream << operation << " failed: " << message << " (" << code << ')';
    return stream.str();
}

}  // namespace

class RtspPublisher::Impl {
public:
    bool open(const RtspPublisherConfig& config, const std::vector<uint8_t>& h264_header,
              std::string* error) {
        close();
        if (config.url.empty() || config.width == 0 || config.height == 0 ||
            config.fps == 0 || h264_header.empty() ||
            h264_header.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            if (error != nullptr) {
                *error = "invalid RTSP publisher configuration";
            }
            return false;
        }

        // FFmpeg 在这里是 muxer/network client，不参与 H.264 编码。
        config_ = config;
        int result = avformat_alloc_output_context2(
            &format_context_, nullptr, "rtsp", config.url.c_str());
        if (result < 0 || format_context_ == nullptr) {
            if (error != nullptr) {
                *error = ffmpeg_error("avformat_alloc_output_context2", result);
            }
            close();
            return false;
        }

        stream_ = avformat_new_stream(format_context_, nullptr);
        if (stream_ == nullptr) {
            if (error != nullptr) {
                *error = "avformat_new_stream returned null";
            }
            close();
            return false;
        }

        // RTP 视频通常使用 90 kHz 时钟。输入 pts 是微秒，write() 中统一 rescale。
        stream_->time_base = AVRational{1, 90'000};
        stream_->avg_frame_rate = AVRational{static_cast<int>(config.fps), 1};
        AVCodecParameters* parameters = stream_->codecpar;
        parameters->codec_type = AVMEDIA_TYPE_VIDEO;
        parameters->codec_id = AV_CODEC_ID_H264;
        parameters->codec_tag = 0;
        parameters->width = static_cast<int>(config.width);
        parameters->height = static_cast<int>(config.height);
        parameters->format = AV_PIX_FMT_YUV420P;
        // SPS/PPS 放进 extradata，RTSP/SDP 才能把解码参数传给后加入的客户端。
        parameters->extradata = static_cast<uint8_t*>(
            av_mallocz(h264_header.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (parameters->extradata == nullptr) {
            if (error != nullptr) {
                *error = "cannot allocate H.264 extradata";
            }
            close();
            return false;
        }
        std::memcpy(parameters->extradata, h264_header.data(), h264_header.size());
        parameters->extradata_size = static_cast<int>(h264_header.size());

        AVDictionary* options = nullptr;
        // TCP 优先保证局域网演示稳定；muxdelay=0 减少 muxer 人为缓存。
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "muxdelay", "0", 0);
        av_dict_set(&options, "rw_timeout", "5000000", 0);

        if ((format_context_->oformat->flags & AVFMT_NOFILE) == 0) {
            result = avio_open2(&format_context_->pb, config.url.c_str(), AVIO_FLAG_WRITE,
                               nullptr, &options);
            if (result < 0) {
                if (error != nullptr) {
                    *error = ffmpeg_error("avio_open2", result);
                }
                av_dict_free(&options);
                close();
                return false;
            }
        }

        result = avformat_write_header(format_context_, &options);
        av_dict_free(&options);
        if (result < 0) {
            if (error != nullptr) {
                *error = ffmpeg_error("avformat_write_header", result);
            }
            close();
            return false;
        }
        header_written_ = true;
        return true;
    }

    bool write(const EncodedPacket& encoded, std::string* error) {
        if (!header_written_ || format_context_ == nullptr || stream_ == nullptr ||
            encoded.bytes.empty() ||
            encoded.bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            if (error != nullptr) {
                *error = "RTSP publisher is not ready or packet is empty";
            }
            return false;
        }

        // 把任意单调时钟起点归零，避免把很大的绝对微秒值直接写入容器时间轴。
        if (!have_first_pts_) {
            first_pts_us_ = encoded.pts;
            have_first_pts_ = true;
        }
        const int64_t relative_pts_us = std::max<int64_t>(0, encoded.pts - first_pts_us_);

        // 编码线程拥有 std::vector；FFmpeg packet 需要自己的可写存储，因此这里复制
        // 压缩后的几 KB/几十 KB，而不是复制约 3 MB 的原始 NV12 图像。
        AVPacket* packet = av_packet_alloc();
        if (packet == nullptr || av_new_packet(packet, static_cast<int>(encoded.bytes.size())) < 0) {
            if (packet != nullptr) {
                av_packet_free(&packet);
            }
            if (error != nullptr) {
                *error = "cannot allocate FFmpeg packet";
            }
            return false;
        }
        std::memcpy(packet->data, encoded.bytes.data(), encoded.bytes.size());
        packet->stream_index = stream_->index;
        packet->pts = av_rescale_q(relative_pts_us, AVRational{1, 1'000'000}, stream_->time_base);
        packet->dts = packet->pts;
        packet->duration = av_rescale_q(1, AVRational{1, static_cast<int>(config_.fps)},
                                        stream_->time_base);
        // 某些驱动时间戳可能重复；RTSP muxer 要求单调递增，故做最小幅度修正。
        if (packet->pts <= last_pts_) {
            packet->pts = last_pts_ + std::max<int64_t>(packet->duration, 1);
            packet->dts = packet->pts;
        }
        const int64_t submitted_pts = packet->pts;
        if (encoded.keyframe) {
            packet->flags |= AV_PKT_FLAG_KEY;
        }

        const int result = av_interleaved_write_frame(format_context_, packet);
        av_packet_free(&packet);
        if (result < 0) {
            if (error != nullptr) {
                *error = ffmpeg_error("av_interleaved_write_frame", result);
            }
            return false;
        }
        last_pts_ = submitted_pts;
        return true;
    }

    void close() {
        // 先写 trailer，再关闭 AVIO，最后释放 format context；所有函数允许重复 close。
        if (format_context_ != nullptr && header_written_) {
            (void)av_write_trailer(format_context_);
        }
        header_written_ = false;
        if (format_context_ != nullptr && format_context_->pb != nullptr &&
            (format_context_->oformat->flags & AVFMT_NOFILE) == 0) {
            avio_closep(&format_context_->pb);
        }
        if (format_context_ != nullptr) {
            avformat_free_context(format_context_);
            format_context_ = nullptr;
        }
        stream_ = nullptr;
        config_ = {};
        have_first_pts_ = false;
        first_pts_us_ = 0;
        last_pts_ = -1;
    }

    ~Impl() {
        close();
    }

private:
    RtspPublisherConfig config_;
    AVFormatContext* format_context_ = nullptr;
    AVStream* stream_ = nullptr;
    bool header_written_ = false;
    bool have_first_pts_ = false;
    int64_t first_pts_us_ = 0;
    int64_t last_pts_ = -1;
};

RtspPublisher::RtspPublisher() : impl_(std::make_unique<Impl>()) {}

RtspPublisher::~RtspPublisher() = default;

bool RtspPublisher::open(const RtspPublisherConfig& config,
                         const std::vector<uint8_t>& h264_header, std::string* error) {
    return impl_->open(config, h264_header, error);
}

bool RtspPublisher::write(const EncodedPacket& packet, std::string* error) {
    return impl_->write(packet, error);
}

void RtspPublisher::close() {
    impl_->close();
}

}  // namespace rkstream
