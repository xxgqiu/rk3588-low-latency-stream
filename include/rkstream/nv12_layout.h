#pragma once

#include <cstddef>
#include <cstdint>

namespace rkstream {

// 将 value 向上取整到 alignment 的整数倍。alignment=0 表示不做对齐。
// MPP 常要求宽高 stride 按 16 对齐，这个小函数集中表达该规则。
constexpr uint32_t align_up(uint32_t value, uint32_t alignment) {
    return alignment == 0 ? value : (value + alignment - 1U) / alignment * alignment;
}

struct Nv12LayoutAudit {
    // width/height 是可见区域；stride 是内存布局，两者不能混为一谈。
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t source_stride = 0;
    uint32_t mpp_horizontal_stride = 0;
    uint32_t mpp_vertical_stride = 0;

    // 名称保留了早期审计字段约定；实际含义是 UV 平面起始偏移：
    // NV12 的 Y 平面之后紧跟交错 UV 平面。
    std::size_t source_y_offset = 0;
    std::size_t mpp_y_offset = 0;

    // 源驱动缓冲和 MPP 对齐缓冲各自需要的总字节数。
    std::size_t source_buffer_bytes = 0;
    std::size_t mpp_buffer_bytes = 0;
    std::size_t alignment_padding_bytes = 0;

    // true 才能让 MPP 直接按目标布局读取源缓冲；false 时必须重排/复制。
    bool direct_mpp_layout_compatible = false;
};

// 比较摄像头 NV12 布局与 MPP 期望布局。
//
// 例如 OV8858 输出 1632x1224：宽 stride 已满足 16 对齐，但 1224 高度不是
// 16 的倍数，MPP vertical stride 为 1232。因此 UV 起点不同，不能把相机
// fd 直接交给 MPP；本项目用 RGA 把可见区域复制到 MPP 的对齐 DMA-BUF。
constexpr Nv12LayoutAudit audit_nv12_layout(uint32_t width, uint32_t height,
                                             uint32_t source_stride,
                                             std::size_t driver_size_image,
                                             uint32_t mpp_alignment = 16) {
    Nv12LayoutAudit result{};
    result.width = width;
    result.height = height;
    result.source_stride = source_stride;
    result.mpp_horizontal_stride = align_up(source_stride, mpp_alignment);
    result.mpp_vertical_stride = align_up(height, mpp_alignment);
    result.source_y_offset = static_cast<std::size_t>(source_stride) * height;
    result.mpp_y_offset = static_cast<std::size_t>(result.mpp_horizontal_stride) *
                          result.mpp_vertical_stride;

    const std::size_t computed_source_bytes = result.source_y_offset * 3U / 2U;
    result.source_buffer_bytes = driver_size_image == 0 ? computed_source_bytes
                                                        : driver_size_image;
    result.mpp_buffer_bytes = result.mpp_y_offset * 3U / 2U;
    result.alignment_padding_bytes = result.mpp_buffer_bytes > result.source_buffer_bytes
                                         ? result.mpp_buffer_bytes - result.source_buffer_bytes
                                         : 0;
    result.direct_mpp_layout_compatible =
        source_stride == result.mpp_horizontal_stride &&
        height == result.mpp_vertical_stride &&
        result.source_y_offset == result.mpp_y_offset &&
        result.source_buffer_bytes >= result.mpp_buffer_bytes;
    return result;
}

}  // namespace rkstream
