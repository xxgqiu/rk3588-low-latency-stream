#pragma once

#include "rkstream/mpp_h264_encoder.h"
#include "rkstream/v4l2_dmabuf_capture.h"

#include <memory>
#include <string>
#include <vector>

namespace rkstream {

// 将相机 NV12 DMA-BUF 的可见区域复制到 MPP 对齐后的 NV12 DMA-BUF。
//
// 这条路径消除了 CPU memcpy，但仍有一次由 RGA 硬件执行的内存复制，所以准确说法
// 是“DMA-BUF + 硬件 copy”，而不是完全不搬运像素的纯 zero-copy。
//
// copy() 使用同步 RGA 调用：只有它返回成功，RGA 才不再读取源 DMA-BUF，此时
// capture.release() 才安全。若改为异步 RGA，就必须额外传递 fence，不能沿用这里
// 的生命周期假设。
class RgaNv12Aligner {
public:
    RgaNv12Aligner();
    ~RgaNv12Aligner();

    RgaNv12Aligner(const RgaNv12Aligner&) = delete;
    RgaNv12Aligner& operator=(const RgaNv12Aligner&) = delete;

    // 导入全部相机 DMA-BUF 和一个 MPP 目标 DMA-BUF。传入 fd 均为借用，不接管 close。
    bool open(const CaptureInfo& capture_info,
              const std::vector<DmabufBufferInfo>& source_buffers,
              int destination_fd, const MppEncoderInfo& destination_info,
              std::string* error);
    // source_index 必须是当前由应用持有 lease 的 V4L2 slot。
    bool copy(uint32_t source_index, std::string* error);
    void close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rkstream
