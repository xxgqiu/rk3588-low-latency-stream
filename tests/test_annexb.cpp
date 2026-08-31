#include "rkstream/annexb.h"

#include <cstdint>
#include <iostream>

int main() {
    const uint8_t stream[] = {
        0, 0, 0, 1, 0x67, 1, 2,
        0, 0, 1, 0x68, 3,
        0, 0, 0, 1, 0x65, 4, 5,
        0, 0, 1, 0x41, 6,
    };
    const rkstream::AnnexBStats stats = rkstream::inspect_annexb(stream, sizeof(stream));
    if (stats.nal_units != 4 || stats.sps != 1 || stats.pps != 1 || stats.idr != 1) {
        std::cerr << "unexpected Annex-B stats\n";
        return 1;
    }
    return 0;
}
