#include "rkstream/mpp_h264_encoder.h"

extern "C" {
#include <mpp_buffer.h>
#include <mpp_err.h>
#include <mpp_frame.h>
#include <mpp_packet.h>
#include <rk_mpi.h>
#include <rk_mpi_cmd.h>
#include <rk_venc_cfg.h>
#include <rk_venc_cmd.h>
#include <rk_venc_rc.h>
}

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

namespace rkstream {
namespace {

uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) / alignment * alignment;
}

std::string mpp_error(const char* operation, MPP_RET code) {
    std::ostringstream stream;
    stream << operation << " failed: MPP_RET=" << code;
    return stream.str();
}

bool set_config(MppEncCfg config, const char* key, int32_t value, std::string* error) {
    const MPP_RET result = mpp_enc_cfg_set_s32(config, key, value);
    if (result != MPP_OK && error != nullptr) {
        *error = mpp_error(key, result);
    }
    return result == MPP_OK;
}

}  // namespace

class MppH264Encoder::Impl {
public:
    bool open(const MppEncoderConfig& config, std::string* error) {
        close();
        if (config.width == 0 || config.height == 0 || config.source_stride < config.width ||
            config.fps == 0 || config.bitrate == 0 || config.gop == 0) {
            if (error != nullptr) {
                *error = "invalid MPP encoder configuration";
            }
            return false;
        }

        // MPP 按 stride 解释内存。可见高度 1224 会向上对齐为 1232，UV 平面因此
        // 必须从 1632*1232 开始；不能沿用相机的 1632*1224 偏移。
        config_ = config;
        info_.horizontal_stride = align_up(config.source_stride, 16);
        info_.vertical_stride = align_up(config.height, 16);
        const uint64_t input_bytes = static_cast<uint64_t>(info_.horizontal_stride) *
                                     info_.vertical_stride * 3ULL / 2ULL;
        if (input_bytes > std::numeric_limits<std::size_t>::max()) {
            if (error != nullptr) {
                *error = "MPP input buffer size overflow";
            }
            return false;
        }
        info_.input_buffer_bytes = static_cast<std::size_t>(input_bytes);

        // 1. 创建通用 MPP context，再初始化为 AVC(H.264) 编码器。
        MPP_RET result = mpp_create(&context_, &api_);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("mpp_create", result);
            }
            close();
            return false;
        }
        result = mpp_init(context_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("mpp_init H.264", result);
            }
            close();
            return false;
        }
        result = mpp_enc_cfg_init(&encoder_config_);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("mpp_enc_cfg_init", result);
            }
            close();
            return false;
        }

        // 2. 配置输入布局、CBR 码控、输入/输出帧率、GOP 和 H.264 High Profile。
        //    CBR 的 min/max 留出 ±10%，不是每一秒都严格等于 target bitrate。
        const int32_t bitrate = static_cast<int32_t>(config.bitrate);
        const int32_t minimum_bitrate = static_cast<int32_t>(config.bitrate * 9ULL / 10ULL);
        const int32_t maximum_bitrate = static_cast<int32_t>(config.bitrate * 11ULL / 10ULL);
        const bool configured =
            set_config(encoder_config_, "prep:width", static_cast<int32_t>(config.width), error) &&
            set_config(encoder_config_, "prep:height", static_cast<int32_t>(config.height), error) &&
            set_config(encoder_config_, "prep:hor_stride",
                       static_cast<int32_t>(info_.horizontal_stride), error) &&
            set_config(encoder_config_, "prep:ver_stride",
                       static_cast<int32_t>(info_.vertical_stride), error) &&
            set_config(encoder_config_, "prep:format", MPP_FMT_YUV420SP, error) &&
            set_config(encoder_config_, "rc:mode", MPP_ENC_RC_MODE_CBR, error) &&
            set_config(encoder_config_, "rc:bps_target", bitrate, error) &&
            set_config(encoder_config_, "rc:bps_min", minimum_bitrate, error) &&
            set_config(encoder_config_, "rc:bps_max", maximum_bitrate, error) &&
            set_config(encoder_config_, "rc:fps_in_flex", 0, error) &&
            set_config(encoder_config_, "rc:fps_in_num", static_cast<int32_t>(config.fps), error) &&
            set_config(encoder_config_, "rc:fps_in_denom", 1, error) &&
            set_config(encoder_config_, "rc:fps_out_flex", 0, error) &&
            set_config(encoder_config_, "rc:fps_out_num", static_cast<int32_t>(config.fps), error) &&
            set_config(encoder_config_, "rc:fps_out_denom", 1, error) &&
            set_config(encoder_config_, "rc:gop", static_cast<int32_t>(config.gop), error) &&
            set_config(encoder_config_, "h264:profile", 100, error) &&
            set_config(encoder_config_, "h264:level", 40, error) &&
            set_config(encoder_config_, "h264:cabac_en", 1, error) &&
            set_config(encoder_config_, "h264:cabac_idc", 0, error);
        if (!configured) {
            close();
            return false;
        }

        result = api_->control(context_, MPP_ENC_SET_CFG, encoder_config_);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("MPP_ENC_SET_CFG", result);
            }
            close();
            return false;
        }

        MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_DEFAULT;
        (void)api_->control(context_, MPP_ENC_SET_HEADER_MODE, &header_mode);

        // 3. 分配固定大小的 MPP DMA 缓冲。整个运行期间复用，避免每帧分配。
        //    input 可由 CPU 映射写入，也可把 fd 借给 RGA；output 保存一帧编码结果。
        result = mpp_buffer_get(nullptr, &input_buffer_, info_.input_buffer_bytes);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("mpp_buffer_get input", result);
            }
            close();
            return false;
        }
        result = mpp_buffer_get(nullptr, &output_buffer_, info_.input_buffer_bytes);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("mpp_buffer_get output", result);
            }
            close();
            return false;
        }

        // 4. 同步取得 SPS/PPS。GET_HDR_SYNC 不会替调用者分配 packet storage，
        //    packet 必须包住真实 MppBuffer；传空 packet 在部分 Rockchip 版本会崩溃。
        MppPacket header_packet = nullptr;
        result = mpp_packet_init_with_buffer(&header_packet, output_buffer_);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("mpp_packet_init header", result);
            }
            close();
            return false;
        }
        mpp_packet_set_length(header_packet, 0);
        result = api_->control(context_, MPP_ENC_GET_HDR_SYNC, header_packet);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("MPP_ENC_GET_HDR_SYNC", result);
            }
            mpp_packet_deinit(&header_packet);
            close();
            return false;
        }
        const auto* header_data = static_cast<const uint8_t*>(mpp_packet_get_pos(header_packet));
        const std::size_t header_size = mpp_packet_get_length(header_packet);
        if (header_data != nullptr && header_size > 0) {
            header_.assign(header_data, header_data + header_size);
        }
        info_.header_bytes = header_.size();
        mpp_packet_deinit(&header_packet);
        if (header_.empty()) {
            if (error != nullptr) {
                *error = "MPP returned an empty H.264 header";
            }
            close();
            return false;
        }
        return true;
    }

    bool encode_nv12(const std::byte* data, std::size_t size, int64_t pts,
                     EncodedPacket* output, std::string* error) {
        if (context_ == nullptr || api_ == nullptr || input_buffer_ == nullptr ||
            data == nullptr || output == nullptr) {
            if (error != nullptr) {
                *error = "MPP encoder is not ready";
            }
            return false;
        }

        const std::size_t source_y_bytes = static_cast<std::size_t>(config_.source_stride) *
                                           config_.height;
        const std::size_t required_source_bytes = source_y_bytes + source_y_bytes / 2;
        if (size < required_source_bytes) {
            if (error != nullptr) {
                *error = "NV12 source buffer is smaller than stride * height * 3/2";
            }
            return false;
        }

        auto* destination = static_cast<uint8_t*>(mpp_buffer_get_ptr(input_buffer_));
        if (destination == nullptr) {
            if (error != nullptr) {
                *error = "mpp_buffer_get_ptr returned null";
            }
            return false;
        }
        // CPU-copy 基线必须逐行复制：源 stride、目标 stride 和 UV 起点都可能不同。
        // sync_begin/end 告诉 DMA/CPU 缓存管理，CPU 即将写入并已写完该 MPP buffer。
        mpp_buffer_sync_begin(input_buffer_);
        std::memset(destination, 0, info_.input_buffer_bytes);
        const auto* source = reinterpret_cast<const uint8_t*>(data);
        for (uint32_t row = 0; row < config_.height; ++row) {
            std::memcpy(destination + static_cast<std::size_t>(row) * info_.horizontal_stride,
                        source + static_cast<std::size_t>(row) * config_.source_stride,
                        config_.width);
        }
        uint8_t* destination_uv = destination +
            static_cast<std::size_t>(info_.horizontal_stride) * info_.vertical_stride;
        const uint8_t* source_uv = source + source_y_bytes;
        for (uint32_t row = 0; row < config_.height / 2; ++row) {
            std::memcpy(destination_uv + static_cast<std::size_t>(row) * info_.horizontal_stride,
                        source_uv + static_cast<std::size_t>(row) * config_.source_stride,
                        config_.width);
        }
        mpp_buffer_sync_end(input_buffer_);

        return encode_prepared_nv12(pts, output, error);
    }

    bool encode_prepared_nv12(int64_t pts, EncodedPacket* output, std::string* error) {
        if (context_ == nullptr || api_ == nullptr || input_buffer_ == nullptr ||
            output_buffer_ == nullptr || output == nullptr) {
            if (error != nullptr) {
                *error = "MPP encoder is not ready for a prepared DMA-BUF frame";
            }
            return false;
        }

        // MppFrame 只是一次提交的描述符；像素实际位于长期复用的 input_buffer_。
        MppFrame frame = nullptr;
        MPP_RET result = mpp_frame_init(&frame);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("mpp_frame_init", result);
            }
            return false;
        }
        mpp_frame_set_width(frame, config_.width);
        mpp_frame_set_height(frame, config_.height);
        mpp_frame_set_hor_stride(frame, info_.horizontal_stride);
        mpp_frame_set_ver_stride(frame, info_.vertical_stride);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_pts(frame, pts);
        mpp_frame_set_eos(frame, 0);
        mpp_frame_set_buffer(frame, input_buffer_);

        // 复用一个有界 MPP 输出缓冲。KEY_OUTPUT_PACKET 告诉编码器把结果写到哪里；
        // 每次提交前必须把 packet length 清零，否则上次结果长度会污染本次编码。
        MppPacket packet = nullptr;
        result = mpp_packet_init_with_buffer(&packet, output_buffer_);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("mpp_packet_init_with_buffer", result);
            }
            mpp_frame_deinit(&frame);
            return false;
        }
        mpp_packet_set_length(packet, 0);
        MppMeta metadata = mpp_frame_get_meta(frame);
        result = mpp_meta_set_packet(metadata, KEY_OUTPUT_PACKET, packet);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("mpp_meta_set_packet", result);
            }
            mpp_packet_deinit(&packet);
            mpp_frame_deinit(&frame);
            return false;
        }

        // MPP 的标准同步提交/取包流程：put_frame 后 frame 描述符即可释放，随后
        // get_packet 等待当前 access unit。应用再复制压缩字节到 vector 跨线程传递。
        result = api_->encode_put_frame(context_, frame);
        mpp_frame_deinit(&frame);
        if (result != MPP_OK) {
            if (error != nullptr) {
                *error = mpp_error("encode_put_frame", result);
            }
            mpp_packet_deinit(&packet);
            return false;
        }

        result = api_->encode_get_packet(context_, &packet);
        if (result != MPP_OK || packet == nullptr) {
            if (error != nullptr) {
                *error = result == MPP_OK ? "encoder returned no packet"
                                         : mpp_error("encode_get_packet", result);
            }
            if (packet != nullptr) {
                mpp_packet_deinit(&packet);
            }
            return false;
        }
        const auto* packet_data = static_cast<const uint8_t*>(mpp_packet_get_pos(packet));
        const std::size_t packet_size = mpp_packet_get_length(packet);
        output->bytes.assign(packet_data, packet_data + packet_size);
        output->pts = mpp_packet_get_pts(packet);
        mpp_packet_deinit(&packet);
        return !output->bytes.empty();
    }

    int input_dmabuf_fd() const {
        return input_buffer_ != nullptr ? mpp_buffer_get_fd(input_buffer_) : -1;
    }

    void close() {
        // MPP 资源有明确依赖关系，按 buffer -> cfg -> context 的逆序释放。
        header_.clear();
        info_ = {};
        if (input_buffer_ != nullptr) {
            mpp_buffer_put(input_buffer_);
            input_buffer_ = nullptr;
        }
        if (output_buffer_ != nullptr) {
            mpp_buffer_put(output_buffer_);
            output_buffer_ = nullptr;
        }
        if (encoder_config_ != nullptr) {
            mpp_enc_cfg_deinit(encoder_config_);
            encoder_config_ = nullptr;
        }
        if (context_ != nullptr) {
            mpp_destroy(context_);
            context_ = nullptr;
            api_ = nullptr;
        }
        config_ = {};
    }

    ~Impl() {
        close();
    }

    MppEncoderConfig config_;
    MppEncoderInfo info_;
    MppCtx context_ = nullptr;
    MppApi* api_ = nullptr;
    MppEncCfg encoder_config_ = nullptr;
    MppBuffer input_buffer_ = nullptr;
    MppBuffer output_buffer_ = nullptr;
    std::vector<uint8_t> header_;
};

MppH264Encoder::MppH264Encoder() : impl_(std::make_unique<Impl>()) {}

MppH264Encoder::~MppH264Encoder() = default;

bool MppH264Encoder::open(const MppEncoderConfig& config, std::string* error) {
    return impl_->open(config, error);
}

bool MppH264Encoder::encode_nv12(const std::byte* data, std::size_t size, int64_t pts,
                                 EncodedPacket* packet, std::string* error) {
    return impl_->encode_nv12(data, size, pts, packet, error);
}

bool MppH264Encoder::encode_prepared_nv12(int64_t pts, EncodedPacket* packet,
                                          std::string* error) {
    return impl_->encode_prepared_nv12(pts, packet, error);
}

void MppH264Encoder::close() {
    impl_->close();
}

const std::vector<uint8_t>& MppH264Encoder::header() const {
    return impl_->header_;
}

const MppEncoderInfo& MppH264Encoder::info() const {
    return impl_->info_;
}

int MppH264Encoder::input_dmabuf_fd() const {
    return impl_->input_dmabuf_fd();
}

}  // namespace rkstream
