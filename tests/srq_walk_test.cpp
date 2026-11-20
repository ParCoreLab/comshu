// CR-19: the SRQ free-list walk stops at the tail instead of following a link
// libmlx5 has not written yet, and never reads an index past the ring.
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
    using ibv::cqeprobe::srq_walk_chain;
    std::uint32_t out[8] = {};
    // The whole chain from one walk: 5, 1, 6, 2 — not consecutive.
    assert(srq_walk_chain(buf, kStride, 5, 2, kWqes, 4, out) == 4);
    assert(out[0] == 5 && out[1] == 1 && out[2] == 6 && out[3] == 2 && "follow the list");
    // Past the tail is unknown, not the stale link (2 -> 5).
    assert(srq_walk_chain(buf, kStride, 5, 2, kWqes, 6, out) == 4 && "stop at the tail");
    assert(srq_walk_chain(buf, kStride, 2, 2, kWqes, 2, out) == 1 && out[0] == 2 &&
           "head == tail: only the first WQE is known");

    // A link naming a slot past the ring's end — stale, or being rewritten by
    // another thread — ends the walk instead of reading past the buffer
    // (CR-19 audit). Under ASan the old walk read out of bounds here.
    link(1, 0xfff0);
    assert(srq_walk_chain(buf, kStride, 5, 2, kWqes, 4, out) == 2 && out[1] == 1 &&
           "an out-of-range link is where knowledge ends");
    assert(srq_walk_chain(buf, kStride, 9, 2, kWqes, 1, out) == 0 && "an out-of-range head too");
    assert(srq_walk_chain(buf, kStride, 5, 2, 0, 1, out) == 0 && "no ring size, no slots");
    std::puts("srq_walk: ok");
    return 0;
}
