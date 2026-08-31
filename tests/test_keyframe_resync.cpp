#include "rkstream/keyframe_resync.h"

#include <iostream>

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
    rkstream::KeyframeResync resync;

    auto decision = resync.observe(10, false);
    CHECK(!decision.accept);
    CHECK(!decision.gap_detected);
    CHECK(resync.waiting_for_keyframe());

    decision = resync.observe(11, true);
    CHECK(decision.accept);
    CHECK(!decision.gap_detected);
    CHECK(!resync.waiting_for_keyframe());

    decision = resync.observe(12, false);
    CHECK(decision.accept);
    CHECK(!decision.gap_detected);

    decision = resync.observe(14, false);
    CHECK(!decision.accept);
    CHECK(decision.gap_detected);
    CHECK(resync.waiting_for_keyframe());

    decision = resync.observe(15, false);
    CHECK(!decision.accept);
    CHECK(!decision.gap_detected);

    decision = resync.observe(16, true);
    CHECK(decision.accept);
    CHECK(!decision.gap_detected);
    CHECK(!resync.waiting_for_keyframe());
    return 0;
}
