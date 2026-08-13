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
#define PCOMM_ENGINE_ABI_VERSION 3u

/* A per-(thread, record-type) append region. The engine owns the storage and
 * the lifetime; the provider only appends into `slots` and bumps `count`.
 * Layout is frozen — it is read by inline code compiled in other objects. */
typedef struct pcomm_arena {
    void*    slots;    /* capacity * stride bytes of Stamped<R> */
    uint32_t count;    /* filled slots; provider increments */
    uint32_t capacity; /* max slots before a flush is required */
    uint32_t stride;   /* sizeof(Stamped<R>) */
    uint32_t _pad;
    void*    _owner; /* engine-private; do not touch */
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
int pcomm_engine_declare_provider(const char* name, uint16_t id_lo, uint16_t id_hi,
                                  uint32_t abi_version);

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
pcomm_arena* pcomm_engine_arena(uint16_t type_id, uint8_t kind, uint32_t stride, uint32_t capacity);

/* Hand the filled region to the sink and reset count to 0. */
void pcomm_engine_arena_flush(pcomm_arena* arena);

/* Drain every arena this thread owns (call before a recording thread exits —
 * the arenas have no TLS destructor, by design: glibc tears down main-thread
 * TLS before atexit handlers, and GPU backends flush from those). */
void pcomm_engine_drain_thread(void);

/* Child side of a fork: discard whatever the parent had buffered in this
 * thread's arenas, and re-register them so the child's own records are found at
 * finalize().
 *
 * Both halves matter. ThreadBufferRegistry::reset_for_fork() clears the
 * registry so the parent's buffers are never flushed through COW-shared FILE*s
 * — correct — but the *records* live in the arenas, not in the registry, and an
 * unregistered arena is one that finalize() will never drain. That is how the
 * child came to write 0 records while the parent wrote all of its own.
 *
 * Nothing needs to rebind: Arena::drain() resolves the sink from the Tracer at
 * drain time, so once atfork_child has re-started into the child's directory
 * these arenas already point at the right place. Call this AFTER that start(). */
void pcomm_engine_reset_arenas_for_fork(void);

/* Intern dictionaries. `domain` is rec::InternDomain. Returns a handle; 0 is
 * the null handle. */
uint32_t pcomm_engine_intern_str(uint8_t domain, const char* s, size_t n);
uint32_t pcomm_engine_intern_key(uint8_t domain, uint64_t key);

/* Absolute path of the loaded libpcomm_engine.so, or NULL. The launcher needs
 * this to put the engine in LD_PRELOAD — it must be *preloaded*, not merely
 * linked, or libc wins the `dlsym` lookup and the interposer never runs. */
const char* pcomm_engine_so_path(void);

/* Innermost application memory object containing `addr`, or 0 if none is known.
 * Out params may be NULL. The engine interposes the allocator APIs that hand
 * back an address (nvshmem_malloc and friends) so this answer does not depend
 * on which provider ran — see core/object_registry.hpp. */
int pcomm_engine_object_at(uint64_t addr, uint64_t* base, uint64_t* size, uint32_t* object,
                           uint16_t* device);

/* Tell the engine a provider just recorded a queue-pair OPEN, so the engine's
 * ibv_destroy_qp interposer can close that exact queue pair.
 *
 * The close door has to live in the engine (ibv_destroy_qp tears down both an
 * ibv_create_qp and an mlx5dv_create_qp queue pair, and the two providers
 * coexist by design, so no provider can own the symbol) while the OPEN doors
 * stay in the providers. That split left the engine with no way to know, per
 * queue pair, whether anything recorded its creation — only whether some
 * QP-recording provider was loaded at all, which is a per-run answer to a
 * per-QP question. This call supplies the per-QP one.
 *
 * `qp` is the ibv_qp* the creation door returned. `opener_type_id` is the record
 * type that carries the open — 102 ib_queue_pair or 210 mlx5_queue_pair — and is
 * echoed back on the close, so a close names the door that actually created its
 * subject instead of always naming 102. Call it AFTER recording the open, and
 * only when the open was recorded: an unregistered destroy still emits a close
 * (see engine.cpp), because a queue pair torn down with no open record is the
 * missing-creation-door signal entity_close_accounted exists to surface.
 *
 * Added WITHOUT bumping PCOMM_ENGINE_ABI_VERSION, deliberately: the version is
 * checked for equality at declare, so a bump would reject providers this
 * addition does not actually break. A provider built before this call simply
 * never makes it, and the engine falls back to the coarse rule it used for
 * everything — the old behaviour exactly, not a wrong one. The other direction
 * cannot go quiet: a provider compiled against this header has a DT_NEEDED on
 * the engine and is linked -Wl,--no-undefined, so an older engine fails the load
 * with `undefined symbol: pcomm_engine_note_qp` rather than mis-accounting
 * anything. */
void pcomm_engine_note_qp(void* qp, uint16_t opener_type_id);

/* Record one kernel EXECUTION window. See core/records.hpp GpuKernel (530).
 *
 * The record is the ENGINE's, in its own 500-599 range, but the engine cannot
 * be the one that measures it: the vendor binding needs cupti.h or NVBit, and
 * this translation unit may not pull in a vendor header (see the invariant at
 * the top of engine.cpp). So the measurement arrives here from whichever
 * backend could take it, and the engine owns the identity, the interning and
 * the stream.
 *
 * That indirection is not tidiness, it is forced. Measured 2026-08-13
 * (tests/hpc/probes/kerneltime): NVBit dlopens libcupti and takes the
 * process's single CUPTI subscriber, so a capture has CUPTI *or* gpumem and
 * never both, while "when did kernels run" has to be answerable under either.
 * One record, two instruments, `source` saying which — the same
 * independent-emitters rule MemObject already follows.
 *
 * `t0`/`t1` are raw ticks in `domain`, NOT converted: the caller supplies the
 * domain its clock belongs to (1 for CUPTI's CLOCK_REALTIME ns, 2+gpu for a
 * globaltimer) and must also have emitted ClockAnchors for it, or pcomm-read
 * drops the rows and link_health counts them.
 *
 * `name` is a NUL-terminated demangled symbol or NULL; the engine interns it.
 * `grid` is 3 uint32, `block` 3 uint16; either may be NULL when the source does
 * not report launch geometry. */
void pcomm_engine_record_kernel(uint64_t t0, uint64_t t1, uint16_t domain, const char* name,
                                uint64_t correlation, uint64_t stream, uint16_t device,
                                uint8_t source, const uint32_t* grid, const uint16_t* block);

/* Record a clock anchor for a non-default domain: (local_ticks, wall_ns)
 * sampled at the same moment. Two per domain per run — start and end — is what
 * pcomm-read's per-(rank, domain) fit needs to recover offset AND rate.
 *
 * Domain 0 (host TSC) is anchored by the engine itself. This exists for the
 * domains only a backend can sample: CUPTI's timebase, and a per-GPU
 * globaltimer whose drift was measured at -13.9 ppm on H100 and +5.7 ppm on
 * A100 — opposite signs, so a shared constant is not an option. */
void pcomm_engine_clock_anchor(uint16_t domain, uint64_t local_ticks, uint64_t wall_ns);

/* Should this process take CUPTI?
 *
 * Returns 0 when something else already owns the CUDA callback subscriber — in
 * practice NVBit, loaded by the gpumem provider. Enabling CUPTI anyway does not
 * degrade that capture, it EMPTIES it: measured on MN5 job 44546506 arm E, the
 * real libgpumem_trace.so produced four rank directories and zero .pcs files,
 * silently. The launcher knows the provider list and sets this; a backend must
 * consult it before calling cuptiActivityRegisterCallbacks. */
int pcomm_engine_kernel_timing_wanted(void);

/* How much this run wants recorded: 0 = aggregate, 1 = simple, 2 = full.
 * Matches pcomm::core::TraceMode, which carries the reasoning and the numbers.
 *
 * Read from PCOMM_TRACE_MODE, set by `pcomm run --mode`. A provider should
 * consult it ONCE and cache the answer — it cannot change mid-run, and the
 * call sites that read it are the hottest paths in the process.
 *
 * Defaults to FULL, which is what every capture before this flag existed
 * recorded — so nothing changes for a caller who does not set it, and Simple is
 * reached for on purpose. See core/vocab.hpp for why the default sits on the
 * expensive side: overhead can be paid again, a missing stream cannot.
 *
 * NOT YET RECORDED IN THE CAPTURE, and that gap now matters less than it did
 * while Simple was the default — an unset mode means full, so the streams are
 * there — but it is still open: a capture taken with PCOMM_TRACE_MODE=simple
 * has no ib_poll_cq stream, and a reader cannot tell that from a run that
 * reaped no completions. That is the exact ambiguity link_health exists to
 * remove everywhere else. It wants a field on an existing artifact row, which
 * is a schema change and therefore its own step. */
int pcomm_engine_trace_mode(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PCOMM_ENGINE_ABI_H */
