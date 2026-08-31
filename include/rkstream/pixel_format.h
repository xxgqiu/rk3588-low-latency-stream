#pragma once

#include <cstdint>
#include <string>

namespace rkstream {

// 把命令行中的易读名称（nv12/yuyv/mjpeg）转换为 V4L2 fourcc。
// 不支持的名称抛出 std::invalid_argument，由最外层参数处理统一输出错误。
uint32_t parse_pixel_format(const std::string& name);

// 把 fourcc 转成可打印字符串。调试格式协商失败时比十六进制整数直观得多。
std::string pixel_format_name(uint32_t fourcc);

}  // namespace rkstream
