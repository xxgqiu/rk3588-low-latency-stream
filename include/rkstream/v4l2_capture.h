#pragma once

#include "rkstream/frame.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rkstream {

// 用户希望协商的采集参数。当前实现要求所有数值均为非零有效值；真正生效的
// 参数仍必须读取 CaptureInfo，不能假设驱动完全接受了请求值。
struct CaptureConfig {
    std::string device;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t pixel_format = 0;
    uint32_t buffer_count = 4;
};

// 完成 VIDIOC_S_FMT / VIDIOC_S_PARM 后从驱动读回的实际配置。
// 该结构既用于后续内存计算，也会打印到日志，保证性能结论基于真实格式。
struct CaptureInfo {
    std::string driver;
    std::string card;
    std::string bus;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_numerator = 0;
    uint32_t fps_denominator = 1;
    uint32_t pixel_format = 0;
    uint32_t bytes_per_line = 0;
    uint32_t size_image = 0;
    uint32_t plane_count = 0;
    uint32_t buffer_count = 0;
    bool multi_planar = false;
};

// capture() 将“暂时没帧”和“设备致命错误”分开，调用者可正确决定重试或退出。
enum class CaptureResult {
    kFrame,
    kTimeout,
    kInterrupted,
    kFatal,
};

// 基于 V4L2 streaming I/O + MMAP 的 CPU-copy 采集器。
//
// 生命周期必须严格遵守：
//   open()  : 打开设备、协商格式、申请并 mmap 驱动缓冲区；
//   start() : 先 QBUF 全部缓冲，再 STREAMON；
//   capture(): poll -> DQBUF -> memcpy 到 Frame -> QBUF；
//   close() : STREAMOFF、munmap、close(fd)。
//
// 该类不可复制，因为 fd 和 mmap 地址都具有唯一资源所有权。
class V4L2Capture {
public:
    V4L2Capture() = default;
    ~V4L2Capture();

    V4L2Capture(const V4L2Capture&) = delete;
    V4L2Capture& operator=(const V4L2Capture&) = delete;

    // 成功后 info() 才有效。失败时会自行清理已取得的部分资源。
    bool open(const CaptureConfig& config, std::string* error);
    bool start(std::string* error);

    // 成功帧写入 *frame。timeout_ms 只限制等待摄像头的时间，不是运行总时长。
    CaptureResult capture(Frame* frame, int timeout_ms, std::string* error);
    void close();

    const CaptureInfo& info() const { return info_; }

private:
    // multi-planar API 中一个 V4L2 buffer 可包含多个 plane，因此映射按两层保存。
    struct PlaneMapping {
        void* address = nullptr;
        std::size_t length = 0;
    };

    struct BufferMapping {
        std::vector<PlaneMapping> planes;
    };

    // ioctl 被信号打断并返回 EINTR 时自动重试，其余错误原样交给调用方处理。
    static int ioctl_retry(int fd, unsigned long request, void* argument);
    uint32_t buffer_type() const;
    bool queue_buffer(uint32_t index, std::string* error);
    void unmap_buffers();

    int fd_ = -1;
    bool streaming_ = false;
    CaptureInfo info_;
    std::vector<BufferMapping> buffers_;
};

}  // namespace rkstream
