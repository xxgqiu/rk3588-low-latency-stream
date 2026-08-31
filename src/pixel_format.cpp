#include "rkstream/pixel_format.h"

#include <linux/videodev2.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace rkstream {

uint32_t parse_pixel_format(const std::string& name) {
    std::string normalized = name;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (normalized == "nv12") {
        return V4L2_PIX_FMT_NV12;
    }
    if (normalized == "yuyv" || normalized == "yuy2") {
        return V4L2_PIX_FMT_YUYV;
    }
    if (normalized == "mjpeg" || normalized == "mjpg") {
        return V4L2_PIX_FMT_MJPEG;
    }
    throw std::invalid_argument("unsupported pixel format: " + name);
}

std::string pixel_format_name(uint32_t fourcc) {
    std::string result(4, ' ');
    result[0] = static_cast<char>(fourcc & 0xffU);
    result[1] = static_cast<char>((fourcc >> 8U) & 0xffU);
    result[2] = static_cast<char>((fourcc >> 16U) & 0xffU);
    result[3] = static_cast<char>((fourcc >> 24U) & 0xffU);
    return result;
}

}  // namespace rkstream
