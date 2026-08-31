#pragma once

#include "rkstream/v4l2_capture.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace rkstream {

// 描述一个由 V4L2 驱动分配、再通过 VIDIOC_EXPBUF 导出的 DMA-BUF。
// fd 的所有权始终属于 V4L2DmabufCapture；调用者只能在 capture 对象存活期间借用，
// 不得 close，也不得在对应 frame 已 release 后继续让硬件读取该缓冲。
struct DmabufBufferInfo {
    // index 是 V4L2 buffer slot 编号，也是后续 DQBUF/QBUF 的身份标识。
    uint32_t index = 0;
    // 可跨设备共享的 DMA-BUF 文件描述符，RGA 可通过它直接访问同一块物理缓冲。
    int fd = -1;
    // 驱动分配的完整缓冲区长度，不一定等于一帧可见像素的理论字节数。
    std::size_t length = 0;
};

// DQBUF 后获得的轻量帧租约。与 Frame 不同，它不复制像素，只记录哪一个 DMA-BUF
// 当前归应用所有。调用者完成 RGA/MPP 读取后必须调用 release，才能把它重新 QBUF。
struct DmabufFrame {
    // 其余模块通过 buffer_index 在 buffers() 中找到对应的 DMA-BUF fd。
    uint32_t buffer_index = 0;
    uint32_t sequence = 0;
    uint64_t driver_timestamp_us = 0;
    uint64_t dequeue_timestamp_us = 0;
    uint32_t timestamp_flags = 0;
    std::size_t bytes_used = 0;
};

// V4L2 MMAP + EXPBUF 采集器：像素留在驱动分配的 DMA 缓冲区中，不复制到 vector。
//
// 与 V4L2Capture 最关键的区别：
//   - capture() 完成 DQBUF 后不会立即 QBUF，而是把 DmabufFrame 租约交给调用者；
//   - RGA 读取完成后，调用者必须 release()；
//   - release() 才执行 QBUF，把 slot 归还摄像头驱动。
//
// 这个延迟归还机制是零 CPU 图像拷贝的前提，也是最容易写错的所有权边界。
class V4L2DmabufCapture {
public:
    V4L2DmabufCapture() = default;
    ~V4L2DmabufCapture();

    V4L2DmabufCapture(const V4L2DmabufCapture&) = delete;
    V4L2DmabufCapture& operator=(const V4L2DmabufCapture&) = delete;

    bool open(const CaptureConfig& config, std::string* error);
    bool start(std::string* error);
    // 成功时调用者取得唯一 lease；每个成功结果最终必须且只能 release 一次。
    CaptureResult capture(DmabufFrame* frame, int timeout_ms, std::string* error);

    // 归还 lease。重复归还、越界 index 或未 DQBUF 的 slot 都会失败。
    bool release(const DmabufFrame& frame, std::string* error);
    void close();

    const CaptureInfo& info() const { return info_; }
    const std::vector<DmabufBufferInfo>& buffers() const { return exported_buffers_; }

private:
    static int ioctl_retry(int fd, unsigned long request, void* argument);
    bool queue_buffer(uint32_t index, std::string* error);

    int fd_ = -1;
    bool streaming_ = false;
    CaptureInfo info_;
    std::vector<DmabufBufferInfo> exported_buffers_;

    // queued_[i] 为 true 表示 slot 在驱动手中；false 表示应用持有 lease。
    // 它是运行时检查“双重 release/错误 QBUF”的轻量状态机。
    std::vector<bool> queued_;
    // capture thread 执行 DQBUF，encode thread 在 RGA 完成后执行 QBUF；
    // 同一个 V4L2 fd 上的 ioctl 和 queued_ 状态必须串行化。
    std::mutex ioctl_mutex_;
};

}  // namespace rkstream
