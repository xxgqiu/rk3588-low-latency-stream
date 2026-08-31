#pragma once

#include <cstddef>

namespace rkstream {

// 计算 DMA-BUF lease 队列的安全容量上限。
//
// 为什么不能像 CPU-copy 路径一样随意把队列设为 4？因为队列中的每一项都占着
// 一个尚未 QBUF 的相机 slot。假设一个 source lease 正被 RGA 使用，并至少留下
// 两个 slot 给驱动持续采集，只有其余 slot 能跨线程排队。容量过大会把全部 buffer
// 扣在应用侧，摄像头会因无可写入的 QBUF 而停顿。
constexpr std::size_t max_dmabuf_lease_queue_capacity(std::size_t camera_buffers) {
    constexpr std::size_t kEncoderLease = 1;
    constexpr std::size_t kDriverReserve = 2;
    constexpr std::size_t kReserved = kEncoderLease + kDriverReserve;
    return camera_buffers > kReserved ? camera_buffers - kReserved : 0;
}

constexpr bool valid_dmabuf_lease_queue_capacity(std::size_t camera_buffers,
                                                  std::size_t queue_capacity) {
    return queue_capacity > 0 &&
           queue_capacity <= max_dmabuf_lease_queue_capacity(camera_buffers);
}

}  // namespace rkstream
