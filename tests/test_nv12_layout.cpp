#include "rkstream/nv12_layout.h"

int main() {
    constexpr auto ov8858 = rkstream::audit_nv12_layout(1632, 1224, 1632, 2'996'352);
    static_assert(ov8858.mpp_horizontal_stride == 1632);
    static_assert(ov8858.mpp_vertical_stride == 1232);
    static_assert(ov8858.source_y_offset == 1'997'568);
    static_assert(ov8858.mpp_y_offset == 2'010'624);
    static_assert(ov8858.source_buffer_bytes == 2'996'352);
    static_assert(ov8858.mpp_buffer_bytes == 3'015'936);
    static_assert(ov8858.alignment_padding_bytes == 19'584);
    static_assert(!ov8858.direct_mpp_layout_compatible);

    constexpr auto aligned = rkstream::audit_nv12_layout(1920, 1088, 1920, 3'133'440);
    static_assert(aligned.mpp_horizontal_stride == 1920);
    static_assert(aligned.mpp_vertical_stride == 1088);
    static_assert(aligned.source_y_offset == aligned.mpp_y_offset);
    static_assert(aligned.source_buffer_bytes == aligned.mpp_buffer_bytes);
    static_assert(aligned.alignment_padding_bytes == 0);
    static_assert(aligned.direct_mpp_layout_compatible);

    constexpr auto computed = rkstream::audit_nv12_layout(640, 480, 640, 0);
    static_assert(computed.source_buffer_bytes == 460'800);
    static_assert(computed.direct_mpp_layout_compatible);
    return 0;
}
