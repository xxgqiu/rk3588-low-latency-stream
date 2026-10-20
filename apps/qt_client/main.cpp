#include "main_window.h"
#include "rtsp_decode_thread.h"

#include <QApplication>
#include <QString>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("rkstream-qt-client"));
    application.setOrganizationName(QStringLiteral("rkstream"));

    // ClientStats 会从解码线程通过 queued signal 进入 Qt 主线程；注册后 Qt 才能安全地
    // 复制并投递该自定义类型。
    qRegisterMetaType<rkstream::qtclient::ClientStats>(
        "rkstream::qtclient::ClientStats");

    // 可以把URL作为第一个命令行参数传入；未传时使用当前开发板正式地址。
    const QString initial_url = argc >= 2
                                    ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral("rtsp://10.42.0.81:8554/live");
    rkstream::qtclient::MainWindow window(initial_url);
    window.show();
    return application.exec();
}
