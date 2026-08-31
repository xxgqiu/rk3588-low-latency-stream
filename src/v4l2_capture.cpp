#include "rkstream/v4l2_capture.h"

#include "rkstream/pixel_format.h"

#include <linux/videodev2.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace rkstream {
namespace {

// steady_clock 不受系统校时影响，适合测量耗时；这里统一换成微秒便于日志对账。
uint64_t monotonic_us() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string errno_message(const char* operation) {
    std::ostringstream stream;
    stream << operation << " failed: errno=" << errno << " (" << std::strerror(errno) << ')';
    return stream.str();
}

}  // namespace

V4L2Capture::~V4L2Capture() {
    close();
}

int V4L2Capture::ioctl_retry(int fd, unsigned long request, void* argument) {
    int result = -1;
    do {
        result = ::ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

uint32_t V4L2Capture::buffer_type() const {
    return info_.multi_planar ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                              : V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

bool V4L2Capture::open(const CaptureConfig& config, std::string* error) {
    // open 可重复调用：先释放旧资源，保证失败路径和重开路径使用同一套清理逻辑。
    close();
    if (config.device.empty() || config.width == 0 || config.height == 0 ||
        config.fps == 0 || config.pixel_format == 0 || config.buffer_count < 2) {
        if (error != nullptr) {
            *error = "invalid capture configuration";
        }
        return false;
    }

    fd_ = ::open(config.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        if (error != nullptr) {
            const std::string operation = "open " + config.device;
            *error = errno_message(operation.c_str());
        }
        return false;
    }

    // 1. 查询能力。不能根据 /dev/videoX 名称猜 API；USB 常是 single-planar，
    //    RKISP mainpath 则可能是 multi-planar。
    v4l2_capability capability{};
    if (ioctl_retry(fd_, VIDIOC_QUERYCAP, &capability) < 0) {
        if (error != nullptr) {
            *error = errno_message("VIDIOC_QUERYCAP");
        }
        close();
        return false;
    }

    const uint32_t capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                                      ? capability.device_caps
                                      : capability.capabilities;
    const bool single_planar = (capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
    const bool multi_planar = (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
    if ((!single_planar && !multi_planar) || (capabilities & V4L2_CAP_STREAMING) == 0) {
        if (error != nullptr) {
            *error = "device does not support streaming video capture";
        }
        close();
        return false;
    }

    info_.driver = reinterpret_cast<const char*>(capability.driver);
    info_.card = reinterpret_cast<const char*>(capability.card);
    info_.bus = reinterpret_cast<const char*>(capability.bus_info);
    info_.multi_planar = multi_planar;

    // 2. 请求像素格式和尺寸。VIDIOC_S_FMT 既是“设置”也是“协商”：驱动可以
    //    调整宽高、stride 等，因此后面必须从返回结构读回实际值。
    v4l2_format format{};
    format.type = buffer_type();
    if (info_.multi_planar) {
        format.fmt.pix_mp.width = config.width;
        format.fmt.pix_mp.height = config.height;
        format.fmt.pix_mp.pixelformat = config.pixel_format;
        format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    } else {
        format.fmt.pix.width = config.width;
        format.fmt.pix.height = config.height;
        format.fmt.pix.pixelformat = config.pixel_format;
        format.fmt.pix.field = V4L2_FIELD_NONE;
    }

    if (ioctl_retry(fd_, VIDIOC_S_FMT, &format) < 0) {
        if (error != nullptr) {
            *error = errno_message("VIDIOC_S_FMT");
        }
        close();
        return false;
    }

    if (info_.multi_planar) {
        info_.width = format.fmt.pix_mp.width;
        info_.height = format.fmt.pix_mp.height;
        info_.pixel_format = format.fmt.pix_mp.pixelformat;
        info_.plane_count = format.fmt.pix_mp.num_planes;
        info_.bytes_per_line = format.fmt.pix_mp.plane_fmt[0].bytesperline;
        info_.size_image = 0;
        for (uint32_t plane = 0; plane < info_.plane_count; ++plane) {
            info_.size_image += format.fmt.pix_mp.plane_fmt[plane].sizeimage;
        }
    } else {
        info_.width = format.fmt.pix.width;
        info_.height = format.fmt.pix.height;
        info_.pixel_format = format.fmt.pix.pixelformat;
        info_.plane_count = 1;
        info_.bytes_per_line = format.fmt.pix.bytesperline;
        info_.size_image = format.fmt.pix.sizeimage;
    }

    if (info_.plane_count == 0 || info_.plane_count > VIDEO_MAX_PLANES) {
        if (error != nullptr) {
            *error = "driver returned an invalid plane count";
        }
        close();
        return false;
    }
    if (info_.pixel_format != config.pixel_format) {
        if (error != nullptr) {
            *error = "driver changed requested format " + pixel_format_name(config.pixel_format) +
                     " to " + pixel_format_name(info_.pixel_format);
        }
        close();
        return false;
    }

    // 3. 请求帧率并用 G_PARM 读回。摄像头声明/协商 30 FPS 不代表真实一定 30；
    //    最终性能仍以驱动 timestamp 和实测 elapsed 为准。
    v4l2_streamparm stream_parameters{};
    stream_parameters.type = buffer_type();
    stream_parameters.parm.capture.timeperframe.numerator = 1;
    stream_parameters.parm.capture.timeperframe.denominator = config.fps;
    (void)ioctl_retry(fd_, VIDIOC_S_PARM, &stream_parameters);

    stream_parameters = {};
    stream_parameters.type = buffer_type();
    if (ioctl_retry(fd_, VIDIOC_G_PARM, &stream_parameters) == 0 &&
        stream_parameters.parm.capture.timeperframe.numerator != 0) {
        info_.fps_numerator = stream_parameters.parm.capture.timeperframe.denominator;
        info_.fps_denominator = stream_parameters.parm.capture.timeperframe.numerator;
    } else {
        info_.fps_numerator = config.fps;
        info_.fps_denominator = 1;
    }

    // 4. 让驱动分配 streaming buffers。MMAP 模式不是应用 malloc 一块相机内存，
    //    而是把驱动已有的 buffer 映射到本进程地址空间。
    v4l2_requestbuffers request{};
    request.count = config.buffer_count;
    request.type = buffer_type();
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(fd_, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
        if (error != nullptr) {
            *error = request.count < 2 ? "driver allocated fewer than two buffers"
                                       : errno_message("VIDIOC_REQBUFS");
        }
        close();
        return false;
    }

    // 5. QUERYBUF 获得每个 plane 的 offset/length，再逐个 mmap。
    //    注意 multi-planar 的“多个 plane”与 NV12 图像的 Y/UV 语义平面并不总是一一对应；
    //    本机 RKISP 的 NV12 是一个 V4L2 plane，内部仍含 Y + UV 两个图像区域。
    buffers_.resize(request.count);
    for (uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer buffer{};
        v4l2_plane planes[VIDEO_MAX_PLANES]{};
        buffer.type = buffer_type();
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (info_.multi_planar) {
            buffer.m.planes = planes;
            buffer.length = info_.plane_count;
        }

        if (ioctl_retry(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
            if (error != nullptr) {
                *error = errno_message("VIDIOC_QUERYBUF");
            }
            close();
            return false;
        }

        auto& mapping = buffers_[index];
        mapping.planes.resize(info_.multi_planar ? buffer.length : 1);
        for (uint32_t plane = 0; plane < mapping.planes.size(); ++plane) {
            const std::size_t length = info_.multi_planar ? planes[plane].length : buffer.length;
            const off_t offset = static_cast<off_t>(
                info_.multi_planar ? planes[plane].m.mem_offset : buffer.m.offset);
            void* address = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, offset);
            if (address == MAP_FAILED) {
                if (error != nullptr) {
                    *error = errno_message("mmap");
                }
                close();
                return false;
            }
            mapping.planes[plane] = {address, length};
        }
    }
    info_.buffer_count = static_cast<uint32_t>(buffers_.size());
    return true;
}

bool V4L2Capture::queue_buffer(uint32_t index, std::string* error) {
    v4l2_buffer buffer{};
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    buffer.type = buffer_type();
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (info_.multi_planar) {
        buffer.m.planes = planes;
        buffer.length = info_.plane_count;
    }
    if (ioctl_retry(fd_, VIDIOC_QBUF, &buffer) < 0) {
        if (error != nullptr) {
            *error = errno_message("VIDIOC_QBUF");
        }
        return false;
    }
    return true;
}

bool V4L2Capture::start(std::string* error) {
    if (fd_ < 0 || buffers_.empty() || streaming_) {
        if (error != nullptr) {
            *error = "capture device is not ready to start";
        }
        return false;
    }
    // 驱动只有拿到已 QBUF 的空 slot 才能写入新画面，所以 STREAMON 前先全部入队。
    for (uint32_t index = 0; index < buffers_.size(); ++index) {
        if (!queue_buffer(index, error)) {
            return false;
        }
    }
    uint32_t type = buffer_type();
    if (ioctl_retry(fd_, VIDIOC_STREAMON, &type) < 0) {
        if (error != nullptr) {
            *error = errno_message("VIDIOC_STREAMON");
        }
        return false;
    }
    streaming_ = true;
    return true;
}

CaptureResult V4L2Capture::capture(Frame* frame, int timeout_ms, std::string* error) {
    if (!streaming_ || frame == nullptr) {
        if (error != nullptr) {
            *error = "capture called before streaming started";
        }
        return CaptureResult::kFatal;
    }

    // poll 避免对非阻塞 fd 忙等。timeout 是正常可观测状态，不直接判设备故障。
    pollfd descriptor{fd_, POLLIN, 0};
    const int poll_result = ::poll(&descriptor, 1, timeout_ms);
    if (poll_result == 0) {
        return CaptureResult::kTimeout;
    }
    if (poll_result < 0) {
        if (errno == EINTR) {
            return CaptureResult::kInterrupted;
        }
        if (error != nullptr) {
            *error = errno_message("poll");
        }
        return CaptureResult::kFatal;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        if (error != nullptr) {
            *error = "camera poll returned an error event";
        }
        return CaptureResult::kFatal;
    }

    v4l2_buffer buffer{};
    v4l2_plane planes[VIDEO_MAX_PLANES]{};
    buffer.type = buffer_type();
    buffer.memory = V4L2_MEMORY_MMAP;
    if (info_.multi_planar) {
        buffer.m.planes = planes;
        buffer.length = info_.plane_count;
    }
    // DQBUF 把一个已填充 slot 的所有权从驱动交给应用。
    if (ioctl_retry(fd_, VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
            return CaptureResult::kTimeout;
        }
        if (error != nullptr) {
            *error = errno_message("VIDIOC_DQBUF");
        }
        return CaptureResult::kFatal;
    }

    if (buffer.index >= buffers_.size()) {
        if (error != nullptr) {
            *error = "driver returned an out-of-range buffer index";
        }
        return CaptureResult::kFatal;
    }

    frame->sequence = buffer.sequence;
    frame->driver_timestamp_us = static_cast<uint64_t>(buffer.timestamp.tv_sec) * 1'000'000ULL +
                                 static_cast<uint64_t>(buffer.timestamp.tv_usec);
    frame->dequeue_timestamp_us = monotonic_us();
    frame->timestamp_flags = buffer.flags &
        (V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_TSTAMP_SRC_MASK);
    frame->pixel_format = info_.pixel_format;
    frame->width = info_.width;
    frame->height = info_.height;
    frame->bytes_per_line = info_.bytes_per_line;
    frame->data.clear();

    // CPU-copy 基线的核心：先计算有效字节总量，再复制每个 V4L2 plane。
    // bytesused 为 0 时才退回映射长度，且始终检查驱动值不超过 mmap 长度。
    const auto& mapping = buffers_[buffer.index];
    std::size_t total_bytes = 0;
    for (uint32_t plane = 0; plane < mapping.planes.size(); ++plane) {
        const std::size_t used = info_.multi_planar ? planes[plane].bytesused : buffer.bytesused;
        total_bytes += used > 0 ? used : mapping.planes[plane].length;
    }
    frame->data.reserve(total_bytes);
    for (uint32_t plane = 0; plane < mapping.planes.size(); ++plane) {
        const auto& mapped_plane = mapping.planes[plane];
        std::size_t used = info_.multi_planar ? planes[plane].bytesused : buffer.bytesused;
        if (used == 0) {
            used = mapped_plane.length;
        }
        if (used > mapped_plane.length) {
            if (error != nullptr) {
                *error = "driver reported bytesused larger than mapped buffer";
            }
            (void)queue_buffer(buffer.index, nullptr);
            return CaptureResult::kFatal;
        }
        const auto* begin = static_cast<const std::byte*>(mapped_plane.address);
        frame->data.insert(frame->data.end(), begin, begin + used);
    }

    // 像素已经由 Frame::data 独立拥有，可以立即把原始 slot 归还驱动。
    if (!queue_buffer(buffer.index, error)) {
        return CaptureResult::kFatal;
    }
    return CaptureResult::kFrame;
}

void V4L2Capture::unmap_buffers() {
    for (auto& buffer : buffers_) {
        for (auto& plane : buffer.planes) {
            if (plane.address != nullptr && plane.address != MAP_FAILED) {
                ::munmap(plane.address, plane.length);
            }
            plane = {};
        }
    }
    buffers_.clear();
}

void V4L2Capture::close() {
    // 释放顺序与取得顺序相反：先停流，确保驱动不再写 mmap，再解除映射并关闭 fd。
    if (fd_ >= 0 && streaming_) {
        uint32_t type = buffer_type();
        (void)ioctl_retry(fd_, VIDIOC_STREAMOFF, &type);
    }
    streaming_ = false;
    unmap_buffers();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    info_ = {};
}

}  // namespace rkstream
