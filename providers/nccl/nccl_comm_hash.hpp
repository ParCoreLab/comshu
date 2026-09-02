#pragma once
// NCCL's OWN communicator hash, reimplemented.
//
// WHY THIS EXISTS. `nccl_comm.comm_hash` is FNV-1a over the ncclUniqueId — our
// construction, chosen so the value is equal on every rank of a communicator.
// That is the right key for joining rank to rank, and it is the WRONG key for
// joining to the profiler plugin, because NCCL never sees it.
//
// The profiler plugin's `init` is handed a `uint64_t commId` which NCCL fills
// from `comm->commHash` (reference/nccl-git/src/plugin/profiler.cc:250). For an
// ordinary init that value is
//
//     comm->commHash = getHash(job->commId->internal, NCCL_UNIQUE_ID_BYTES)
//     (reference/nccl-git/src/init.cc:1905)
//
// and `getHash` is a pure function of the bytes
// (reference/nccl-git/src/include/bitops.h:502-543). We already see the
// ncclUniqueId in the ncclCommInitRank interposer, so we compute the same number
// and the profiler rows join to the host-API rows deterministically.
//
// WHY NOT CORRELATE BY TIMING INSTEAD. The tempting alternative is to stash the
// communicator under construction in a thread-local and have the profiler `init`
// callback pick it up, on the theory that the callback fires inside the
// interposed call. That is a hope, not a fact: ncclProfilerPluginInit is called
// from initTransportsRank (reference/nccl-git/src/init.cc:1520), which runs on
// an async job thread when several communicators are initialised inside one
// group. A pure function has no such failure mode.
//
// WHAT IT DOES NOT COVER, and this is the whole reason nccl_comm_hash can be 0.
// Two init paths produce a commHash that is not a function of any ncclUniqueId
// we are shown:
//
//   ncclCommSplit  ->  digestHash over the parent chain   (init.cc:1877-1880)
//   the grow path  ->  hashCombine(baseMagic, nranks)     (init.cc:1900)
//
// For those the caller must record 0, meaning "not derivable". A zero that says
// "unknown" is a fact; a guessed hash is a fabrication, and a reader joining on
// it would silently attribute one communicator's algorithm choice to another.
//
// ENDIANNESS. NCCL memcpy's input bytes into a uint64_t, so the hash is
// byte-order dependent — as is this. Both agree on any one host, which is all
// the join needs; the golden vectors in tests/nccl_comm_hash_test.cpp are
// little-endian and every target we build for is x86_64.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pcomm::providers::nccl {

// Verbatim reimplementation of NCCL's eatHash/digestHash pair. The two-word
// accumulator and the constants are NCCL's; changing any of them changes the
// join key and breaks it silently, so tests/nccl_comm_hash_test.cpp pins the
// output against vectors generated from NCCL's own source.
inline void nccl_eat_hash(std::uint64_t acc[2], const void* bytes, std::size_t size) {
    const char* ptr = static_cast<const char*>(bytes);
    acc[0] ^= size;
    while (size != 0) {
        acc[0] += acc[1];
        acc[1] ^= acc[0];
        acc[0] ^= acc[0] >> 31;
        acc[0] *= 0x9de62bbc8cef3ce3ull;
        acc[1] ^= acc[1] >> 32;
        acc[1] *= 0x485cd6311b599e79ull;
        const std::size_t chunk = size < sizeof(std::uint64_t) ? size : sizeof(std::uint64_t);
        std::uint64_t     x = 0;
        std::memcpy(&x, ptr, chunk);
        ptr += chunk;
        size -= chunk;
        acc[0] += x;
    }
}

inline std::uint64_t nccl_digest_hash(const std::uint64_t acc[2]) {
    std::uint64_t h = acc[0];
    h ^= h >> 31;
    h *= 0xbac3bd562846de6bull;
    h += acc[1];
    h ^= h >> 32;
    h *= 0x995a187a14e7b445ull;
    return h;
}

// The value NCCL will hand the profiler plugin as `commId`, for a communicator
// built from this ncclUniqueId by the ordinary init path.
inline std::uint64_t nccl_comm_hash(const void* unique_id, std::size_t size) {
    std::uint64_t acc[2] = {1, 1};
    nccl_eat_hash(acc, unique_id, size);
    return nccl_digest_hash(acc);
}

}  // namespace pcomm::providers::nccl
