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
//
// The slots of a whole post chain of `n` WQEs, in ONE walk from one (head,
// tail) snapshot: writes out[0..k) and returns k, the number known. Asking
// per WQE (as the post hook once did) re-read the SRQ and re-walked from the
// head each time — quadratic, and each answer from a possibly different
// snapshot (CR-19 audit).
//
// `nwqe` bounds every index read: a link is read out of the SRQ's own ring, and
// one that another thread is rewriting, or that is stale, can name any 16-bit
// value; an index at or past the ring's end ends the walk instead of reading
// past the buffer. 0 means the ring size is not known, and nothing is.
inline unsigned srq_walk_chain(const void* buf, std::uint32_t stride, std::uint32_t head,
                               std::uint32_t tail, std::uint32_t nwqe, unsigned n,
                               std::uint32_t* out) {
    if (!buf || !stride || !out || !nwqe)
        return 0;
    std::uint32_t slot = head;
    for (unsigned k = 0; k < n; ++k) {
        if (slot >= nwqe)
            return k;
        out[k] = slot;
        if (k + 1 == n)
            return n;
        if (slot == tail)
            return k + 1;  // libmlx5 links past the tail only while posting
        std::uint16_t be;
        std::memcpy(&be, static_cast<const std::uint8_t*>(buf) + std::size_t{slot} * stride + 2,
                    sizeof be);
        slot = be16toh(be);
    }
    return n;
}

}  // namespace ibv::cqeprobe
