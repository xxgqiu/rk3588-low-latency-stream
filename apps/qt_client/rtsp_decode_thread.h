#pragma once

#include <QMetaType>
#include <QString>
#include <QThread>
#include <QtGlobal>

#include <atomic>

namespace rkstream::qtclient {

class LatestFrameMailbox;

// 每秒从解码线程发送给 GUI 一次的观测数据。
//
// input_fps 表示这一秒实际从解码器输出多少张图；bitrate_mbps 按收到的视频 AVPacket
// 字节数估算；decode_ms 只覆盖 avcodec_send_packet/receive_frame 到取得解码帧的阶段，
// 不等于摄像头到显示器的端到端延迟。端到端延迟由同屏拍照法单独测量。
struct ClientStats {
    double input_fps = 0.0;
    double bitrate_mbps = 0.0;
    double average_decode_ms = 0.0;
    double maximum_decode_ms = 0.0;
    quint64 total_packets = 0;
    quint64 total_frames = 0;
    quint64 decode_errors = 0;
    quint64 mailbox_overwrites = 0;
};

// RTSP 拉流和 H.264 解码必须放到后台线程。
//
// avformat_open_input()、av_read_frame() 和 H.264 解码都可能阻塞或耗时；如果在 Qt
// 主线程调用，窗口会卡住、按钮无法点击、计时器停止刷新。这个类把完整 FFmpeg 循环
// 放进 QThread::run()，GUI 只通过 LatestFrameMailbox 取最新图像。
class RtspDecodeThread final : public QThread {
    Q_OBJECT

public:
    explicit RtspDecodeThread(LatestFrameMailbox* mailbox);
    ~RtspDecodeThread() override;

    RtspDecodeThread(const RtspDecodeThread&) = delete;
    RtspDecodeThread& operator=(const RtspDecodeThread&) = delete;

    // 只能在线程尚未运行时配置。transport 当前接受 tcp 或 udp。
    bool configure(const QString& url, const QString& transport);

    // 只设置原子停止标志。FFmpeg interrupt_callback 会检查它并打断阻塞中的网络调用。
    // request_stop() 不负责等待线程结束；调用方随后还要 wait()/join。
    void request_stop();

signals:
    void status_changed(const QString& message);
    void stream_opened(int width, int height, double declared_fps);
    void stats_updated(const rkstream::qtclient::ClientStats& stats);
    void playback_finished(const QString& reason);

protected:
    void run() override;

private:
    static int interrupt_callback(void* opaque);

    LatestFrameMailbox* mailbox_ = nullptr;  // 不拥有；MainWindow 保证其生命周期更长。
    QString url_;
    QString transport_ = QStringLiteral("tcp");
    std::atomic<bool> stop_requested_{false};
};

}  // namespace rkstream::qtclient

// queued signal 跨线程传递自定义结构体时，Qt 元对象系统必须知道该类型。
Q_DECLARE_METATYPE(rkstream::qtclient::ClientStats)
