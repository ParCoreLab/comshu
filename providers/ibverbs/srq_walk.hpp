#pragma once
// Where the n-th WQE of a post chain will land in an mlx5 SRQ — the walk
// mlx5_cqe.cpp's srq_slot() does, without mlx5dv, so it can be unit-tested
// (tests/srq_walk_test.cpp).
//
// libmlx5 keeps an SRQ's free WQEs as a linked list from `head` to `tail` and
// takes the head each time (`head = next_wqe_index`). But the list is NOT
// complete ahead of time: when a post finds head == tail, libmlx5 extends it
// right then from its free-WQE bitmap (set_next_tail / set_next_contig_wqes in
// rdma-core providers/mlx5/srq.c). So the tail's `next_wqe_index` is not the
// next slot until that post runs, and walking through it — as this walk once
// did — reads a stale link and names a plausible, wrong slot (CR-19).
//
// So: the WQE posted AT the tail is known (it is the head when head == tail);
// anything after it is not, and is reported unknown rather than guessed.

#include <cstdint>
#include <cstring>
#include <endian.h>

namespace ibv::cqeprobe {

// The 16-bit big-endian link sits at bytes [2, 4) of each WQE
// (struct mlx5_wqe_srq_next_seg { uint16_t rsvd0; __be16 next_wqe_index; ... }).
inline bool srq_walk(const void* buf, std::uint32_t stride, std::uint32_t head,
                     std::uint32_t tail, unsigned n, std::uint32_t* out) {
    if (!buf || !stride || !out)
        return false;
    std::uint32_t slot = head;
    for (unsigned k = 0; k < n; ++k) {
        if (slot == tail)
            return false;  // libmlx5 links past the tail only while posting
        std::uint16_t be;
        std::memcpy(&be, static_cast<const std::uint8_t*>(buf) + std::size_t{slot} * stride + 2,
                    sizeof be);
        slot = be16toh(be);
    }
    *out = slot;
    return true;
}

}  // namespace ibv::cqeprobe
