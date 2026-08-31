#include "rkstream/pixel_format.h"

#include <linux/videodev2.h>

#include <iostream>
#include <stdexcept>

namespace {

bool check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    }
    return condition;
}

}  // namespace

#define CHECK(expression)                                      \
    do {                                                       \
        if (!check((expression), #expression, __LINE__)) {     \
            return 1;                                          \
        }                                                      \
    } while (false)

int main() {
    CHECK(rkstream::parse_pixel_format("nv12") == V4L2_PIX_FMT_NV12);
    CHECK(rkstream::parse_pixel_format("YUYV") == V4L2_PIX_FMT_YUYV);
    CHECK(rkstream::parse_pixel_format("mjpg") == V4L2_PIX_FMT_MJPEG);
    CHECK(rkstream::pixel_format_name(V4L2_PIX_FMT_NV12) == "NV12");

    bool rejected_unknown = false;
    try {
        (void)rkstream::parse_pixel_format("rgb");
    } catch (const std::invalid_argument&) {
        rejected_unknown = true;
    }
    CHECK(rejected_unknown);
    return 0;
}
