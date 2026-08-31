#pragma once

#include <cstddef>
#include <cstdint>

namespace rkstream {

// H.264 Annex-B 裸码流由 00 00 01 或 00 00 00 01 起始码分隔 NAL 单元。
// 这里不做完整解码，只统计最关键的单元，作为编码结果的低成本结构性检查。
struct AnnexBStats {
    // 找到的全部 NAL 单元数量。
    std::size_t nal_units = 0;
    // SPS/PPS 描述解码参数；缺失时新播放器通常无法加入码流。
    std::size_t sps = 0;
    std::size_t pps = 0;
    // IDR 是可独立开始解码的关键帧，也是丢包后重新同步的锚点。
    std::size_t idr = 0;
};

// 扫描一段 Annex-B 数据并返回统计结果。函数不拥有 data，也不会越界读取。
AnnexBStats inspect_annexb(const uint8_t* data, std::size_t size);

}  // namespace rkstream
