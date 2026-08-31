#include "rkstream/annexb.h"

namespace rkstream {
namespace {

// Annex-B 同时允许 3 字节和 4 字节起始码。先做边界检查，再访问后续字节，
// 因此即使输入在起始码中途截断也不会越界。
std::size_t start_code_length(const uint8_t* data, std::size_t size, std::size_t offset) {
    if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 1) {
        return 3;
    }
    if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 &&
        data[offset + 2] == 0 && data[offset + 3] == 1) {
        return 4;
    }
    return 0;
}

}  // namespace

AnnexBStats inspect_annexb(const uint8_t* data, std::size_t size) {
    AnnexBStats stats;
    if (data == nullptr) {
        return stats;
    }

    for (std::size_t offset = 0; offset < size;) {
        const std::size_t prefix = start_code_length(data, size, offset);
        if (prefix == 0) {
            ++offset;
            continue;
        }
        const std::size_t header = offset + prefix;
        if (header >= size) {
            break;
        }

        ++stats.nal_units;
        // H.264 NAL header 的低 5 bit 是 nal_unit_type：5=IDR、7=SPS、8=PPS。
        switch (data[header] & 0x1fU) {
            case 5:
                ++stats.idr;
                break;
            case 7:
                ++stats.sps;
                break;
            case 8:
                ++stats.pps;
                break;
            default:
                break;
        }
        offset = header + 1;
    }
    return stats;
}

}  // namespace rkstream
