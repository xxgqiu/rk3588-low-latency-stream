#pragma once

#include "rkstream/mpp_h264_encoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rkstream {

struct RtspPublisherConfig {
    std::string url;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
};

// 使用 FFmpeg RTSP muxer 发布已经编码好的 H.264 Annex-B access unit。
//
// 本类只负责“封装与发送”，不负责编码：这样网络阻塞发生在独立线程中，不会直接
// 卡住摄像头 DQBUF。真正提供 RTSP 服务的是 MediaMTX，本类作为 publisher 连接它。
class RtspPublisher {
public:
    RtspPublisher();
    ~RtspPublisher();

    RtspPublisher(const RtspPublisher&) = delete;
    RtspPublisher& operator=(const RtspPublisher&) = delete;

    // h264_header 必须包含 SPS/PPS，供后加入的客户端建立解码参数。
    bool open(const RtspPublisherConfig& config, const std::vector<uint8_t>& h264_header,
              std::string* error);
    // 同步写一个 access unit；调用方应在网络线程中调用。
    bool write(const EncodedPacket& packet, std::string* error);
    void close();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rkstream
