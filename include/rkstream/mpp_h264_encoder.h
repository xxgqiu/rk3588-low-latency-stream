#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rkstream {

// MPP 编码器配置。source_stride 是相机源布局；encoder 内部会计算满足 MPP
// 要求的 horizontal/vertical stride，并通过 info() 暴露给 CPU/RGA 写入端。
struct MppEncoderConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t source_stride = 0;
    uint32_t fps = 0;
    uint32_t bitrate = 0;
    uint32_t gop = 0;
};

// 一个完整 H.264 access unit（通常对应一张输入图像）及其可观测元数据。
// bytes 由该对象独立拥有，所以可以安全地跨线程进入网络队列。
struct EncodedPacket {
    std::vector<uint8_t> bytes;

    // FFmpeg/RTSP 使用的显示时间戳。编码器透传调用者传入的 pts；当前程序传入
    // V4L2 driver timestamp（微秒），RtspPublisher 再换算到 90 kHz 时间基准。
    int64_t pts = 0;

    // encoded_sequence 检查编码包队列丢包；source_sequence 对照摄像头驱动帧序号。
    uint64_t encoded_sequence = 0;
    uint32_t source_sequence = 0;

    // 两个单调时钟时间点用于计算端到端内部处理延迟。
    uint64_t source_dequeue_timestamp_us = 0;
    uint64_t encoded_timestamp_us = 0;

    // true 表示 IDR；网络发生 gap 后必须等待它才能让新解码器重新同步。
    bool keyframe = false;
};

// 编码器真正采用的缓冲布局和 SPS/PPS 大小。
struct MppEncoderInfo {
    uint32_t horizontal_stride = 0;
    uint32_t vertical_stride = 0;
    std::size_t input_buffer_bytes = 0;
    std::size_t header_bytes = 0;
};

// Rockchip MPP H.264 硬件编码器的 RAII 封装。
//
// 提供两条输入路径：
//   encode_nv12()          CPU-copy 基线，把普通内存逐行复制进 MPP DMA-BUF；
//   encode_prepared_nv12() DMA 路径，假设 RGA 已写好同一 input DMA-BUF。
//
// 两条路径共用完全相同的 MPP 配置，便于公平比较 CPU 拷贝与 DMA-BUF/RGA。
class MppH264Encoder {
public:
    MppH264Encoder();
    ~MppH264Encoder();

    MppH264Encoder(const MppH264Encoder&) = delete;
    MppH264Encoder& operator=(const MppH264Encoder&) = delete;

    // 初始化 MPP context、编码配置、输入/输出 buffer，并同步取得 SPS/PPS header。
    bool open(const MppEncoderConfig& config, std::string* error);

    // data 是源 NV12 布局；函数在返回前完成复制和同步编码。
    bool encode_nv12(const std::byte* data, std::size_t size, int64_t pts,
                     EncodedPacket* packet, std::string* error);
    // 编码已经由 RGA/其他 DMA 设备写入内部 input buffer 的一帧。
    // 调用者必须保证硬件写操作已同步完成，且布局与 info() 完全一致。
    bool encode_prepared_nv12(int64_t pts, EncodedPacket* packet, std::string* error);
    void close();

    // header 是借用引用，生命周期不超过 encoder；通常在打开 RTSP muxer 时使用。
    const std::vector<uint8_t>& header() const;
    const MppEncoderInfo& info() const;
    // 返回借用的 DMA-BUF fd，所有权仍属于 encoder，调用者不得 close。
    int input_dmabuf_fd() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rkstream
