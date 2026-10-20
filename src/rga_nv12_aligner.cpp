#include "rkstream/rga_nv12_aligner.h"

#include <im2d.h>

#include <string>
#include <utility>
#include <vector>

namespace rkstream {

class RgaNv12Aligner::Impl {
public:
    bool open(const CaptureInfo& capture_info,
              const std::vector<DmabufBufferInfo>& source_buffers,
              int destination_fd,
              uint32_t destination_width,
              uint32_t destination_height,
              const MppEncoderInfo& destination_info,
              std::string* error) {
        close();
        if (source_buffers.empty() || destination_fd < 0 ||
            capture_info.width == 0 || capture_info.height == 0 ||
            destination_width == 0 || destination_height == 0 ||
            destination_info.input_buffer_bytes == 0) {
            if (error != nullptr) {
                *error = "invalid RGA aligner configuration";
            }
            return false;
        }

        // importbuffer_fd 让 RGA 认识现有 DMA-BUF；它不复制像素，也不取得 fd 所有权。
        // 导入动作只在初始化做一次，不能放进逐帧热路径。
        destination_handle_ = importbuffer_fd(
            destination_fd, static_cast<int>(destination_info.input_buffer_bytes));
        if (destination_handle_ == 0) {
            if (error != nullptr) {
                *error = "RGA could not import MPP destination DMA-BUF";
            }
            return false;
        }
        // wrapbuffer_handle 描述“如何解释这块内存”。可见尺寸仍是 width/height，
        // 但目标 stride 使用 MPP 的对齐值，RGA 会把 UV 写到正确目标偏移。
        destination_ = wrapbuffer_handle(
            destination_handle_, static_cast<int>(destination_width),
            static_cast<int>(destination_height), RK_FORMAT_YCbCr_420_SP,
            static_cast<int>(destination_info.horizontal_stride),
            static_cast<int>(destination_info.vertical_stride));
        resize_required_ = capture_info.width != destination_width ||
                           capture_info.height != destination_height;

        source_handles_.reserve(source_buffers.size());
        source_images_.resize(source_buffers.size());
        source_registered_.assign(source_buffers.size(), false);
        // camera slot index 必须稠密，因为 DQBUF 返回 index 后会直接 O(1) 查表。
        for (const auto& source : source_buffers) {
            if (source.index >= source_images_.size()) {
                if (error != nullptr) {
                    *error = "RGA source index is not dense";
                }
                close();
                return false;
            }
            const rga_buffer_handle_t handle =
                importbuffer_fd(source.fd, static_cast<int>(source.length));
            if (handle == 0) {
                if (error != nullptr) {
                    *error = "RGA could not import camera DMA-BUF index=" +
                             std::to_string(source.index);
                }
                close();
                return false;
            }
            source_handles_.push_back(handle);
            source_images_[source.index] = wrapbuffer_handle(
                handle, static_cast<int>(capture_info.width),
                static_cast<int>(capture_info.height), RK_FORMAT_YCbCr_420_SP,
                static_cast<int>(capture_info.bytes_per_line),
                static_cast<int>(capture_info.height));
            source_registered_[source.index] = true;
        }

        // 在真正开流前对每个源布局做 imcheck，尽早暴露格式/stride 不受支持的问题。
        rga_buffer_t empty_buffer{};
        im_rect empty_rect{};
        for (uint32_t index = 0; index < source_images_.size(); ++index) {
            if (!source_registered_[index]) {
                if (error != nullptr) {
                    *error = "RGA source index is missing: " + std::to_string(index);
                }
                close();
                return false;
            }
            const IM_STATUS status = imcheck_t(source_images_[index], destination_, empty_buffer,
                                               empty_rect, empty_rect, empty_rect, 0);
            if (status != IM_STATUS_NOERROR && status != IM_STATUS_SUCCESS) {
                if (error != nullptr) {
                    *error = std::string("RGA layout check failed: ") + imStrError_t(status);
                }
                close();
                return false;
            }
        }
        return true;
    }

    bool copy(uint32_t source_index, std::string* error) {
        if (source_index >= source_images_.size() || !source_registered_[source_index] ||
            destination_handle_ == 0) {
            if (error != nullptr) {
                *error = "invalid RGA source buffer index";
            }
            return false;
        }
        // sync=1：返回时硬件处理已结束，源 lease 此刻即可安全 QBUF。
        const IM_STATUS status = resize_required_
            ? imresize(source_images_[source_index], destination_,
                       0.0, 0.0, INTER_LINEAR, 1)
            : imcopy(source_images_[source_index], destination_, 1);
        if (status != IM_STATUS_NOERROR && status != IM_STATUS_SUCCESS) {
            if (error != nullptr) {
                *error = std::string(resize_required_ ? "RGA resize failed: "
                                                      : "RGA copy failed: ") +
                         imStrError_t(status);
            }
            return false;
        }
        return true;
    }

    void close() {
        // releasebuffer_handle 只撤销 RGA 导入；真正的 fd 分别由 capture/encoder 持有。
        for (const auto handle : source_handles_) {
            if (handle != 0) {
                (void)releasebuffer_handle(handle);
            }
        }
        source_handles_.clear();
        source_images_.clear();
        source_registered_.clear();
        if (destination_handle_ != 0) {
            (void)releasebuffer_handle(destination_handle_);
            destination_handle_ = 0;
        }
        destination_ = {};
        resize_required_ = false;
    }

    ~Impl() {
        close();
    }

private:
    std::vector<rga_buffer_handle_t> source_handles_;
    std::vector<rga_buffer_t> source_images_;
    std::vector<bool> source_registered_;
    rga_buffer_handle_t destination_handle_ = 0;
    rga_buffer_t destination_{};
    bool resize_required_ = false;
};

RgaNv12Aligner::RgaNv12Aligner() : impl_(std::make_unique<Impl>()) {}

RgaNv12Aligner::~RgaNv12Aligner() = default;

bool RgaNv12Aligner::open(const CaptureInfo& capture_info,
                          const std::vector<DmabufBufferInfo>& source_buffers,
                          int destination_fd,
                          uint32_t destination_width,
                          uint32_t destination_height,
                          const MppEncoderInfo& destination_info,
                          std::string* error) {
    return impl_->open(capture_info, source_buffers, destination_fd,
                       destination_width, destination_height,
                       destination_info, error);
}

bool RgaNv12Aligner::copy(uint32_t source_index, std::string* error) {
    return impl_->copy(source_index, error);
}

void RgaNv12Aligner::close() {
    impl_->close();
}

}  // namespace rkstream
