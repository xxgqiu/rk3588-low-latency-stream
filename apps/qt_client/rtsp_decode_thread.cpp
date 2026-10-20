#include "rtsp_decode_thread.h"

#include "latest_frame_mailbox.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <QImage>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

namespace rkstream::qtclient {
namespace {

// FFmpeg 大多数 API 失败时返回负整数。统一在这里转换，确保 GUI 能显示“哪个 API、
// 哪个错误码、FFmpeg 给出的文字”，而不是只有一个无法定位的“连接失败”。
QString ffmpeg_error(const char* operation, int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof(text));
    return QStringLiteral("%1失败：%2（%3）")
        .arg(QString::fromUtf8(operation), QString::fromUtf8(text))
        .arg(code);
}

}  // namespace

RtspDecodeThread::RtspDecodeThread(LatestFrameMailbox* mailbox)
    : mailbox_(mailbox) {}

RtspDecodeThread::~RtspDecodeThread() {
    // QThread 对象析构时底层线程必须已经结束。这里只请求停止并等待；绝不使用
    // terminate() 强杀线程，因为强杀可能让 FFmpeg 正持有 mutex/socket/heap 对象，
    // 留下资源泄漏甚至破坏进程状态。
    request_stop();
    wait();
}

bool RtspDecodeThread::configure(const QString& url, const QString& transport) {
    if (isRunning() || url.trimmed().isEmpty()) {
        return false;
    }
    const QString normalized_transport = transport.trimmed().toLower();
    if (normalized_transport != QStringLiteral("tcp") &&
        normalized_transport != QStringLiteral("udp")) {
        return false;
    }
    url_ = url.trimmed();
    transport_ = normalized_transport;
    stop_requested_.store(false, std::memory_order_release);
    return true;
}

void RtspDecodeThread::request_stop() {
    stop_requested_.store(true, std::memory_order_release);
}

int RtspDecodeThread::interrupt_callback(void* opaque) {
    // FFmpeg 在连接、读 socket、探测码流时周期性调用该回调。返回非 0 会让阻塞 API
    // 尽快以 AVERROR_EXIT 结束，这就是“点击断开后不必等网络超时”的关键。
    const auto* self = static_cast<const RtspDecodeThread*>(opaque);
    return self != nullptr && self->stop_requested_.load(std::memory_order_acquire) ? 1 : 0;
}

