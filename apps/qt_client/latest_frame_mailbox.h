#pragma once

#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>

#include <utility>

namespace rkstream::qtclient {

// LatestFrameMailbox 是解码线程与 Qt 主线程之间的“容量 1 帧邮箱”。
//
// 为什么不用普通 std::queue<QImage>：
//   RTSP 解码线程可能持续产生约 24 帧/秒，而 GUI 一旦因为窗口缩放、截图或系统负载
//   暂时变慢，无界队列就会积累旧画面。队列越长，用户看到的内容越旧，端到端延迟
//   会不断增长。实时预览更重视“最新画面”，所以新帧直接覆盖尚未显示的旧帧。
//
// 线程约束：
//   publish() 只由解码线程调用；take_latest() 只由 Qt 主线程调用。QMutex 保护 QImage、
//   generation 和 unread，避免两个线程同时读写。QImage 是隐式共享对象，从邮箱复制到
//   GUI 后只增加引用计数，不会再次复制整张 RGB 图像。
class LatestFrameMailbox final {
public:
    // 发布一张新解码帧。返回 true 表示邮箱里原来有一张 GUI 尚未取走的帧，本次发布
    // 覆盖了它；该计数用于证明客户端是否因显示不及时而主动舍弃旧画面。
    bool publish(QImage image) {
        QMutexLocker lock(&mutex_);
        const bool replaced_unread_frame = unread_;
        if (replaced_unread_frame) {
            ++overwritten_count_;
        }
        image_ = std::move(image);
        unread_ = true;
        ++generation_;
        return replaced_unread_frame;
    }

    // GUI 定时器调用该函数取得最新帧。只有出现新 generation 时才返回 true，避免在
    // 60 Hz UI 刷新中反复缩放同一张约 24 FPS 的视频帧。
    bool take_latest(quint64* last_generation, QImage* image) {
        if (last_generation == nullptr || image == nullptr) {
            return false;
        }
        QMutexLocker lock(&mutex_);
        if (!unread_ || generation_ == *last_generation || image_.isNull()) {
            return false;
        }
        *image = image_;
        *last_generation = generation_;
        unread_ = false;
        return true;
    }

    // 新会话开始前清掉上一条 RTSP 连接残留的画面。generation 不回退，避免 GUI 保存
    // 的 last_generation 与新会话第一帧产生误判。
    void clear() {
        QMutexLocker lock(&mutex_);
        image_ = QImage{};
        unread_ = false;
    }

    quint64 overwritten_count() const {
        QMutexLocker lock(&mutex_);
        return overwritten_count_;
    }

private:
    mutable QMutex mutex_;
    QImage image_;
    quint64 generation_ = 0;
    quint64 overwritten_count_ = 0;
    bool unread_ = false;
};

}  // namespace rkstream::qtclient
