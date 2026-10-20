#include "main_window.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWidget>

namespace rkstream::qtclient {
namespace {

QLabel* make_metric_value() {
    auto* label = new QLabel(QStringLiteral("--"));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setMinimumWidth(140);
    return label;
}

}  // namespace

MainWindow::MainWindow(const QString& initial_url, QWidget* parent)
    : QMainWindow(parent), decoder_(&mailbox_) {
    build_ui();
    url_edit_->setText(initial_url);

    // 解码线程只发送低频状态/指标信号；视频帧本体通过容量1的 mailbox 传递。
    connect(&decoder_, &RtspDecodeThread::status_changed,
            this, &MainWindow::on_status_changed);
    connect(&decoder_, &RtspDecodeThread::stream_opened,
            this, &MainWindow::on_stream_opened);
    connect(&decoder_, &RtspDecodeThread::stats_updated,
            this, &MainWindow::on_stats_updated);
    connect(&decoder_, &RtspDecodeThread::playback_finished,
            this, &MainWindow::on_playback_finished);

    connect(connect_button_, &QPushButton::clicked, this, &MainWindow::start_playback);
    connect(disconnect_button_, &QPushButton::clicked, this, &MainWindow::stop_playback);
    connect(snapshot_button_, &QPushButton::clicked,
            this, &MainWindow::save_measurement_snapshot);

    // 15 ms约等于66 Hz。显示定时器可以比约24 FPS的视频快，但 take_latest() 只在
    // 有新帧时工作，不会重复转换同一帧。PreciseTimer减少操作系统主动合并定时器造成的抖动。
    video_refresh_timer_.setTimerType(Qt::PreciseTimer);
    video_refresh_timer_.setInterval(15);
    connect(&video_refresh_timer_, &QTimer::timeout,
            this, &MainWindow::refresh_video_frame);
    video_refresh_timer_.start();

    // 同屏时钟每10 ms更新。普通60 Hz显示器真正能呈现的时间粒度约16.7 ms，所以
    // 三位毫秒不代表1 ms测量精度；它只是让拍照读数方便，误差要在报告中单独说明。
    latency_clock_.start();
    clock_refresh_timer_.setTimerType(Qt::PreciseTimer);
    clock_refresh_timer_.setInterval(10);
    connect(&clock_refresh_timer_, &QTimer::timeout,
            this, &MainWindow::refresh_latency_clock);
    clock_refresh_timer_.start();
    refresh_latency_clock();

    set_controls_for_running(false);
    resize(1500, 860);
}

MainWindow::~MainWindow() {
    // 析构前先结束后台线程；否则 decoder_ 可能继续向即将销毁的 mailbox_ 写帧。
    decoder_.request_stop();
    decoder_.wait();
}

void MainWindow::build_ui() {
    setWindowTitle(QStringLiteral("RK3588低延迟RTSP客户端与拍照法测量工具"));

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    // 顶部连接栏：URL与transport都显式展示，避免把测试条件藏在代码里。
    auto* controls = new QHBoxLayout();
    controls->addWidget(new QLabel(QStringLiteral("RTSP地址：")));
    url_edit_ = new QLineEdit();
    url_edit_->setPlaceholderText(QStringLiteral("rtsp://10.42.0.81:8554/live"));
    controls->addWidget(url_edit_, 1);
    controls->addWidget(new QLabel(QStringLiteral("媒体传输：")));
    transport_combo_ = new QComboBox();
    transport_combo_->addItem(QStringLiteral("TCP（正式基线）"), QStringLiteral("tcp"));
    transport_combo_->addItem(QStringLiteral("UDP（需服务端另行启用）"), QStringLiteral("udp"));
    controls->addWidget(transport_combo_);
    connect_button_ = new QPushButton(QStringLiteral("连接"));
    disconnect_button_ = new QPushButton(QStringLiteral("断开"));
    snapshot_button_ = new QPushButton(QStringLiteral("保存同屏截图"));
    controls->addWidget(connect_button_);
    controls->addWidget(disconnect_button_);
    controls->addWidget(snapshot_button_);
    root->addLayout(controls);

    auto* content = new QHBoxLayout();

    // 左侧就是给摄像头拍摄的“源时钟”。镜头只需覆盖黑色计时区域，尽量不要把右侧
    // 返回视频也拍进去，否则会出现递归画面，影响读数。
    auto* clock_group = new QGroupBox(QStringLiteral("拍照法源时钟（让摄像头拍摄此区域）"));
    auto* clock_layout = new QVBoxLayout(clock_group);
    auto* clock_help = new QLabel(QStringLiteral(
        "源时钟是当前值；右侧视频中的时钟是经过摄像头、编码、网络、解码后的旧值。\n"
        "两者差值就是本次屏幕时钟拍照法的端到端延迟估计。"));
    clock_help->setWordWrap(true);
    clock_layout->addWidget(clock_help);
    source_clock_label_ = new QLabel(QStringLiteral("000:00.000"));
    source_clock_label_->setAlignment(Qt::AlignCenter);
    source_clock_label_->setMinimumSize(430, 250);
    source_clock_label_->setStyleSheet(
        QStringLiteral("QLabel { background: #050505; color: #f4f4f4; "
                       "border: 3px solid #38bdf8; border-radius: 8px; }"));
    QFont clock_font(QStringLiteral("DejaVu Sans Mono"));
    clock_font.setStyleHint(QFont::Monospace);
    clock_font.setBold(true);
    clock_font.setPointSize(48);
    source_clock_label_->setFont(clock_font);
    clock_layout->addWidget(source_clock_label_, 1);
    auto* uncertainty = new QLabel(QStringLiteral(
        "说明：建议至少拍30张，报告中给出中位数/P95；显示器刷新、摄像头帧周期和滚动快门"
        "都会形成测量不确定度，不能把末三位当作1 ms精度。"));
    uncertainty->setWordWrap(true);
    clock_layout->addWidget(uncertainty);
    content->addWidget(clock_group, 0);

    auto* video_group = new QGroupBox(QStringLiteral("RTSP返回画面"));
    auto* video_layout = new QVBoxLayout(video_group);
    video_label_ = new QLabel(QStringLiteral("尚未连接"));
    video_label_->setAlignment(Qt::AlignCenter);
    video_label_->setMinimumSize(760, 540);
    video_label_->setStyleSheet(
        QStringLiteral("QLabel { background: #111827; color: #cbd5e1; border: 1px solid #475569; }"));
    video_layout->addWidget(video_label_, 1);
    content->addWidget(video_group, 1);
    root->addLayout(content, 1);

    auto* metrics = new QGroupBox(QStringLiteral("客户端实时观测（不是光学端到端延迟）"));
    auto* grid = new QGridLayout(metrics);
    status_label_ = make_metric_value();
    stream_info_label_ = make_metric_value();
    fps_label_ = make_metric_value();
    bitrate_label_ = make_metric_value();
    decode_label_ = make_metric_value();
    packet_label_ = make_metric_value();
    overwrite_label_ = make_metric_value();
    grid->addWidget(new QLabel(QStringLiteral("状态")), 0, 0);
    grid->addWidget(status_label_, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("码流")), 0, 2);
    grid->addWidget(stream_info_label_, 0, 3);
    grid->addWidget(new QLabel(QStringLiteral("解码输出FPS")), 1, 0);
    grid->addWidget(fps_label_, 1, 1);
    grid->addWidget(new QLabel(QStringLiteral("接收码率")), 1, 2);
    grid->addWidget(bitrate_label_, 1, 3);
    grid->addWidget(new QLabel(QStringLiteral("解码耗时")), 2, 0);
    grid->addWidget(decode_label_, 2, 1);
    grid->addWidget(new QLabel(QStringLiteral("累计包/帧/错误")), 2, 2);
    grid->addWidget(packet_label_, 2, 3);
    grid->addWidget(new QLabel(QStringLiteral("显示来不及覆盖帧")), 3, 0);
    grid->addWidget(overwrite_label_, 3, 1);
    root->addWidget(metrics);