void RtspDecodeThread::run() {
    if (mailbox_ == nullptr) {
        emit playback_finished(QStringLiteral("内部错误：LatestFrameMailbox为空"));
        return;
    }

    // 这些对象全部只在当前解码线程创建、使用和释放，不跨线程传递 FFmpeg 裸指针。
    AVFormatContext* format_context = nullptr;
    AVCodecContext* decoder_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    SwsContext* sws_context = nullptr;

    // 所有正常/错误出口最终都调用 cleanup，释放顺序与创建顺序相反。
    const auto cleanup = [&]() {
        sws_freeContext(sws_context);
        sws_context = nullptr;
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder_context);
        if (format_context != nullptr) {
            avformat_close_input(&format_context);
        }
    };

    QString finish_reason;
    emit status_changed(QStringLiteral("正在连接 %1").arg(url_));

    // 网络模块全局初始化是幂等的；这里只初始化，不在单个会话结束时 deinit，避免未来
    // 多客户端场景下一个线程清掉另一个线程仍在使用的全局网络状态。
    avformat_network_init();
    format_context = avformat_alloc_context();
    if (format_context == nullptr) {
        emit playback_finished(QStringLiteral("avformat_alloc_context无法分配内存"));
        return;
    }
    format_context->interrupt_callback.callback = &RtspDecodeThread::interrupt_callback;
    format_context->interrupt_callback.opaque = this;
    format_context->flags |= AVFMT_FLAG_NOBUFFER;

    AVDictionary* options = nullptr;
    const QByteArray transport_utf8 = transport_.toUtf8();
    // rtsp_transport 决定 RTP 媒体走 TCP interleaved 还是 UDP；默认 TCP 与板端正式
    // MediaMTX 配置一致。其余选项减少客户端主动缓存，并给阻塞网络 I/O 设置 3 秒上限。
    av_dict_set(&options, "rtsp_transport", transport_utf8.constData(), 0);
    av_dict_set(&options, "fflags", "nobuffer", 0);
    av_dict_set(&options, "flags", "low_delay", 0);
    av_dict_set(&options, "max_delay", "0", 0);
    av_dict_set(&options, "probesize", "32768", 0);
    av_dict_set(&options, "analyzeduration", "0", 0);
    av_dict_set(&options, "rw_timeout", "3000000", 0);

    const QByteArray url_utf8 = url_.toUtf8();
    int result = avformat_open_input(&format_context, url_utf8.constData(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0) {
        finish_reason = ffmpeg_error("avformat_open_input", result);
        cleanup();
        emit playback_finished(finish_reason);
        return;
    }

    // RTSP 的 DESCRIBE/SDP 已经提供大量流信息，但仍调用 find_stream_info，让 FFmpeg
    // 完成标准化的 stream/codecpar 初始化。interrupt_callback 同样能中断这个阶段。
    result = avformat_find_stream_info(format_context, nullptr);
    if (result < 0) {
        finish_reason = ffmpeg_error("avformat_find_stream_info", result);
        cleanup();
        emit playback_finished(finish_reason);
        return;
    }

    const int video_stream_index = av_find_best_stream(
        format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        finish_reason = ffmpeg_error("av_find_best_stream(video)", video_stream_index);
        cleanup();
        emit playback_finished(finish_reason);
        return;
    }

    AVStream* video_stream = format_context->streams[video_stream_index];
    const AVCodec* decoder = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (decoder == nullptr) {
        finish_reason = QStringLiteral("找不到视频解码器，codec_id=%1")
                            .arg(video_stream->codecpar->codec_id);
        cleanup();
        emit playback_finished(finish_reason);
        return;
    }

    decoder_context = avcodec_alloc_context3(decoder);
    if (decoder_context == nullptr) {
        finish_reason = QStringLiteral("avcodec_alloc_context3无法分配内存");
        cleanup();
        emit playback_finished(finish_reason);
        return;
    }
    result = avcodec_parameters_to_context(decoder_context, video_stream->codecpar);
    if (result < 0) {
        finish_reason = ffmpeg_error("avcodec_parameters_to_context", result);
        cleanup();
        emit playback_finished(finish_reason);
        return;
    }
    decoder_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    result = avcodec_open2(decoder_context, decoder, nullptr);
    if (result < 0) {
        finish_reason = ffmpeg_error("avcodec_open2", result);
        cleanup();
        emit playback_finished(finish_reason);
        return;
    }

    packet = av_packet_alloc();
    frame = av_frame_alloc();
    if (packet == nullptr || frame == nullptr) {
        finish_reason = QStringLiteral("无法分配AVPacket或AVFrame");
        cleanup();
        emit playback_finished(finish_reason);
        return;
    }

    // “声明帧率”应优先展示发布端写入 SDP/AVStream 的 avg_frame_rate。
    // av_guess_frame_rate() 会结合 H.264 时序信息做推测，对某些没有 B 帧的实时码流
    // 可能把 24 FPS 推成 48 FPS。那不是实际解码输出速度，也不是摄像头速度。
    // 因此顺序是：avg_frame_rate -> r_frame_rate -> 最后才用 guess 兜底。
    AVRational declared_rate = video_stream->avg_frame_rate;
    if (declared_rate.num <= 0 || declared_rate.den <= 0) {
        declared_rate = video_stream->r_frame_rate;
    }
    if (declared_rate.num <= 0 || declared_rate.den <= 0) {
        declared_rate = av_guess_frame_rate(format_context, video_stream, nullptr);
    }
    const double declared_fps =
        declared_rate.num > 0 && declared_rate.den > 0 ? av_q2d(declared_rate) : 0.0;
    emit stream_opened(decoder_context->width, decoder_context->height, declared_fps);
    emit status_changed(QStringLiteral("RTSP已连接，正在解码"));

    using Clock = std::chrono::steady_clock;
    auto interval_started = Clock::now();
    quint64 total_packets = 0;
    quint64 total_frames = 0;
    quint64 decode_errors = 0;
    quint64 interval_bytes = 0;
    quint64 interval_frames = 0;
    double interval_decode_ms = 0.0;
    double interval_max_decode_ms = 0.0;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        result = av_read_frame(format_context, packet);
        if (result == AVERROR(EAGAIN)) {
            continue;
        }
        if (result < 0) {
            finish_reason = stop_requested_.load(std::memory_order_acquire)
                                ? QStringLiteral("用户请求停止")
                                : ffmpeg_error("av_read_frame", result);
            break;
        }

        if (packet->stream_index != video_stream_index) {
            av_packet_unref(packet);
            continue;
        }

        ++total_packets;
        interval_bytes += static_cast<quint64>(std::max(packet->size, 0));
        const auto decode_started = Clock::now();
        result = avcodec_send_packet(decoder_context, packet);
        // send_packet 成功后解码器持有自己需要的引用，因此当前循环可以立即 unref，
        // 让 AVPacket 在下一次 av_read_frame 前复用。
        av_packet_unref(packet);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            ++decode_errors;
            continue;
        }

        while (!stop_requested_.load(std::memory_order_acquire)) {
            result = avcodec_receive_frame(decoder_context, frame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            if (result < 0) {
                ++decode_errors;
                break;
            }

            // 解码器通常输出 YUV420P；Qt 的 QImage 直接显示 RGB888。sws_getCachedContext
            // 只在尺寸/像素格式变化时重建转换器，稳定播放期间复用内部表和缓冲。
            sws_context = sws_getCachedContext(
                sws_context,
                frame->width,
                frame->height,
                static_cast<AVPixelFormat>(frame->format),
                frame->width,
                frame->height,
                AV_PIX_FMT_RGB24,
                SWS_FAST_BILINEAR,
                nullptr,
                nullptr,
                nullptr);
            if (sws_context == nullptr) {
                ++decode_errors;
                av_frame_unref(frame);
                continue;
            }

            QImage rgb(frame->width, frame->height, QImage::Format_RGB888);
            if (rgb.isNull()) {
                ++decode_errors;
                av_frame_unref(frame);
                continue;
            }
            if (rgb.bytesPerLine() > std::numeric_limits<int>::max()) {
                ++decode_errors;
                av_frame_unref(frame);
                continue;
            }
            uint8_t* destination_data[4] = {rgb.bits(), nullptr, nullptr, nullptr};
            int destination_linesize[4] = {
                static_cast<int>(rgb.bytesPerLine()), 0, 0, 0};
            const int converted_rows = sws_scale(
                sws_context,
                frame->data,
                frame->linesize,
                0,
                frame->height,
                destination_data,
                destination_linesize);
            if (converted_rows != frame->height) {
                ++decode_errors;
                av_frame_unref(frame);
                continue;
            }

            const double decode_ms = std::chrono::duration<double, std::milli>(
                                         Clock::now() - decode_started)
                                         .count();
            interval_decode_ms += decode_ms;
            interval_max_decode_ms = std::max(interval_max_decode_ms, decode_ms);
            ++interval_frames;
            ++total_frames;

            // publish() 发生在解码线程，GUI 通过自己的刷新定时器主动取帧。这里不逐帧
            // emit QImage，避免 Qt 事件队列在 GUI 忙时悄悄堆积数十张旧图。
            mailbox_->publish(std::move(rgb));
            av_frame_unref(frame);
        }

        const auto now = Clock::now();
        const double interval_seconds =
            std::chrono::duration<double>(now - interval_started).count();
        if (interval_seconds >= 1.0) {
            ClientStats stats;
            stats.input_fps = static_cast<double>(interval_frames) / interval_seconds;
            stats.bitrate_mbps =
                static_cast<double>(interval_bytes) * 8.0 / interval_seconds / 1'000'000.0;
            stats.average_decode_ms =
                interval_frames > 0 ? interval_decode_ms / static_cast<double>(interval_frames)
                                    : 0.0;
            stats.maximum_decode_ms = interval_max_decode_ms;
            stats.total_packets = total_packets;
            stats.total_frames = total_frames;
            stats.decode_errors = decode_errors;
            stats.mailbox_overwrites = mailbox_->overwritten_count();
            emit stats_updated(stats);

            interval_started = now;
            interval_bytes = 0;
            interval_frames = 0;
            interval_decode_ms = 0.0;
            interval_max_decode_ms = 0.0;
        }
    }

    if (finish_reason.isEmpty()) {
        finish_reason = QStringLiteral("解码线程已停止");
    }
    cleanup();
    emit playback_finished(finish_reason);
}

}  // namespace rkstream::qtclient
