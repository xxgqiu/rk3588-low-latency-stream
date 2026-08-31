#include "rkstream/dmabuf_queue_policy.h"

int main() {
    static_assert(rkstream::max_dmabuf_lease_queue_capacity(3) == 0);
    static_assert(rkstream::max_dmabuf_lease_queue_capacity(4) == 1);
    static_assert(rkstream::max_dmabuf_lease_queue_capacity(5) == 2);
    static_assert(!rkstream::valid_dmabuf_lease_queue_capacity(4, 0));
    static_assert(rkstream::valid_dmabuf_lease_queue_capacity(4, 1));
    static_assert(!rkstream::valid_dmabuf_lease_queue_capacity(4, 2));
    static_assert(rkstream::valid_dmabuf_lease_queue_capacity(5, 2));
    return 0;
}
