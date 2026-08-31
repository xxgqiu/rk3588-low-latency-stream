#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rkstream {

// 有界“最新优先”队列。
//
// 普通任务队列通常希望一个不丢；实时视频恰好不同：如果消费者比摄像头慢，
// 无界排队会让用户看到越来越旧的画面。这里在容量满时丢最旧项、保留新项，
// 用可控丢帧换取低延迟和固定内存上限。
//
// T 可以是普通 Frame，也可以是需要显式归还的 DMA-BUF lease。后一种情况下，
// push() 返回的 dropped_value 仍归调用者所有，调用者必须执行 release。
template <typename T>
class BoundedLatestQueue {
public:
    struct PushResult {
        bool accepted = false;
        bool dropped_oldest = false;
        std::size_t size = 0;
        // 对普通值可以忽略；对 DMA-BUF lease 等带外部资源的值，生产者必须在
        // dropped_oldest=true 时显式处理并归还该资源。
        std::optional<T> dropped_value;
    };

    explicit BoundedLatestQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("queue capacity must be greater than zero");
        }
    }

    BoundedLatestQueue(const BoundedLatestQueue&) = delete;
    BoundedLatestQueue& operator=(const BoundedLatestQueue&) = delete;

    // value 按值传入后移动进队列，清晰表达所有权转移。
    // 队列已 close 时 accepted=false，调用者仍负责处理自己传入的资源。
    PushResult push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return {};
        }

        std::optional<T> dropped_value;
        if (queue_.size() == capacity_) {
            dropped_value.emplace(std::move(queue_.front()));
            queue_.pop_front();
            ++dropped_count_;
        }
        queue_.push_back(std::move(value));
        high_watermark_ = queue_.size() > high_watermark_ ? queue_.size() : high_watermark_;
        const std::size_t size = queue_.size();
        ready_.notify_one();
        return {true, dropped_value.has_value(), size, std::move(dropped_value)};
    }

    // 队列为空时阻塞；close 后若仍有残留项，会先逐项返回，最后返回 nullopt。
    // 这种语义让流水线可以在停机时完成 drain，而不是突然丢掉已编码数据。
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return std::nullopt;
        }

        T value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }

    // close 只阻止新 push 并唤醒消费者，不主动销毁队列中已有资源。
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

    // 消费线程因错误提前退出时，队列中可能仍有尚未处理的外部资源。
    // drain 将所有权交还调用者，避免仅靠析构静默丢弃 DMA-BUF lease。
    std::vector<T> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<T> values;
        values.reserve(queue_.size());
        while (!queue_.empty()) {
            values.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return values;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t capacity() const { return capacity_; }

    std::size_t dropped_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_count_;
    }

    std::size_t high_watermark() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_watermark_;
    }

private:
    // capacity_ 固定且非零，因此内存不会随运行时长增长。
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> queue_;
    bool closed_ = false;
    std::size_t dropped_count_ = 0;
    std::size_t high_watermark_ = 0;
};

}  // namespace rkstream
