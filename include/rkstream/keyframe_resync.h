#pragma once

#include <cstdint>

namespace rkstream {

struct KeyframeResyncDecision {
    // accept=false 表示该 P 帧应丢弃，直到新的 IDR 到来。
    bool accept = false;
    // gap_detected=true 表示本次输入序号与上一个包不连续。
    bool gap_detected = false;
};

// H.264 丢包后的关键帧重同步状态机。
//
// P 帧依赖前面的参考帧。如果有界网络队列丢掉一个编码包，继续发送后续 P 帧只会
// 让客户端持续花屏。因此检测到 sequence gap 后，丢弃所有非关键帧，直到下一张
// 可独立解码的 IDR 才恢复发送。首次启动同样从 IDR 开始。
class KeyframeResync {
public:
    // 每个编码包按产生顺序调用一次；状态机本身不持有 packet 数据。
    KeyframeResyncDecision observe(uint64_t sequence, bool keyframe) {
        const bool gap = have_previous_ && sequence != previous_sequence_ + 1;
        if (gap) {
            waiting_for_keyframe_ = true;
        }
        previous_sequence_ = sequence;
        have_previous_ = true;

        if (waiting_for_keyframe_ && !keyframe) {
            return {false, gap};
        }
        waiting_for_keyframe_ = false;
        return {true, gap};
    }

    bool waiting_for_keyframe() const { return waiting_for_keyframe_; }

private:
    bool have_previous_ = false;
    bool waiting_for_keyframe_ = true;
    uint64_t previous_sequence_ = 0;
};

}  // namespace rkstream
