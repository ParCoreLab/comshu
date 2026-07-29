/* pcomm engine ABI — the boundary between a provider and libpcomm_engine.so.
 *
 * Deliberately C, not C++. The toolchain split is permanent: NVBit forces a
 * gcc-11.4 host compiler for the gpumem provider while ibverbs needs gcc-13.2
 * for GLIBCXX_3.4.31. Before this header, gpumem's inline `buffer<R>()` called
 * `operator[]` on a `std::map` that the gcc-13.2-built engine had constructed —
 * two compilers mutating the same C++ container. libstdc++'s ABI stability made
 * that work, but it was load-bearing luck, and future plugins (CUPTI ships its
 * own headers; UCX/libfabric come from site modules) make it worse.
 *
 * Only POD crosses here. C++ callers keep the familiar Recorder/Tracer
 * interface — those are now inline wrappers over these calls, so provider
 * sources are unchanged.
 *
 * It is also faster: the old path did a std::map lookup keyed on
 * {recorder, type_id} for *every record*. A provider now caches the arena
 * pointer in a thread_local and appends with a bounds check and a store.
 */
#ifndef PCOMM_ENGINE_ABI_H
#define PCOMM_ENGINE_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on any incompatible change below. Checked at acquire. */
#define PCOMM_ENGINE_ABI_VERSION 2u

/* A per-(thread, record-type) append region. The engine owns the storage and
 * the lifetime; the provider only appends into `slots` and bumps `count`.
 * Layout is frozen — it is read by inline code compiled in other objects. */
typedef struct pcomm_arena {
    void*    slots;    /* capacity * stride bytes of Stamped<R> */
    uint32_t count;    /* filled slots; provider increments */
    uint32_t capacity; /* max slots before a flush is required */
    uint32_t stride;   /* sizeof(Stamped<R>) */
    uint32_t _pad;
    void*    _owner;   /* engine-private; do not touch */
} pcomm_arena;

uint32_t pcomm_engine_abi_version(void);

/* Declare a provider before acquiring. Checks three things nothing checked
 * before:
 *   - the provider was compiled against a compatible PCOMM_ENGINE_ABI_VERSION;
 *   - its record-type id range does not overlap an already-declared provider's
 *     (pcomm-read merges provider streams assuming disjointness, and nothing
 *     enforced it);
 *   - later, that records actually land inside the declared range.
 * Returns 0 on success, non-zero on rejection (and reports why on stderr).
 * Safe to call more than once with identical arguments. */
int pcomm_engine_declare_provider(const char* name, uint16_t id_lo,
                                  uint16_t id_hi, uint32_t abi_version);

/* Is our `dlsym` the one an external caller resolves?
 *
 * 1 = yes; 0 = something else (usually libc) wins, which means preload-based
 * capture is silently degraded — not absent, just incomplete, which is the
 * worst failure mode. This library must be PRELOADED, not merely linked: as a
 * plain DT_NEEDED it loses the lookup and 12 of 16 memory regions and 36 of
 * 432 queue pairs went missing with no error at all. Checked at acquire. */
int pcomm_engine_interposer_live(void);

/* Bring the capture up / take it down. Refcounted: the second provider to
 * acquire joins the running capture rather than re-initialising it, and only
 * the last release finalizes. Returns 0 on success. */
int  pcomm_engine_acquire(const char* output_dir, int rank, int verbose);
void pcomm_engine_release(void);

/* Get this thread's arena for a record type, creating it on first use. The
 * returned pointer is stable for the life of the thread, so callers should
 * cache it in a thread_local. `kind` is core::RecordKind. */
pcomm_arena* pcomm_engine_arena(uint16_t type_id, uint8_t kind,
                                uint32_t stride, uint32_t capacity);

/* Hand the filled region to the sink and reset count to 0. */
void pcomm_engine_arena_flush(pcomm_arena* arena);

/* Drain every arena this thread owns (call before a recording thread exits —
 * the arenas have no TLS destructor, by design: glibc tears down main-thread
 * TLS before atexit handlers, and GPU backends flush from those). */
void pcomm_engine_drain_thread(void);

/* Intern dictionaries. `domain` is rec::InternDomain. Returns a handle; 0 is
 * the null handle. */
uint32_t pcomm_engine_intern_str(uint8_t domain, const char* s, size_t n);
uint32_t pcomm_engine_intern_key(uint8_t domain, uint64_t key);

/* Absolute path of the loaded libpcomm_engine.so, or NULL. The launcher needs
 * this to put the engine in LD_PRELOAD — it must be *preloaded*, not merely
 * linked, or libc wins the `dlsym` lookup and the interposer never runs. */
const char* pcomm_engine_so_path(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PCOMM_ENGINE_ABI_H */
