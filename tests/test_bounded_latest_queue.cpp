#include "rkstream/bounded_latest_queue.h"

#include <iostream>
#include <memory>
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
    bool rejected_zero_capacity = false;
    try {
        rkstream::BoundedLatestQueue<int> invalid(0);
    } catch (const std::invalid_argument&) {
        rejected_zero_capacity = true;
    }
    CHECK(rejected_zero_capacity);

    rkstream::BoundedLatestQueue<int> queue(2);
    CHECK(queue.push(1).accepted);
    CHECK(queue.push(2).accepted);
    const auto replacement = queue.push(3);
    CHECK(replacement.accepted);
    CHECK(replacement.dropped_oldest);
    CHECK(replacement.dropped_value.has_value() && *replacement.dropped_value == 1);
    CHECK(queue.dropped_count() == 1);
    CHECK(queue.high_watermark() == 2);

    const auto first = queue.pop();
    const auto second = queue.pop();
    CHECK(first.has_value() && *first == 2);
    CHECK(second.has_value() && *second == 3);

    queue.close();
    CHECK(!queue.push(4).accepted);
    CHECK(!queue.pop().has_value());

    rkstream::BoundedLatestQueue<std::unique_ptr<int>> resource_queue(2);
    CHECK(resource_queue.push(std::make_unique<int>(10)).accepted);
    CHECK(resource_queue.push(std::make_unique<int>(20)).accepted);
    auto resource_replacement = resource_queue.push(std::make_unique<int>(30));
    CHECK(resource_replacement.dropped_oldest);
    CHECK(resource_replacement.dropped_value.has_value());
    CHECK(**resource_replacement.dropped_value == 10);
    const auto remaining = resource_queue.drain();
    CHECK(remaining.size() == 2);
    CHECK(*remaining[0] == 20);
    CHECK(*remaining[1] == 30);
    CHECK(resource_queue.size() == 0);
    return 0;
}
