#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rkstream {

// CPU 内存中的一帧完整图像。
//
// 这是项目最容易理解、也最安全的基线表示：V4L2 驱动 DQBUF 后，采集线程把
// 驱动缓冲区中的有效字节复制进 data，然后立即把驱动缓冲区 QBUF。这样 Frame
// 离开 capture() 后仍完全有效，但代价是每帧有一次 CPU memcpy。
//
// DMA-BUF 优化路径不使用该类型保存像素，而使用 DmabufFrame 表示“临时租约”。
struct Frame {
    // V4L2 驱动给出的帧序号。它用于检查摄像头侧是否发生跳帧。
    uint32_t sequence = 0;

    // 摄像头驱动写入 v4l2_buffer.timestamp 的时间，单位微秒。
    // 注意它使用哪一种时钟由 timestamp_flags 指示，不能直接当墙上时间显示。
    uint64_t driver_timestamp_us = 0;

    // 本程序完成 DQBUF 时读取的 CLOCK_MONOTONIC 时间，单位微秒。
    // 它适合计算帧在本进程中的排队和处理延迟。
    uint64_t dequeue_timestamp_us = 0;

    // 原始 V4L2 timestamp flags，保留下来便于审计驱动时间戳语义。
    uint32_t timestamp_flags = 0;

    // V4L2 fourcc，例如 V4L2_PIX_FMT_NV12 / YUYV / MJPEG。
    uint32_t pixel_format = 0;

    // 可见图像尺寸。它们不一定等于内存中每行的实际跨度。
    uint32_t width = 0;
    uint32_t height = 0;

    // 一行像素在驱动缓冲区中占用的字节数（stride/pitch）。
    // 处理图像时必须使用它，不能默认 bytes_per_line == width。
    uint32_t bytes_per_line = 0;

    // 当前帧有效数据的独立副本。该 vector 拥有内存，生命周期不依赖 V4L2。
    std::vector<std::byte> data;
};

}  // namespace rkstream
