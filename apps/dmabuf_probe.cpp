#include "rkstream/nv12_layout.h"

extern "C" {
#include <mpp_buffer.h>
}

#include <im2d.h>
#include <linux/videodev2.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

/*
 * DMA-BUF/RGA 硬件可行性探针，而不是最终业务程序。
 *
 * 它直接展示最底层步骤：V4L2 REQBUFS/EXPBUF、RGA import/wrap/copy、MPP DMA-BUF
 * 分配，并用可见 NV12 区域 checksum 验证复制前后内容。之所以保留这个独立工具，
 * 是因为 DMA-BUF 问题常发生在 fd 导出、stride/offset 或 RGA 布局层；用完整 RTSP
 * 程序排查会混入线程和网络噪声。
 */
namespace {

struct ExportedBuffer {
    uint32_t index = 0;
    std::size_t length = 0;
    int fd = -1;
    rga_buffer_handle_t rga_handle = 0;
};

int ioctl_retry(int fd, unsigned long request, void* argument) {
    int result = -1;
    do {
        result = ::ioctl(fd, request, argument);
    } while (result == -1 && errno == EINTR);
    return result;
}

std::string errno_message(const char* operation) {
    std::ostringstream stream;
    stream << operation << " failed: errno=" << errno << " (" << std::strerror(errno) << ')';
    return stream.str();
}

std::string fourcc(uint32_t value) {
    char text[5] = {
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
        '\0',
    };
    return text;
}

uint64_t update_checksum(uint64_t hash, const uint8_t* data, std::size_t size) {
    constexpr uint64_t kPrime = 1099511628211ULL;
    for (std::size_t offset = 0; offset < size; ++offset) {
        hash ^= data[offset];
        hash *= kPrime;
    }
    return hash;
}

uint64_t visible_nv12_checksum(const uint8_t* data, uint32_t width, uint32_t height,
                               uint32_t stride, std::size_t uv_offset) {
    // 只哈希可见 width 字节，跳过每行 padding；Y 和 UV 分别按各自行数遍历。
    // checksum 相同不是图像质量证明，但足以发现明显的错位、漏拷贝和错误 UV 起点。
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    uint64_t hash = kOffset;
    for (uint32_t row = 0; row < height; ++row) {
        hash = update_checksum(hash, data + static_cast<std::size_t>(row) * stride, width);
    }
    const uint8_t* uv = data + uv_offset;
    for (uint32_t row = 0; row < height / 2; ++row) {
        hash = update_checksum(hash, uv + static_cast<std::size_t>(row) * stride, width);
    }
    return hash;
}

void release_buffers(std::vector<ExportedBuffer>* buffers) {
    for (auto& buffer : *buffers) {
        if (buffer.rga_handle != 0) {
            (void)releasebuffer_handle(buffer.rga_handle);
            buffer.rga_handle = 0;
        }
        if (buffer.fd >= 0) {
            ::close(buffer.fd);
            buffer.fd = -1;
        }
    }
}

bool queue_buffer(int camera_fd, uint32_t index, uint32_t plane_count, std::string* error) {
    v4l2_buffer buffer{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.m.planes = planes.data();
    buffer.length = plane_count;
    if (ioctl_retry(camera_fd, VIDIOC_QBUF, &buffer) < 0) {
        if (error != nullptr) {
            *error = errno_message("VIDIOC_QBUF");
        }
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string device = "/dev/video-camera0";
    if (argc == 3 && std::string(argv[1]) == "--device") {
        device = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: dmabuf_probe [--device /dev/video-camera0]\n";
        return 2;
    }

    int camera_fd = -1;
    bool streaming = false;
    MppBuffer destination = nullptr;
    rga_buffer_handle_t destination_rga_handle = 0;
    std::vector<ExportedBuffer> buffers;
    std::string error;
    bool expbuf_pass = false;
    bool mpp_import_pass = false;
    bool rga_import_pass = false;
    bool rga_copy_pass = false;
    bool visible_content_match = false;
    uint64_t source_checksum = 0;
    uint64_t destination_checksum = 0;

    auto cleanup = [&]() {
        if (streaming && camera_fd >= 0) {
            uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            (void)ioctl_retry(camera_fd, VIDIOC_STREAMOFF, &type);
            streaming = false;
        }
        if (destination_rga_handle != 0) {
            (void)releasebuffer_handle(destination_rga_handle);
            destination_rga_handle = 0;
        }
        if (destination != nullptr) {
            mpp_buffer_put(destination);
            destination = nullptr;
        }
        release_buffers(&buffers);
        if (camera_fd >= 0) {
            ::close(camera_fd);
            camera_fd = -1;
        }
    };

    camera_fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (camera_fd < 0) {
        std::cerr << errno_message("open camera") << '\n';
        return 1;
    }

    v4l2_capability capability{};
    if (ioctl_retry(camera_fd, VIDIOC_QUERYCAP, &capability) < 0) {
        error = errno_message("VIDIOC_QUERYCAP");
        cleanup();
        std::cerr << error << '\n';
        return 1;
    }
    const uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
                              ? capability.device_caps
                              : capability.capabilities;
    if ((caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) == 0 ||
        (caps & V4L2_CAP_STREAMING) == 0) {
        cleanup();
        std::cerr << "camera does not support multi-planar streaming capture\n";
        return 1;
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    format.fmt.pix_mp.width = 1632;
    format.fmt.pix_mp.height = 1224;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (ioctl_retry(camera_fd, VIDIOC_S_FMT, &format) < 0) {
        error = errno_message("VIDIOC_S_FMT");
        cleanup();
        std::cerr << error << '\n';
        return 1;
    }

    const uint32_t width = format.fmt.pix_mp.width;
    const uint32_t height = format.fmt.pix_mp.height;
    const uint32_t pixel_format = format.fmt.pix_mp.pixelformat;
    const uint32_t plane_count = format.fmt.pix_mp.num_planes;
    const uint32_t stride = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    const std::size_t size_image = format.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (pixel_format != V4L2_PIX_FMT_NV12 || plane_count != 1 || stride < width) {
        cleanup();
        std::cerr << "unsupported negotiated layout: format=" << fourcc(pixel_format)
                  << " planes=" << plane_count << " stride=" << stride << '\n';
        return 1;
    }

    const auto layout = rkstream::audit_nv12_layout(width, height, stride, size_image);
    std::cout << "LAYOUT device=" << device
              << " format=" << fourcc(pixel_format)
              << " width=" << width
              << " height=" << height
              << " source_stride=" << stride
              << " source_uv_offset=" << layout.source_y_offset
              << " source_bytes=" << layout.source_buffer_bytes
              << " mpp_hstride=" << layout.mpp_horizontal_stride
              << " mpp_vstride=" << layout.mpp_vertical_stride
              << " mpp_uv_offset=" << layout.mpp_y_offset
              << " mpp_bytes=" << layout.mpp_buffer_bytes
              << " padding_gap_bytes=" << layout.alignment_padding_bytes
              << " direct_mpp_layout_compatible="
              << (layout.direct_mpp_layout_compatible ? "true" : "false") << '\n';

    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(camera_fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
        error = request.count < 2 ? "driver allocated fewer than two buffers"
                                  : errno_message("VIDIOC_REQBUFS");
        cleanup();
        std::cerr << error << '\n';
        return 1;
    }

    buffers.reserve(request.count);
    mpp_import_pass = true;
    rga_import_pass = true;
    for (uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer query{};
        std::array<v4l2_plane, VIDEO_MAX_PLANES> planes{};
        query.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        query.memory = V4L2_MEMORY_MMAP;
        query.index = index;
        query.m.planes = planes.data();
        query.length = plane_count;
        if (ioctl_retry(camera_fd, VIDIOC_QUERYBUF, &query) < 0) {
            error = errno_message("VIDIOC_QUERYBUF");
            cleanup();
            std::cerr << error << '\n';
            return 1;
        }

        v4l2_exportbuffer export_buffer{};
        export_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        export_buffer.index = index;
        export_buffer.plane = 0;
        export_buffer.flags = O_RDWR | O_CLOEXEC;
        if (ioctl_retry(camera_fd, VIDIOC_EXPBUF, &export_buffer) < 0) {
            error = errno_message("VIDIOC_EXPBUF");
            cleanup();
            std::cerr << error << '\n';
            return 1;
        }

        ExportedBuffer exported{};
        exported.index = index;
        exported.length = planes[0].length;
        exported.fd = export_buffer.fd;

        struct stat status{};
        const off_t reported_size = ::fstat(exported.fd, &status) == 0 ? status.st_size : -1;

        MppBuffer imported = nullptr;
        MppBufferInfo import_info{};
        import_info.type = MPP_BUFFER_TYPE_EXT_DMA;
        import_info.size = exported.length;
        import_info.fd = exported.fd;
        import_info.index = static_cast<int>(index);
        const MPP_RET mpp_result = mpp_buffer_import(&imported, &import_info);
        if (mpp_result != MPP_OK || imported == nullptr) {
            mpp_import_pass = false;
        }
        const int imported_fd = imported != nullptr ? mpp_buffer_get_fd(imported) : -1;
        const std::size_t imported_size = imported != nullptr ? mpp_buffer_get_size(imported) : 0;
        if (imported != nullptr) {
            mpp_buffer_put(imported);
        }

        exported.rga_handle = importbuffer_fd(exported.fd, static_cast<int>(exported.length));
        if (exported.rga_handle == 0) {
            rga_import_pass = false;
        }

        std::cout << "BUFFER index=" << index
                  << " length=" << exported.length
                  << " dma_fd=" << exported.fd
                  << " fstat_size=" << reported_size
                  << " mpp_import=" << (mpp_result == MPP_OK ? "PASS" : "FAIL")
                  << " mpp_fd=" << imported_fd
                  << " mpp_size=" << imported_size
                  << " rga_import=" << (exported.rga_handle != 0 ? "PASS" : "FAIL")
                  << '\n';
        buffers.push_back(exported);
    }
    expbuf_pass = buffers.size() == request.count;

    if (!mpp_import_pass || !rga_import_pass) {
        cleanup();
        std::cerr << "DMA-BUF import audit failed\n";
        return 1;
    }

    MPP_RET mpp_result = mpp_buffer_get(nullptr, &destination, layout.mpp_buffer_bytes);
    if (mpp_result != MPP_OK || destination == nullptr) {
        cleanup();
        std::cerr << "mpp_buffer_get destination failed: MPP_RET=" << mpp_result << '\n';
        return 1;
    }
    const int destination_fd = mpp_buffer_get_fd(destination);
    destination_rga_handle = importbuffer_fd(destination_fd,
                                              static_cast<int>(layout.mpp_buffer_bytes));
    if (destination_rga_handle == 0) {
        cleanup();
        std::cerr << "RGA could not import MPP destination DMA-BUF\n";
        return 1;
    }

    for (const auto& buffer : buffers) {
        if (!queue_buffer(camera_fd, buffer.index, plane_count, &error)) {
            cleanup();
            std::cerr << error << '\n';
            return 1;
        }
    }
    uint32_t type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl_retry(camera_fd, VIDIOC_STREAMON, &type) < 0) {
        error = errno_message("VIDIOC_STREAMON");
        cleanup();
        std::cerr << error << '\n';
        return 1;
    }
    streaming = true;

    pollfd descriptor{camera_fd, POLLIN, 0};
    const int poll_result = ::poll(&descriptor, 1, 3000);
    if (poll_result <= 0) {
        error = poll_result == 0 ? "poll camera timed out" : errno_message("poll camera");
        cleanup();
        std::cerr << error << '\n';
        return 1;
    }

    v4l2_buffer captured{};
    std::array<v4l2_plane, VIDEO_MAX_PLANES> captured_planes{};
    captured.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    captured.memory = V4L2_MEMORY_MMAP;
    captured.m.planes = captured_planes.data();
    captured.length = plane_count;
    if (ioctl_retry(camera_fd, VIDIOC_DQBUF, &captured) < 0 || captured.index >= buffers.size()) {
        error = errno_message("VIDIOC_DQBUF");
        cleanup();
        std::cerr << error << '\n';
        return 1;
    }

    const rga_buffer_t source_image = wrapbuffer_handle(
        buffers[captured.index].rga_handle, static_cast<int>(width), static_cast<int>(height),
        RK_FORMAT_YCbCr_420_SP, static_cast<int>(stride), static_cast<int>(height));
    rga_buffer_t destination_image = wrapbuffer_handle(
        destination_rga_handle, static_cast<int>(width), static_cast<int>(height),
        RK_FORMAT_YCbCr_420_SP, static_cast<int>(layout.mpp_horizontal_stride),
        static_cast<int>(layout.mpp_vertical_stride));
    im_rect empty_rect{};
    rga_buffer_t empty_buffer{};
    const IM_STATUS check_result = imcheck_t(source_image, destination_image, empty_buffer,
                                             empty_rect, empty_rect, empty_rect, 0);
    IM_STATUS copy_result = IM_STATUS_FAILED;
    const auto copy_started = std::chrono::steady_clock::now();
    if (check_result == IM_STATUS_NOERROR || check_result == IM_STATUS_SUCCESS) {
        copy_result = imcopy(source_image, destination_image, 1);
    }
    const auto copy_finished = std::chrono::steady_clock::now();
    const double copy_ms = std::chrono::duration<double, std::milli>(copy_finished - copy_started).count();
    rga_copy_pass = copy_result == IM_STATUS_SUCCESS || copy_result == IM_STATUS_NOERROR;

    if (rga_copy_pass) {
        MppBuffer source_read_buffer = nullptr;
        MppBufferInfo source_read_info{};
        source_read_info.type = MPP_BUFFER_TYPE_EXT_DMA;
        source_read_info.size = buffers[captured.index].length;
        source_read_info.fd = buffers[captured.index].fd;
        source_read_info.index = static_cast<int>(captured.index);
        const MPP_RET source_import_result =
            mpp_buffer_import(&source_read_buffer, &source_read_info);
        auto* source_data = source_read_buffer != nullptr
                                ? static_cast<const uint8_t*>(mpp_buffer_get_ptr(source_read_buffer))
                                : nullptr;
        auto* destination_data = static_cast<const uint8_t*>(mpp_buffer_get_ptr(destination));
        if (source_import_result != MPP_OK || source_data == nullptr ||
            destination_data == nullptr) {
            rga_copy_pass = false;
        } else {
            (void)mpp_buffer_sync_ro_begin(source_read_buffer);
            (void)mpp_buffer_sync_ro_begin(destination);
            source_checksum = visible_nv12_checksum(source_data, width, height, stride,
                                                    layout.source_y_offset);
            destination_checksum = visible_nv12_checksum(
                destination_data, width, height, layout.mpp_horizontal_stride,
                layout.mpp_y_offset);
            (void)mpp_buffer_sync_ro_end(destination);
            (void)mpp_buffer_sync_ro_end(source_read_buffer);
            visible_content_match = source_checksum == destination_checksum;
        }
        if (source_read_buffer != nullptr) {
            mpp_buffer_put(source_read_buffer);
        }
    }
    rga_copy_pass = rga_copy_pass && visible_content_match;

    std::cout << "RGA_COPY frame_index=" << captured.index
              << " sequence=" << captured.sequence
              << " bytesused=" << captured_planes[0].bytesused
              << " check=" << imStrError_t(check_result)
              << " copy=" << imStrError_t(copy_result)
              << " copy_ms=" << copy_ms
              << " destination_fd=" << destination_fd
              << " source_visible_checksum=" << source_checksum
              << " destination_visible_checksum=" << destination_checksum
              << " visible_content_match=" << (visible_content_match ? "true" : "false")
              << '\n';

    (void)queue_buffer(camera_fd, captured.index, plane_count, nullptr);
    cleanup();

    const bool pass = expbuf_pass && mpp_import_pass && rga_import_pass && rga_copy_pass;
    std::cout << "DMABUF_AUDIT result=" << (pass ? "PASS" : "FAIL")
              << " expbuf=" << (expbuf_pass ? "PASS" : "FAIL")
              << " mpp_import=" << (mpp_import_pass ? "PASS" : "FAIL")
              << " rga_import=" << (rga_import_pass ? "PASS" : "FAIL")
              << " rga_align_copy=" << (rga_copy_pass ? "PASS" : "FAIL")
              << " direct_camera_to_mpp="
              << (layout.direct_mpp_layout_compatible ? "COMPATIBLE" : "LAYOUT_INCOMPATIBLE")
              << " recommended_path="
              << (layout.direct_mpp_layout_compatible ? "V4L2_DMABUF_TO_MPP"
                                                      : "V4L2_DMABUF_TO_RGA_TO_MPP")
              << '\n';
    return pass ? 0 : 1;
}
