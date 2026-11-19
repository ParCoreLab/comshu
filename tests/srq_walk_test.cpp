// CR-19: the SRQ free-list walk stops at the tail instead of following a link
// libmlx5 has not written yet.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <endian.h>

#include "srq_walk.hpp"

int main() {
    constexpr std::uint32_t kStride = 64, kWqes = 8;
    std::uint8_t            buf[kStride * kWqes] = {};
    auto link = [&](std::uint32_t from, std::uint32_t to) {
        const std::uint16_t be = htobe16(static_cast<std::uint16_t>(to));
        std::memcpy(buf + from * kStride + 2, &be, sizeof be);
    };
    // Free list 5 -> 1 -> 6 -> 2 (tail). 2's link is STALE: it points at 5,
    // which is exactly what an un-extended tail looks like.
    link(5, 1);
    link(1, 6);
    link(6, 2);
    link(2, 5);
    using ibv::cqeprobe::srq_walk;
    std::uint32_t s = 0;
    assert(srq_walk(buf, kStride, 5, 2, 0, &s) && s == 5);
    assert(srq_walk(buf, kStride, 5, 2, 1, &s) && s == 1 && "not consecutive: follow the list");
    assert(srq_walk(buf, kStride, 5, 2, 3, &s) && s == 2 && "the tail itself is postable");
    assert(!srq_walk(buf, kStride, 5, 2, 4, &s) && "past the tail is unknown, not the stale link");
    assert(!srq_walk(buf, kStride, 2, 2, 1, &s) && "head == tail: only slot 0 is known");
    assert(srq_walk(buf, kStride, 2, 2, 0, &s) && s == 2);
    std::puts("srq_walk: ok");
    return 0;
}
