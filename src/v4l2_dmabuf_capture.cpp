#include "rkstream/v4l2_dmabuf_capture.h"

#include "rkstream/pixel_format.h"

#include <linux/videodev2.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rkstream {
namespace {

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

V4L2DmabufCapture::~V4L2DmabufCapture() {
    close();
}

int V4L2DmabufCapture::ioctl_retry(int fd, unsigned long request, void* argument) {
    int result = -1;
    do {
        result = ::ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

bool V4L2DmabufCapture::open(const CaptureConfig& config, std::string* error) {
    // DMA 路径目前针对已经在板端验证的 RKISP multi-planar NV12。限制越明确，
    // 越不容易把 USB 摄像头的 single-planar/MJPEG 缓冲误交给 RGA。
    close();
    if (config.device.empty() || config.width == 0 || config.height == 0 ||
        config.fps == 0 || config.pixel_format == 0 || config.buffer_count < 2) {
        if (error != nullptr) {
            *error = "invalid DMA-BUF capture configuration";
        }
        return false;
    }

    fd_ = ::open(config.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        if (error != nullptr) {
            *error = errno_message("open camera");
        }
        return false;
    }

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
    if ((capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0 ||
        (capabilities & V4L2_CAP_STREAMING) == 0) {
        if (error != nullptr) {
            *error = "DMA-BUF path requires multi-planar streaming capture";
        }
        close();
        return false;
    }

    info_.driver = reinterpret_cast<const char*>(capability.driver);
    info_.card = reinterpret_cast<const char*>(capability.card);
    info_.bus = reinterpret_cast<const char*>(capability.bus_info);
    info_.multi_planar = true;

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = config.width;
    format.fmt.pix_mp.height = config.height;
    format.fmt.pix_mp.pixelformat = config.pixel_format;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl_retry(fd_, VIDIOC_S_FMT, &format) < 0) {
        if (error != nullptr) {
            *error = errno_message("VIDIOC_S_FMT");
        }
        close();
        return false;
    }

    info_.width = format.fmt.pix_mp.width;
    info_.height = format.fmt.pix_mp.height;
    info_.pixel_format = format.fmt.pix_mp.pixelformat;
    info_.plane_count = format.fmt.pix_mp.num_planes;
    info_.bytes_per_line = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    info_.size_image = format.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (info_.pixel_format != config.pixel_format || info_.plane_count != 1 ||
        info_.bytes_per_line < info_.width) {
        if (error != nullptr) {
            *error = "driver returned unsupported DMA-BUF layout: format=" +
                     pixel_format_name(info_.pixel_format) +
                     " planes=" + std::to_string(info_.plane_count);
        }
        close();
        return false;
    }

    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator = config.fps;
    (void)ioctl_retry(fd_, VIDIOC_S_PARM, &parameters);
    parameters = {};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl_retry(fd_, VIDIOC_G_PARM, &parameters) == 0 &&
        parameters.parm.capture.timeperframe.numerator != 0) {
        info_.fps_numerator = parameters.parm.capture.timeperframe.denominator;
        info_.fps_denominator = parameters.parm.capture.timeperframe.numerator;
    } else {
        info_.fps_numerator = config.fps;
        info_.fps_denominator = 1;
    }

    // 仍由 V4L2 驱动用 MMAP memory type 分配 slot；“DMA-BUF 路径”指随后通过
    // EXPBUF 导出这些 slot 的 fd，而不是改成 V4L2_MEMORY_DMABUF 导入外部分配内存。
    v4l2_requestbuffers request{};
    request.count = config.buffer_count;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(fd_, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
        if (error != nullptr) {
            *error = request.count < 2 ? "driver allocated fewer than two DMA-BUF buffers"
                                       : errno_message("VIDIOC_REQBUFS");
        }
        close();
        return false;
    }

    // 每个 slot 只导出一次 fd，后续所有帧复用。这样不会按帧 open/close 或 import。
    exported_buffers_.reserve(request.count);
    queued_.assign(request.count, false);
    for (uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer query{};
        std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
        query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        query.memory = V4L2_MEMORY_MMAP;
        query.index = index;
        query.m.planes = planes.data();
        query.length = info_.plane_count;
        if (ioctl_retry(fd_, VIDIOC_QUERYBUF, &query) < 0) {
            if (error != nullptr) {
                *error = errno_message("VIDIOC_QUERYBUF");
            }
            close();
            return false;
        }

        v4l2_exportbuffer export_buffer{};
        export_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        export_buffer.index = index;
        export_buffer.plane = 0;
        export_buffer.flags = O_RDWR | O_CLOEXEC;
        if (ioctl_retry(fd_, VIDIOC_EXPBUF, &export_buffer) < 0) {
            if (error != nullptr) {
                *error = errno_message("VIDIOC_EXPBUF");
            }
            close();
            return false;
        }
        exported_buffers_.push_back({index, export_buffer.fd, planes[0].length});
    }
    info_.buffer_count = static_cast<uint32_t>(exported_buffers_.size());
    return true;
}

bool V4L2DmabufCapture::queue_buffer(uint32_t index, std::string* error) {
    // capture 线程会 DQBUF，encode 线程会在 RGA 完成后 QBUF；两者共享同一个
    // V4L2 fd，所以 ioctl 和 queued_ 状态必须放在同一临界区中更新。
    std::lock_guard<std::mutex> lock(ioctl_mutex_);
    if (index >= exported_buffers_.size()) {
        if (error != nullptr) {
            *error = "DMA-BUF index is out of range";
        }
        return false;
    }
    if (queued_[index]) {
        if (error != nullptr) {
            *error = "DMA-BUF buffer is already queued";
        }
        return false;
    }
    v4l2_buffer buffer{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.m.planes = planes.data();
    buffer.length = info_.plane_count;
    if (ioctl_retry(fd_, VIDIOC_QBUF, &buffer) < 0) {
        if (error != nullptr) {
            *error = errno_message("VIDIOC_QBUF");
        }
        return false;
    }
    queued_[index] = true;
    return true;
}

bool V4L2DmabufCapture::start(std::string* error) {
    if (fd_ < 0 || exported_buffers_.empty() || streaming_) {
        if (error != nullptr) {
            *error = "DMA-BUF capture device is not ready to start";
        }
        return false;
    }
    for (uint32_t index = 0; index < exported_buffers_.size(); ++index) {
        if (!queue_buffer(index, error)) {
            return false;
        }
    }
    uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl_retry(fd_, VIDIOC_STREAMON, &type) < 0) {
        if (error != nullptr) {
            *error = errno_message("VIDIOC_STREAMON");
        }
        return false;
    }
    streaming_ = true;
    return true;
}

CaptureResult V4L2DmabufCapture::capture(DmabufFrame* frame, int timeout_ms,
                                         std::string* error) {
    if (!streaming_ || frame == nullptr) {
        if (error != nullptr) {
            *error = "DMA-BUF capture called before streaming started";
        }
        return CaptureResult::kFatal;
    }
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
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.m.planes = planes.data();
    buffer.length = info_.plane_count;
    {
        std::lock_guard<std::mutex> lock(ioctl_mutex_);
        // DQBUF 成功是租约的唯一创建点：queued_ 从 true 变 false。
        if (ioctl_retry(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                return CaptureResult::kTimeout;
            }
            if (error != nullptr) {
                *error = errno_message("VIDIOC_DQBUF");
            }
            return CaptureResult::kFatal;
        }
        if (buffer.index >= exported_buffers_.size() || !queued_[buffer.index]) {
            if (error != nullptr) {
                *error = "driver returned an invalid or already leased DMA-BUF index";
            }
            return CaptureResult::kFatal;
        }
        queued_[buffer.index] = false;
    }

    // 只复制几十字节的描述信息，不复制约 3 MB 的 NV12 像素。
    frame->buffer_index = buffer.index;
    frame->sequence = buffer.sequence;
    frame->driver_timestamp_us = static_cast<uint64_t>(buffer.timestamp.tv_sec) * 1'000'000ULL +
                                 static_cast<uint64_t>(buffer.timestamp.tv_usec);
    frame->dequeue_timestamp_us = monotonic_us();
    frame->timestamp_flags = buffer.flags &
        (V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_TSTAMP_SRC_MASK);
    frame->bytes_used = planes[0].bytesused > 0 ? planes[0].bytesused
                                                : exported_buffers_[buffer.index].length;
    if (frame->bytes_used > exported_buffers_[buffer.index].length) {
        if (error != nullptr) {
            *error = "driver reported bytesused larger than exported DMA-BUF";
        }
        return CaptureResult::kFatal;
    }
    return CaptureResult::kFrame;
}

bool V4L2DmabufCapture::release(const DmabufFrame& frame, std::string* error) {
    if (!streaming_ || frame.buffer_index >= queued_.size()) {
        if (error != nullptr) {
            *error = "invalid DMA-BUF frame release";
        }
        return false;
    }
    if (queued_[frame.buffer_index]) {
        if (error != nullptr) {
            *error = "DMA-BUF frame was already released";
        }
        return false;
    }
    // queue_buffer 是租约的唯一销毁点：queued_ 从 false 变 true。
    return queue_buffer(frame.buffer_index, error);
}

void V4L2DmabufCapture::close() {
    // STREAMOFF 会让所有 V4L2 slot 停止流转；之后才能关闭 EXPBUF 导出的 fd。
    // 应用正常停机前仍应主动 drain/release 队列，这样 lease 计数可以完整审计。
    if (streaming_ && fd_ >= 0) {
        uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        (void)ioctl_retry(fd_, VIDIOC_STREAMOFF, &type);
    }
    streaming_ = false;
    for (auto& buffer : exported_buffers_) {
        if (buffer.fd >= 0) {
            ::close(buffer.fd);
            buffer.fd = -1;
        }
    }
    exported_buffers_.clear();
    queued_.clear();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    info_ = {};
}

}  // namespace rkstream
