#pragma once

#include "latest_frame_mailbox.h"
#include "rtsp_decode_thread.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace rkstream::qtclient {

// 主窗口只负责界面、同屏计时器和“取最新帧显示”。
// RTSP网络读取与H.264解码全部由 RtspDecodeThread 负责，避免阻塞GUI事件循环。
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(const QString& initial_url, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void start_playback();
    void stop_playback();
    void refresh_video_frame();
    void refresh_latency_clock();
    void save_measurement_snapshot();
    void on_status_changed(const QString& message);
    void on_stream_opened(int width, int height, double declared_fps);
    void on_stats_updated(const rkstream::qtclient::ClientStats& stats);
    void on_playback_finished(const QString& reason);

private:
    void build_ui();
    void set_controls_for_running(bool running);

    // mailbox_ 必须比 decoder_ 更早构造、更晚析构，因此声明顺序不能随意交换。
    LatestFrameMailbox mailbox_;
    RtspDecodeThread decoder_;

    QTimer video_refresh_timer_;
    QTimer clock_refresh_timer_;
    QElapsedTimer latency_clock_;
    quint64 displayed_generation_ = 0;

    QLineEdit* url_edit_ = nullptr;
    QComboBox* transport_combo_ = nullptr;
    QPushButton* connect_button_ = nullptr;
    QPushButton* disconnect_button_ = nullptr;
    QPushButton* snapshot_button_ = nullptr;
    QLabel* source_clock_label_ = nullptr;
    QLabel* video_label_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* stream_info_label_ = nullptr;
    QLabel* fps_label_ = nullptr;
    QLabel* bitrate_label_ = nullptr;
    QLabel* decode_label_ = nullptr;
    QLabel* packet_label_ = nullptr;
    QLabel* overwrite_label_ = nullptr;
};

}  // namespace rkstream::qtclient