    setCentralWidget(central);
}

void MainWindow::start_playback() {
    if (decoder_.isRunning()) {
        return;
    }
    const QString url = url_edit_->text().trimmed();
    const QString transport = transport_combo_->currentData().toString();
    if (!decoder_.configure(url, transport)) {
        status_label_->setText(QStringLiteral("URL或传输参数无效"));
        return;
    }

    mailbox_.clear();
    video_label_->setText(QStringLiteral("正在连接……"));
    set_controls_for_running(true);
    decoder_.start();
}

void MainWindow::stop_playback() {
    if (!decoder_.isRunning()) {
        set_controls_for_running(false);
        return;
    }
    status_label_->setText(QStringLiteral("正在协调线程退出……"));
    decoder_.request_stop();
    // wait相当于std::thread::join：它只等待，不发停止命令，所以前一行request_stop不可少。
    if (!decoder_.wait(5000)) {
        status_label_->setText(QStringLiteral("停止超过5秒，仍在等待FFmpeg退出"));
        decoder_.wait();
    }
    set_controls_for_running(false);
}

void MainWindow::refresh_video_frame() {
    QImage image;
    if (!mailbox_.take_latest(&displayed_generation_, &image)) {
        return;
    }

    // FastTransformation的缩放质量略低于SmoothTransformation，但计算更少、不会为了预览
    // 增加额外显示排队。原始解码图像本身没有被改写。
    const QPixmap pixmap = QPixmap::fromImage(image);
    video_label_->setPixmap(pixmap.scaled(
        video_label_->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
}

void MainWindow::refresh_latency_clock() {
    const qint64 elapsed_ms = latency_clock_.elapsed();
    const qint64 minutes = elapsed_ms / 60'000;
    const qint64 seconds = (elapsed_ms / 1'000) % 60;
    const qint64 milliseconds = elapsed_ms % 1'000;
    source_clock_label_->setText(
        QStringLiteral("%1:%2.%3")
            .arg(minutes, 3, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'))
            .arg(milliseconds, 3, 10, QChar('0')));
}

void MainWindow::save_measurement_snapshot() {
    QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (directory.isEmpty()) {
        directory = QDir::homePath();
    }
    directory = QDir(directory).filePath(QStringLiteral("rkstream-latency"));
    QDir().mkpath(directory);
    const QString filename = QStringLiteral("latency_%1.png").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")));
    const QString path = QDir(directory).filePath(filename);
    if (grab().save(path, "PNG")) {
        status_label_->setText(QStringLiteral("截图已保存：%1").arg(path));
    } else {
        status_label_->setText(QStringLiteral("截图保存失败：%1").arg(path));
    }
}

void MainWindow::on_status_changed(const QString& message) {
    status_label_->setText(message);
}

void MainWindow::on_stream_opened(int width, int height, double declared_fps) {
    stream_info_label_->setText(
        QStringLiteral("%1×%2，声明%3 FPS").arg(width).arg(height).arg(declared_fps, 0, 'f', 2));
}

void MainWindow::on_stats_updated(const ClientStats& stats) {
    fps_label_->setText(QStringLiteral("%1 FPS").arg(stats.input_fps, 0, 'f', 2));
    bitrate_label_->setText(QStringLiteral("%1 Mbps").arg(stats.bitrate_mbps, 0, 'f', 3));
    decode_label_->setText(QStringLiteral("平均%1 ms，最大%2 ms")
                               .arg(stats.average_decode_ms, 0, 'f', 3)
                               .arg(stats.maximum_decode_ms, 0, 'f', 3));
    packet_label_->setText(QStringLiteral("%1 / %2 / %3")
                               .arg(stats.total_packets)
                               .arg(stats.total_frames)
                               .arg(stats.decode_errors));
    overwrite_label_->setText(QString::number(stats.mailbox_overwrites));
}

void MainWindow::on_playback_finished(const QString& reason) {
    status_label_->setText(reason);
    set_controls_for_running(false);
}

void MainWindow::set_controls_for_running(bool running) {
    connect_button_->setEnabled(!running);
    disconnect_button_->setEnabled(running);
    url_edit_->setEnabled(!running);
    transport_combo_->setEnabled(!running);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    stop_playback();
    event->accept();
}

}  // namespace rkstream::qtclient
