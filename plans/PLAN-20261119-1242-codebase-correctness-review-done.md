# Codebase Correctness Review Backlog

Status: **DONE** — all 76 tickets closed 2026-11-19; CR-25's window follow-up 2026-11-20.
Created: 2026-11-19 12:42
Baseline: `build-all/core` ctest 38/38 passing at review time.
Scope: Full correctness review across `libcomshu/`, `providers/ibverbs/`, `providers/mlx5dv/`, `providers/ucx/`, `providers/ucx_deep/`, `providers/nccl/`, `providers/gpumem/`, `gpu-backends/`, `comshu/`, `comshu-read/`, `comshu-ui/`, `ui/src/`, and `agent/`.

## Summary

| Severity | Count | Ticket Range |
|---|---:|---|
| Critical | 6 | `CR-01` – `CR-06` |
| High | 31 | `CR-07` – `CR-37` |
| Medium | 28 | `CR-38` – `CR-65` |
| Low | 10 | `CR-66` – `CR-75` |
| **Total** | **75** | |

---

## Critical

### `CR-01` — Multi-statement SQL injection & row-cap bypass in `POST /query`
- [x] **Status:** Done (`query_guard.hpp` checks balanced parens + single statement + trailing `;` stripping; `comshu-ui/main.cpp` verifies `duckdb_extract_statements == 1` and `DUCKDB_STATEMENT_TYPE_SELECT` via DuckDB parser; regression cases in `tests/query_guard_test.cpp`)
- **Subsystem:** `comshu-ui`
- **Locations:** `comshu-ui/main.cpp:245-259`, `comshu-ui/query_guard.hpp:59-65`
- **Problem:** `looks_like_select()` only skips leading whitespace/comments and checks that the first token starts with `SELECT` or `WITH`. `main.cpp:256` wraps `sql` verbatim into `"SELECT * FROM (" + sql + ") LIMIT " + std::to_string(kRowCap)` and passes it to `duckdb_query()`, which executes multi-statement SQL. A payload such as `SELECT 1); DROP TABLE gpu_access; --` executes arbitrary DDL/DML; `SELECT 1) UNION ALL SELECT * FROM huge_table; --` bypasses `kRowCap`.
- **Fix:** Use `duckdb_extract_statements()` to require exactly one statement and verify it is a read-only `SELECT` (or prepare via `duckdb_prepare()` and reject non-`SELECT` statement types) before executing, rather than string-wrapping raw SQL. Add regression cases to `tests/query_guard_test.cpp`.

### `CR-02` — Lazy preload resolution race (`std::abort()`) and `dlsym(RTLD_DEFAULT, ...)` infinite recursion
- [x] **Status:** Done (atomic `RealFunctionStorage`, mutex-guarded idempotent `register_real()`, self-recursion guard in `register_hook()`, regression tests in `tests/hook_smoke.cpp`)
- **Subsystem:** `libcomshu/hook`
- **Locations:** `libcomshu/include/comshu/hook/CompositeHookManager.hpp:208-216`, `libcomshu/include/comshu/hook/detail/PreloadHookManager.hpp:123-131, 235-241`
- **Problem:**
  1. `invoke_preload()` checks `if (!real)` without synchronization and calls `register_real<Tag>()`, which calls `std::abort()` if `*real_fn_ptr` is already non-null. Two threads calling an interposed symbol for the first time concurrently race and abort.
  2. `register_hook()` registers a `smart_dlsym` callback that unconditionally assigns `*real_fn_ptr = reinterpret_cast<Trait::FuncPtr>(real_func)` without checking whether `real_func` is our own `hook_fn`. Any `dlsym(RTLD_DEFAULT, sym)` resolves to our preloaded hook and overwrites `*real_fn_ptr` with itself, causing infinite recursion.
- **Fix:** Store real function pointers atomically (or resolve under `std::call_once` / mutex, making `register_real` idempotent) and guard the `smart_dlsym` callback with `if (real_func && !is_self(real_func))`.

### `CR-03` — Use-After-Free & freed-heap write in `mlx5dv_devx_free_uar` post-hook
- [x] **Status:** Done (moved `hijack::teardown` and `hostdb::disarm` to `register_pre<devx_free_uar_tag>` before `mlx5dv_devx_free_uar` unmaps/frees `uar`)
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/mlx5dv_provider.cpp:746-757`, `providers/mlx5dv/host_doorbell.hpp:395-418`
- **Problem:** `devx_free_uar_tag` is registered with `register_post`, running *after* `mlx5dv_devx_free_uar(uar)` has `munmap`'d the UAR BAR page and `free()`'d `uar`. Consequently: (1) the repeater thread can write to the unmapped BAR page during free, (2) `hijack::teardown(uar->reg_addr)` reads freed memory, and (3) `hostdb::disarm(uar)` executes `uar->reg_addr = it->second.real` into freed heap memory.
- **Fix:** Move `hijack::teardown(uar->reg_addr)` and `hostdb::disarm(uar)` into a `register_pre<devx_free_uar_tag>` hook (before the real free runs), keeping `EntityClose` recording in post or pre as appropriate.

### `CR-04` — Use-After-Free on `ibv_cq*` address reuse in CQE ring cache
- [x] **Status:** Done (the ring cache is `providers/ibverbs/cq_ring_cache.hpp`, keyed by CQ and remembering its context; `register_pre<destroy_cq_tag>` calls `cqeprobe::forget_cq` and `register_pre<close_device_tag>` `cqeprobe::forget_context`, both before the free. `tests/cq_ring_cache_test.cpp` covers reuse of a destroyed CQ's address and per-context eviction)
- **Subsystem:** `providers/ibverbs`
- **Locations:** `providers/ibverbs/mlx5_cqe.cpp:97-100, 136-197, 203-212, 309-322`, `providers/ibverbs/ibverbs_provider.cpp:1059-1065`
- **Problem:** `ibv::cqeprobe::rings()` caches `ibv_cq* -> Ring{buf, cnt, size, dbrec}` permanently and never evicts on `ibv_destroy_cq`. When a new CQ reuses a freed `ibv_cq*` heap address, `cqe_at()` and `on_poll()` dereference the old CQ's unmapped `buf` and freed `dbrec`.
- **Fix:** Evict `cq` from `ibv::cqeprobe::rings()` in the `ibv_destroy_cq` pre-hook (and on `forget_context`).

### `CR-05` — ABI mismatch in `ucp_ep_rkey_unpack_internal` on newer UCX
- [x] **Status:** Done (the signature was checked against every release tag: rkey_p is argument 6 on 1.18, 7 on 1.19–1.22, and 8 from 1.23.0-rc3 on (`uct_flags`, upstream db905d96f). The hook now declares and ALWAYS forwards eight slots; `rkey_out_param_for(major, minor, release)` in `ucx_hooks.hpp` is a static_assert'ed table: 1.23.0 is Unverified because rc1/rc2 still take seven, and anything newer than 1.23 is Unverified and says so once on stderr. `tests/ucx_rkey_abi.sh` runs the provider against stand-in 7- and 8-argument libucps at 1.19/1.22/1.23.2/1.23.0/1.24, and goes red if the eighth argument is not forwarded (mutation-checked))
- **Subsystem:** `providers/ucx`
- **Locations:** `providers/ucx/ucx_hooks.hpp:284-286`, `providers/ucx/ucx_provider.cpp:139-172, 567-580, 835-841`
- **Problem:** The hook declares only 7 parameters (`a1..a7`) and `rkey_out_param()` classifies any UCX `1.x` with `minor >= 19` as `RkeyOutParam::Seventh` with no upper bound. Upstream UCX added `uint64_t uct_flags` as arg 7 and moved `ucp_rkey_h *rkey_p` to arg 8. Against such a build, the 8th argument is not forwarded (corrupting stack/memory in `libucp`), and `a7` (`uct_flags`) is dereferenced as `ucp_rkey_h*`.
- **Fix:** Bound the verified UCX minor version range in `rkey_out_param()` (returning `Unverified` for unverified versions) and/or support the 8-argument signature when matched.

### `CR-06` — Uninitialized `ChannelDev::comshu_dbg` dereferenced by GPU on every `push()`
- [x] **Status:** Done (`nvbit_at_ctx_init` value-initialises the managed ChannelDev with placement new, so `comshu_dbg` starts null, and a failed `cudaMallocManaged` aborts with a message instead of a nameless segfault. No GPU on the local build host: verified by the MN5 gpumem cases)
- **Subsystem:** `gpu-backends/nvbit`
- **Locations:** `gpu-backends/nvbit/nvbit_backend.cu:1292-1306`, `gpu-backends/nvbit/nvbit-1.7.7.1/utils/channel.hpp:52-55, 165-180`
- **Problem:** `state->channel_dev` is allocated with `cudaMallocManaged(&state->channel_dev, sizeof(ChannelDev))` without placement-new or `memset`, and `ChannelDev::init()` does not initialize `comshu_dbg`. When `COMSHU_CHANNEL_DEBUG` is unset and `cudaMallocManaged` returns recycled non-zero memory, every GPU `ChannelDev::push()` executes `if (comshu_dbg) atomicAdd(&comshu_dbg[0], 1ull)` on a garbage address.
- **Fix:** Zero-initialize `state->channel_dev` (or explicitly set `state->channel_dev->comshu_dbg = nullptr;` before the `COMSHU_CHANNEL_DEBUG` check, and initialize it in `ChannelDev::init()`).

---

## High

### Core Engine (`libcomshu/`)

#### `CR-07` — `Tracer::atfork_child()` wipes the child's start-of-run `ClockAnchor` and `NodeIdentity`, and can deadlock on `m_lifecycle`
- [x] **Status:** Done (`atfork_child` clears and re-registers the arenas BEFORE `start()` (they used to be wiped after it, taking the child's start-of-run ClockAnchor and NodeIdentity with them), and placement-resets `m_lifecycle` so a lock held by a parent thread at fork cannot deadlock the child. `fork_smoke` now requires the child's node_identity and both of its clock anchors; it reproduced the loss (child: 1 anchor, 0 node_identity) before the fix)
- **Locations:** `libcomshu/include/comshu/tracer.hpp:78, 112-113, 139, 327-330, 349-373`, `libcomshu/src/engine.cpp:481-494`
- **Problem:** `atfork_child()` calls `reset_for_fork()`, then `t.start(cfg)` (which writes the child's start-of-run `ClockAnchor` and `NodeIdentity` into the cached TLS arenas while they are unregistered), and *then* calls `comshu_engine_reset_arenas_for_fork()`, which sets `a->pub.count = 0` and wipes them. Also, `pthread_atfork` registers no `prepare` handler, so locking `m_lifecycle` in `t.start(cfg)` deadlocks if another thread held it across `fork()`.
- **Fix:** Reset/re-register arenas *before* `t.start(cfg)` (or clear `pub.count = 0` before `start()` and re-register before emitting start-of-run records), and either acquire/release `m_lifecycle` in `prepare`/`parent`/`child` handlers or placement-reset `m_lifecycle` in `atfork_child()`.

#### `CR-08` — Data race between `Recorder::append()` and cross-thread `Arena::drain()`
- [x] **Status:** Done (the owner publishes `count` with a release store after the slot is whole (still a plain uint32_t, so the C ABI is unchanged); every drain holds a per-arena mutex and loads it with acquire; only the owning thread resets the arena, and other drainers advance a `drained` watermark; the signal path uses `try_drain` and never waits. `tests/arena_drain_race.cpp` (4 threads × 300k records under continuous cross-thread drains) is exact with the fix, and against the old code wrote ~1.9M rows for 1.2M records (~60% duplicated, 3–6% lost))
- **Locations:** `libcomshu/src/engine.cpp:118-135`, `libcomshu/include/comshu/rec/recorder.hpp:73-97`, `libcomshu/include/comshu/rec/thread_buffer_registry.hpp:44-67`
- **Problem:** `Arena::pub.count` is a plain non-atomic `uint32_t`. During `Tracer::finalize()` or `flush_from_signal()`, `Arena::drain()` reads `pub.count` twice (`bytes` and `count`) and zeroes it while background threads (e.g. NCCL proxy, CQ poll) may still be appending, risking torn records or mismatched `bytes/count` stride.
- **Fix:** Guard per-arena drain vs append (e.g., atomic sequence/lock or atomic swap/snapshot of `count` with release/acquire ordering after writing the slot).

#### `CR-09` — Uninitialized `m_cpu_domain` and `m_sink` in default-constructed `Recorder`
- [x] **Status:** Done (default member initialisers `m_sink = nullptr`, `m_cpu_domain = kDomainHostTsc`)
- **Locations:** `libcomshu/include/comshu/rec/recorder.hpp:45, 59, 128-129, 141-142`
- **Problem:** `Recorder() = default;` leaves `ISink* m_sink;` and `core::DomainId m_cpu_domain;` uninitialized. Any non-static default-constructed `Recorder<>` reads garbage `m_cpu_domain` in `now()`/`now_instant()`, stamping records with an unanchored domain.
- **Fix:** Add default member initializers `ISink* m_sink = nullptr;` and `core::DomainId m_cpu_domain = core::kDomainHostTsc;` (or `0`).

#### `CR-10` — `dlvsym(RTLD_NEXT, ...)` discards `find_symbol_next()` and calls `real_dlvsym` twice
- [x] **Status:** Done (the RTLD_NEXT branch takes the library `find_symbol_next()` lands in (relative to the real caller) and asks glibc for the requested VERSION in that library, falling back to glibc's own answer. The ticket's suggested fix (return `find_symbol_next`'s result) would have ignored the version. `tests/dlvsym_next/`: an interposer library with its own vsym_probe@V1 must get the next library's @V1 (101) and @@V2 (102); the old code returned the caller's own (mutation-checked))
- **Locations:** `libcomshu/src/hook/Dlsym.cpp:252-270`
- **Problem:** Line 268 discards both `result = find_symbol_next(caller, symbol)` and `glibc_result` and calls `real_dlvsym(handle, symbol, version)` a second time, resolving relative to `libcomshu_engine.so` instead of the caller.
- **Fix:** Return/use the resolved `result` (or `glibc_result` fallback) instead of calling `real_dlvsym` again.

#### `CR-11` — `Dlsym` unsynchronized `RegisteredSymbols` map, double `init()` wiping registrations, and null `symbol_name` UB
- [x] **Status:** Done (the registry is a map behind one mutex that is never held across a callback (a lookup copies the symbol's chain, then runs it unlocked, so registrants may resolve symbols themselves); created on first use, so a registration made before the constructor survives; `init()` runs once via `call_once`; a null `symbol_name` returns the real symbol. `RegisteredSymbols` is no longer exported (`registrant_count()` and `interposer_ready()` replace it). `hook_smoke` adds 8 threads × 200 concurrent registrations and lookups)
- **Locations:** `libcomshu/src/hook/Dlsym.cpp:129-165, 180-200, 224-226, 255-257`, `libcomshu/include/comshu/hook/dlsym/Dlsym.hpp:55-57`
- **Problem:** `RegisteredSymbols` is mutated/read across threads with no mutex; `init()` unconditionally runs `RegisteredSymbols = new ...` (so lazy `dlsym` before `constructor(0)` wipes registrations); `on_symbol_load()` passes `symbol_name` to `find()` without checking for `nullptr`.
- **Fix:** Guard `init()` with a one-time flag (`std::call_once` / atomic CAS), guard `RegisteredSymbols` access with a lock (or RCU/copy-on-write snapshot for signal/re-entrancy safety), and guard `if (!symbol_name) return real_func;`.

#### `CR-12` — Re-calling `CompositeHookManager::install(obj)` on the same object causes infinite recursion
- [x] **Status:** Done (each context keeps one slot shared by all its tags; `install_if_object_ops` leaves an op alone if it is already any of our trampolines, and an object at a recycled address reuses its context's slot with freshly saved originals. The ticket's suggested early return would have left such a recycled object unpatched. `hook_smoke` covers re-install (the old code segfaulted by infinite recursion, mutation-checked) and a recycled object)
- **Locations:** `libcomshu/include/comshu/hook/CompositeHookManager.hpp:269-279, 378-395`, `libcomshu/include/comshu/hook/detail/ObjectOpsHookManager.hpp:144-156`
- **Problem:** On a second `install(obj)`, `get_context(obj)` returns the existing `Context*`, and `install_if_object_ops()` unconditionally executes `ctx->original<Tag>() = op`, overwriting the saved real function with our own trampoline.
- **Fix:** Make `install(obj)` idempotent (return early if `obj` already has an installed context / if `op` is already one of our trampolines).

#### `CR-13` — Fatal signal handler permanently disables itself after any `SIG_IGN` signal + `g_app_action` data race
- [x] **Status:** Done (an ignored signal returns BEFORE the recursion guard is armed, and the check reads `sa_handler == SIG_IGN` whatever `sa_flags` say (the old SA_SIGINFO-first test would have called address 1). `g_app_action` is a per-signal seqlock: lock-free reads in the handler, and writers serialise and block all signals. `crash_identity_smoke` arm 5 (SIGTERM ignored, then SIGSEGV) recovers all 8 records; with the old guard it recovered 0 (mutation-checked))
- **Locations:** `libcomshu/src/engine.cpp:1267-1302, 1350-1398`
- **Problem:** `comshu_fatal_signal_handler()` sets `in_handler = 1` (line 1274) and returns on `SIG_IGN` (lines 1289-1290) without resetting `in_handler = 0`, permanently disabling crash flush for any subsequent fatal signal. `g_app_action[sig]` is read/written in `sigaction()`/`signal()` and the signal handler without synchronization or signal masking.
- **Fix:** Reset `in_handler = 0` before returning on `SIG_IGN` (or check `SIG_IGN` before setting `in_handler = 1` / only set `in_handler` around `flush_from_signal()`), and block the target signal (or use lock-free/atomic publication) when updating `g_app_action[sig]`.

---

### Providers & GPU Backends

#### `CR-14` — Missing dmabuf offset (`in->addr`) in `UmemInfo::gpu_base`
- [x] **Status:** Done (`umem_gpu_base()` (static_assert'ed) gives range base + the umem's offset under dmabuf; `UmemInfo::gpu_base` and the MemObject row share it, so the hijack's copy-out base and `Mlx5DevxCq.cq_base` can no longer disagree with the object. Hardware check: du04 cases 09/12)
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/mlx5dv_provider.cpp:172-173, 371, 487`
- **Problem:** Unlike `MemObject` (line 230), `gpu_base` omits `+ reinterpret_cast<std::uint64_t>(addr)` for `ByDmabuf` umems. When a umem is registered at a non-zero offset within a dmabuf, `hijack::register_qp` copies WQEs from offset 0 of the dmabuf and `Mlx5DevxCq.cq_base` records the wrong GPU VA.
- **Fix:** Add `+ reinterpret_cast<std::uint64_t>(addr)` to `dmabuf_range.base` when computing `gpu_base` for `ByDmabuf`.

#### `CR-15` — Stale `wr::g_ops` and thread-local `wr::slot_for` cache never evicted on QP destruction
- [x] **Status:** Done (providers never see ibv_destroy_qp (the engine owns it), so eviction happens where a recycled QP must pass: `install()` treats an existing entry whose QP now carries fresh provider pointers as stale, replaces it and bumps `g_ops_gen`; a second conversion of a patched QP (wr_start already ours) is left alone. Each thread's slot cache compares the generation (one relaxed load) and refreshes its saved ops in place, which keeps a batch open on another QP. No local test (needs a verbs device); covered by the MN5 IB cases)
- **Subsystem:** `providers/ibverbs`
- **Locations:** `providers/ibverbs/ibverbs_provider.cpp:374-414, 672-749`
- **Problem:** `wr::install()` returns early on map collision (`g_ops.emplace(qpx, ops).second`) and `slot_for()` caches `Slot` in a `thread_local` map/pointer with no eviction on QP destroy. Reusing a freed `ibv_qp_ex*` skips patching the new QP's `wr_*` ops and silently drops sends.
- **Fix:** Evict `qpx` from `wr::g_ops` on `ibv_destroy_qp` and invalidate `thread_local` `slot_for` caches via a generation counter bumped on QP destroy.

#### `CR-16` — Dangling `reg_addr` in `hijack::state().uar_pages` / `uar_ptrs` causes false UAR hijack on VA reuse
- [x] **Status:** Done (`hijack::teardown(reg_addr, /*forget_uar=*/true)` erases `reg_addr` from `uar_pages` and `uar_ptrs` on `mlx5dv_devx_free_uar`. `skipped` is kept so a `cuMemHostUnregister` arriving after the free still returns success without reaching the driver, which never saw the register; erasing it there returned `CUDA_ERROR_HOST_MEMORY_NOT_REGISTERED` to the app. A stale `skipped[p]` is dropped instead in `intercept_host_register`'s pass-through branch, when the VA is registered again as ordinary memory.)
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/uar_hijack.hpp:199-200, 234-246, 843-865, 874-884`
- **Problem:** `teardown()` never erases `reg_addr` from `s.uar_pages` or `s.uar_ptrs`. If the OS maps a regular host buffer at the recycled VA and the app calls `cuMemHostRegister(..., DEVICEMAP)`, `intercept_host_register` falsely hijacks it.
- **Fix:** Erase `reg_addr` from `s.uar_pages` and `s.uar_ptrs` in `teardown()`.

#### `CR-17` — Missing `mlx5dv_devx_obj_destroy` hook corrupts `last_prod` on QP reuse and leaks CUDA resources
- [x] **Status:** Done (`mlx5dv_devx_obj_destroy` is now hooked (pre): it erases `g_obj_ctx`/`g_qp_src_lid` and calls `hijack::forget_qp`, which drops the QP's `qp_info` entries and copy state. `qp_copy` holds shared_ptrs so an in-flight copy-out cannot be freed under it; a `dead` flag stops a waiting one; the stream and pinned buffer are released inside a context push/pop so the application's current context is untouched. Nothing is recorded, so entity accounting is unchanged)
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/mlx5dv_provider.cpp:120-121, 273, 759-783`, `providers/mlx5dv/uar_hijack.hpp:211-212, 255-268, 761-786`
- **Problem:** `mlx5dv_devx_obj_destroy` is unhooked, so `g_obj_ctx`, `g_qp_src_lid`, `s.qp_info`, and `s.qp_copy` are never cleaned up. Reused `mlx5dv_devx_obj*` pointers inherit stale `have_last = true` and `last_prod != 0` (skipping WQEs or decoding uninitialized slots), and each destroyed QP leaks its `CUstream` and pinned `host_buf`.
- **Fix:** Hook `mlx5dv_devx_obj_destroy` to erase from `g_obj_ctx`, `g_qp_src_lid`, `s.qp_info`, and `s.qp_copy` (destroying `CUstream` and freeing `host_buf`).

#### `CR-18` — TOCTOU race in `hostdb::arm()` corrupts refcount, leaks decoy `mmap`, and can chain repeaters
- [x] **Status:** Done (`arm()` re-checks `s.armed` after its unlocked setup: whoever arrives second takes a reference and discards its own repeater (joined) and decoy (munmap))
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/host_doorbell.hpp:341-383`
- **Problem:** `arm()` checks `s.armed.find(uar)` under `s.mutex`, releases the lock to `mmap` a decoy page and start a `RepeaterHandle`, then re-locks and unconditionally overwrites `s.armed[uar]` with `refs = 1`. Two threads concurrently arming a singleton NC UAR leak the first `mmap`, lose a refcount, and can chain a repeater onto the first decoy page.
- **Fix:** Re-check `s.armed.find(uar)` after re-acquiring `s.mutex` (if already present, increment `refs` and tear down the redundant decoy/repeater), or hold `s.mutex` / reserve a pending slot across setup.

#### `CR-19` — Broken and unsynchronized SRQ free-list walk in `srq_slot()`
- [x] **Status:** Done (the walk is `providers/ibverbs/srq_walk.hpp` and stops at `tail`: libmlx5 links past the tail only while posting (set_next_tail/set_next_contig_wqes, checked in rdma-core srq.c), so later WRs in the chain get slot-unknown instead of a stale link's guess. `tests/srq_walk_test.cpp`. The race with another thread posting to the same SRQ is not closable from outside libmlx5 (its lock is private); the CQE wqe_counter cross-check is what keeps it from becoming a wrong attribution)
- **Subsystem:** `providers/ibverbs`
- **Locations:** `providers/ibverbs/mlx5_cqe.cpp:238-289`, `providers/ibverbs/ibverbs_provider.cpp:1404-1414`
- **Problem:** `srq_slot(srq, n)` walks `next_wqe_index` `n` times from `out.head` without holding `srq->lock` or checking `slot == out.tail` / bounds. In `rdma-core`, `mlx5_post_srq_recv` lazily links freed WQEs from `free_wqe_bitmap` only when `head == tail`; walking past `tail` reads stale indices, and concurrent post/poll races with the unlocked walk.
- **Fix:** Check `slot == out.tail` and bounds during the walk, and either synchronize with SRQ posting or record slot mapping safely on post/completion.

#### `CR-20` — Unsynchronized data race / heap-UAF on `G().id_to_memop` in NVBit
- [x] **Status:** Done (`Globals::id_to_memop` is a fixed table of `std::atomic<MemOp>` (16Ki entries) that never moves: written once per new opcode with release, read per packet with acquire; an id past the table reads as Load instead of out of range)
- **Subsystem:** `gpu-backends/nvbit`
- **Locations:** `gpu-backends/nvbit/nvbit_backend.cu:630, 1045`
- **Problem:** `instrument_function_if_needed()` appends to `G().id_to_memop` under `g_mutex`, while `recv_thread_fun()` reads `G().id_to_memop[ma->opcode_id]` unlocked. Reallocation during concurrent instrumentation causes heap-UAF.
- **Fix:** Protect `id_to_memop` access (e.g., fixed-size chunks / `std::deque` with atomic size publication, or a lock/shared_mutex).

#### `CR-21` — Unsynchronized data race & null-deref hazard on `g_launch_ticks` / `g_launch_owner` and `g_launch_unknown`
- [x] **Status:** Done (the launch ring is static zero-initialised atomics (no allocate-then-publish window), and each slot is a seqlock: the writer retires the owner, writes ticks, publishes owner = id + 1; the reader accepts the time only if the owner matched before and after. `g_launch_unknown` is atomic)
- **Subsystem:** `gpu-backends/nvbit`
- **Locations:** `gpu-backends/nvbit/nvbit_backend.cu:237-262, 872, 1032-1035`
- **Problem:** `g_launch_ticks` is allocated and published before `g_launch_owner`, while `launch_time()` only checks `if (!g_launch_ticks)`. `g_launch_owner[slot] = id` is written before `g_launch_ticks[slot] = now` with non-atomic stores, so readers can observe `id` with stale/zero ticks. `++g_launch_unknown` is non-atomic across per-context recv threads.
- **Fix:** Allocate both arrays before publishing (or use static/fixed arrays), store `ticks` before `owner` with `release`/`acquire` atomics (or a seqlock/mutex), and make `g_launch_unknown` `std::atomic<uint64_t>`.

#### `CR-22` — Static destruction order fiasco (use-after-destruction) on `g_launch_meta` and `g_launch_span`
- [x] **Status:** Done (`launch_meta()`/`launch_span()` are leaked function-local maps, like `G()`, so ctx_term and emit_kernel_spans after static destruction no longer walk destroyed maps)
- **Subsystem:** `gpu-backends/nvbit`
- **Locations:** `gpu-backends/nvbit/nvbit_backend.cu:208-209, 1015, 1123, 1132, 1388`
- **Problem:** `g_launch_meta` and `g_launch_span` are namespace-scope `std::unordered_map` objects with non-trivial destructors, accessed by `recv_thread_fun` and `emit_kernel_spans()` inside `nvbit_at_ctx_term`, which can run after C++ static destructors.
- **Fix:** Move them into the immortal `G()` singleton (constructed via leaky `new`), matching the rest of `nvbit_backend.cu`.

#### `CR-23` — Data race / wrong-parent attribution in `EventPool::resolve()` and free-list corruption on double `release()`
- [x] **Status:** Done (`Event`/`EventPool` moved to `providers/nccl/nccl_event_pool.hpp`. `generation`, `id` and a new `released` flag are atomic. Parent lookup (`id_of`) re-checks the generation after reading `id`, so a racing re-issue reports 0, never the new occupant. `release()` is idempotent via `released`, and stopEvent uses `resolve_live`, so a double stop neither emits twice nor links the slot into the free list twice. The generation is NOT bumped on release, which the ticket proposed: that would end the quarantine that keeps a stopped collective nameable for its late proxy ops. `tests/nccl_event_pool_test.cpp` drains all 2M slots and requires them distinct; with the guard removed it fails (mutation-checked))
- **Subsystem:** `providers/nccl`
- **Locations:** `providers/nccl/nccl_profiler_plugin.cpp:113, 116, 207-256, 319-322, 409`
- **Problem:** `Event::generation` and `Event::id` are plain non-atomic fields written in `acquire()`/`prof_start_event()` and read unlocked in `resolve()`/`parent_id_of()`, allowing data races and mis-attribution on slot reuse. `release()` does not bump `generation` or mark the slot released, so duplicate `release()` appends the same slot to the free list twice (creating a cycle).
- **Fix:** Make `generation` atomic (or hold `mu_` in `resolve()`/`parent_id_of()`), bump `generation` (e.g. odd/even live/free bit) inside `release()` under `mu_` to make double-release a no-op, and write `ev->id` before exposing the handle.

#### `CR-24` — Unsynchronized data race / iterator invalidation on `ucx_code_ranges_storage()`
- [x] **Status:** Done (the code-range vector has its own mutex; refresh reads /proc/self/maps into a local vector and swaps it in under the lock, and `is_ucx_code` iterates under it)
- **Subsystem:** `providers/ucx_deep`
- **Locations:** `providers/ucx_deep/ucx_deep_provider.cpp:195-242, 701-707`
- **Problem:** `on_iface_open()` calls `ops_layout_plausible()` (`is_ucx_code()`) and `refresh_ucx_code_ranges()` (`clear()`/`push_back()`) *before* acquiring `table_mu()`. Concurrent `uct_iface_open()` calls race on the vector.
- **Fix:** Protect `ucx_code_ranges_storage()` with a mutex (or acquire `table_mu()` before calling `ops_layout_plausible()`/`refresh_ucx_code_ranges()`).

#### `CR-25` — NCCL hooks ignore `ncclInProgress`, dropping non-blocking events and permanently skewing `group_depth()`
- [x] **Status:** Done (`issued()` (ncclSuccess or, from 2.14, ncclInProgress) gates collectives, P2P and ncclGroupStart/End, so a non-blocking group closes its depth. A non-blocking INIT returning ncclInProgress is NOT recorded then: its rank and size are still being filled in. It is held pending and recorded, with the init call's timing, when a later call on it finds `ncclCommGetAsyncError` == ncclSuccess; destroy/abort of a comm that never became ready records no close. Window registration stays success-only, since a window that is not ready cannot be asked for peer pointers; deregister already closes only windows seen open. Exercised on hardware by case 29)
- **Subsystem:** `providers/nccl`
- **Locations:** `providers/nccl/nccl_provider.cpp:158-265, 301, 334, 415, 424`
- **Problem:** Non-blocking NCCL communicators return `ncclInProgress` on success for init, collectives, P2P, and `ncclGroupEnd`. Every hook checks `res == ncclSuccess` exclusively, dropping those records and failing to decrement `group_depth()` on `ncclGroupEnd`.
- **Fix:** Treat `res == ncclSuccess || res == ncclInProgress` as success across NCCL hooks.
- [x] **Window follow-up — Done:** registrations that have not produced their window are held (`PendingWindow`) with the call's timing and the caller's handle slot. Inside a caller's group they wait on this thread's list for the outermost `ncclGroupEnd` — which the note below missed: `ncclCommWindowRegister` sets `*win = nullptr` and NCCL writes it when the group runs, so an in-group window was never recorded even on a BLOCKING communicator. In progress, they wait on a shared list for the first ready observation: a new `ncclCommGetAsyncError` hook (the owner's poll), or any later hooked call on the communicator; destroy/abort drops them unread. The pending-init path settles at the same poll. Case 29 (`29_nccl_lsa_nonblocking`: case 28's ring on a `blocking = 0` communicator, window registered inside a group) asserts the comm, window and peer rows and the same exact placement as case 28. du04 (NCCL 2.32.6): cases 28 and 29 PASS; against the old provider case 29 fails with 0 nccl_window and 0 nccl_window_peer rows on all four ranks and every store unplaced (mutation-checked). The communicator itself was already recorded there, via the pending-init path. The hook must be in `nccl.map` (the pre-commit check caught it): without the export the first green run recorded the windows only at ncclCommWindowDeregister, after every store. Exported, rank 0's nccl_window_peer rows land 0.23 G ticks after the group end and 3.6 G ticks before the window closes, i.e. at the poll.
- **Not enough for the window hooks** (`providers/nccl/nccl_provider.cpp`, both `window_register_tag` post-callbacks). On a non-blocking communicator `ncclCommWindowRegister` returning `ncclInProgress` means the window is NOT ready: `*win` and the peer pointers `ncclGetPeerDevicePointer` returns (the `nccl_window_peer` rows, 0c73efb) are only valid once `ncclCommGetAsyncError` reports `ncclSuccess`. Record the window and its peer mappings at that point (deferred until the communicator completes), not at return. Until then such windows' stores are counted in `link_health.unattributed_p2p_accesses`, never misplaced.

#### `CR-26` — Wrong `a.thread` attribution for divergent warps in NVBit drain
- [x] **Status:** Done (host-side: `access_thread()` (static_assert'ed) uses the warp base thread_index - lane_id, since the packet's thread_index is the pushing (first active) lane's. Device code unchanged)
- **Subsystem:** `gpu-backends/nvbit`
- **Locations:** `gpu-backends/nvbit/inject_funcs.cu:20-21, 35, 41-46`, `gpu-backends/nvbit/nvbit_backend.cu:1037-1044`
- **Problem:** `ma.thread_index` is the flat thread ID of `first_laneid` (`ma.lane_id`), not lane 0 of the warp. `nvbit_backend.cu:1044` computes `a.thread = static_cast<uint32_t>(ma->thread_index) + lane;`, double-counting `ma->lane_id` whenever lane 0 is inactive.
- **Fix:** Compute `(static_cast<uint32_t>(ma->thread_index) - ma->lane_id) + lane` (or store warp base `thread_index - laneid` in `inject_funcs.cu`).

---

### CLI, Reader, SQL & UI Backend

#### `CR-27` — Unsynchronized concurrent access to a single `duckdb_connection` across HTTP worker threads
- [x] **Status:** Done (`comshu-ui/main.cpp` opens a per-request `duckdb_connection` via `duckdb_connect(db, &req_con)` with RAII disconnect)
- **Subsystem:** `comshu-ui`
- **Locations:** `comshu-ui/main.cpp:111, 243-299`
- **Problem:** `httplib::Server` serves requests concurrently on a thread pool, and `POST /query` calls `duckdb_query(con, ...)` on a single shared `duckdb_connection con` with no mutex or per-thread connection. Parallel browser panel loads race inside DuckDB.
- **Fix:** Either open a per-request / `thread_local` `duckdb_connection` via `duckdb_connect(db, ...)` or serialize `duckdb_query` with a mutex.

#### `CR-28` — `mount_raw_tables()` CSV loading path is incompatible with `Raw::ddl`
- [x] **Status:** Done (the CSV branch IS load-bearing: the rule tests' fixtures are raw-layout CSV and link through it. What broke was a DECODED `export --csv` (ticks, names, hex) fed into the raw DDL. `mount_raw_tables` now loads a CSV only if its header is the raw layout (same width, opening with rank + the six temporal_* columns; payload names are not compared, because COPY is positional) and reports any other by name; `comshu link` and `comshu ui` refuse a decoded export with directions to use the capture or `--parquet`. `comshu sql` keeps querying CSV exports through read_csv. A stale fixture (`nccl_execution_rule` clock_anchor names) was corrected. Test: `tests/csv_export_contract.sh`)
- **Subsystem:** `comshu-read`
- **Locations:** `comshu-read/duck_mount.hpp:157-178`
- **Problem:** `mount_raw_tables()` creates tables using `Raw::ddl` (raw C++ struct layout with `temporal_*` columns, padding, numeric enums, `UBIGINT`s) and executes `COPY "<name>" FROM '<csv>'`, but `comshu export --csv` emits different column counts/names and decoded/hex text, causing every CSV `COPY` to fail.
- **Fix:** Either load CSV exports via `read_csv_auto` views (matching `run_sql()`) or align the CSV export format/loader contract and add a test covering `comshu link` / `comshu ui` on `--csv` exports.

#### `CR-29` — Unquoted CSV strings in `Table<GpuKernel>` (and other tables) corrupt `--csv` exports
- [x] **Status:** Done (all six free-text columns (mem_object.name, source_line.file, gpu_device.name, gpu_kernel.name, ib_device.fw_ver, uct_iface tl/dev_name) go through `csv_quoted`. `tests/csv_export_contract.sh` exports a capture with `void k<int, float>(int, float)` and a quoted object name and checks every row's width and the round trip. `ucx_deep_records.sh`'s awk parser now strips quotes. The suite's SQL asserts read through DuckDB and are unaffected; case 27's grep reads an unquoted enum column)
- **Subsystem:** `comshu-read`
- **Locations:** `comshu-read/main.cpp:424-442` (also `300-305`, `320-322`, `355-361`, `802-809`, `1432-1438`)
- **Problem:** `Table<GpuKernel>::row` (and `MemObject`, `SourceLine`, `GpuDevice`, `IbDevice`, `UctIface`) formats string columns with bare `%s` instead of `csv_quoted()`. Demangled C++ kernel names routinely contain commas, splitting rows into extra columns.
- **Fix:** Use `csv_quoted(...)` for all string columns in CSV emitters.

#### `CR-30` — Cartesian fan-out in `link_health.capture_streams_missing` produces false-positive defects on replayed captures
- [x] **Status:** Done (`capture_streams_missing` compares per-rank SUMS (opened across passes vs found across directories) instead of joining rows; neither table carries a pass key, so a per-pass join was not available without a schema change. `capture_integrity_rule.sh` adds two replayed ranks: two whole passes count 0 (the old join reported 2), and a pass that lost one file counts 1)
- **Subsystem:** `comshu-read`
- **Locations:** `comshu-read/link_sql.hpp:3457-3463`, `comshu-read/capture_load.hpp:268-306`
- **Problem:** Replayed captures produce multiple rows per `rank` in both `capture_status` and `capture_integrity`. Joining them directly on `i.rank = s.rank` creates an N×N Cartesian product per rank and falsely fires `capture_streams_missing` whenever two passes open different numbers of streams.
- **Fix:** Aggregate per rank (`SUM(streams_found)` vs `SUM(streams_open)`) or carry a pass/subdir key on both tables and join on `(rank, pass)`.

#### `CR-31` — In-place `comshu bake -o <host> <host>` destroys the host binary; 32-bit overflow in TOC bounds check
- [x] **Status:** Done (bake writes to a mkstemp temporary beside the output and `rename()`s it into place (the mode is read before the rename, since in-place the host is the output); every failure path unlinks the temporary. The TOC bound is `toc_len > size || toc_off > size - toc_len` and the name bound `left < 32 || left - 32 < nlen`. `tests/bake_rule.sh`: an in-place bake gives a working launcher that lists its payload, and crafted footers for both wraps are refused; with the old 32-bit check the crafted name length segfaulted --list (mutation-checked))
- **Subsystem:** `comshu`
- **Locations:** `comshu/baked.cpp:103, 118-123, 448-485`
- **Problem:**
  1. `bake_main` opens `host` read-only and then `fopen(out_path, "wb")` before copying, truncating `host` to 0 bytes if `out_path` refers to the same file.
  2. Line 120 computes `nlen + 32u` in 32-bit `unsigned` arithmetic, which wraps when `nlen >= 0xFFFFFFE0u` and bypasses the bounds check before `e.name.assign(..., nlen)`. Line 103's `toc_off + toc_len` can similarly overflow 64-bit unsigned addition.
- **Fix:** Write to a temporary file in the destination directory and `rename()` atomically; use `size_t` / overflow-safe subtraction (`end - p < 32 || static_cast<size_t>(end - p) - 32 < nlen`) for TOC/footer bounds checks.

---

### UI & Agent

#### `CR-32` — Middle-stage Sankey node totals are double-counted, halving all stage-1 ribbon heights
- [x] **Status:** Done (a node's value is max(in, out) flow, not in + out; ribbons scale by it, so a conserving middle node is filled on both sides. `ui/test/sankey.test.mjs`)
- **Subsystem:** `ui`
- **Locations:** `ui/src/lib/sankey.js:19-21, 58-59`
- **Problem:** Every link adds `l.value` to both `l.from` and `l.to`, giving stage-1 nodes `value = 2 × flow`. Ribbon heights divide by `a.value` / `b.value`, drawing every stage-1 ribbon at 50% height and leaving the bottom half of every middle-stage node blank.
- **Fix:** Track `inTotal` and `outTotal` separately per node, set `node.value = Math.max(inTotal, outTotal)`, and divide `ah` by `outTotal` and `bh` by `inTotal`.

#### `CR-33` — Boolean strings from `/query` break approximate-offset indicators (`"false"` is truthy)
- [x] **Status:** Done (`bool()` in `ui/src/lib/query.js`, beside `num()`, reads `/query`'s "true"/"false" strings (NULL is not true); Objects uses it at both sites, which are the only BOOLEAN columns the panels query. `ui/test/query.test.mjs`)
- **Subsystem:** `ui`
- **Locations:** `ui/src/panels/Objects.svelte:131, 147, 232, 307`
- **Problem:** `/query` returns DuckDB booleans as strings (`"true"` / `"false"`). `b.all_exact ? '' : ' (approx.)'` and `{#if !sp.exact}` treat `"false"` as truthy, so approximate-offset indicators never render.
- **Fix:** Compare explicitly against `'true'` / `'false'` (e.g., `b.all_exact === 'true'`).

#### `CR-34` — `PiSession.ask()` never settles on child exit/error, causing hangs and `busy` corruption on `/reset`
- [x] **Status:** Done (`PiSession`: `exited` settles on 'exit' OR 'error' (a failed spawn emits only 'error', and without a listener it crashed the bridge); every `ask()` races it and rejects at once with the reason; stdin EPIPE is swallowed; the idle timer is unref'd. Bridge: `busy` is released only by the /ask that owns the current generation, and /reset bumps it. `agent/test/session.test.mjs`: pi missing, and pi dying mid-question, both reject in milliseconds; without the race both time out (mutation-checked))
- **Subsystem:** `agent`
- **Locations:** `agent/lib/session.mjs:67-81, 118-194`, `agent/bridge.mjs:189-195, 222-246`
- **Problem:** `PiSession.start()` attaches no `'error'` listener to `this.child` (unhandled error crashes bridge on `ENOENT`). `ask()` does not reject/settle on `this.exited`, hanging for 10 min on child crash or `/reset`. In `bridge.mjs`, `/reset` clears `busy = false` while an earlier `/ask` is still pending, so its `finally` later clobbers `busy` for a newer `/ask`.
- **Fix:** Attach an `'error'` handler on `spawn()`, race/reject in-flight `ask()` when `child` exits or errors, and guard `busy` with a request token/generation in `bridge.mjs`.

#### `CR-35` — `req.on('close')` attached after `readBody(req)` never detects client disconnect
- [x] **Status:** Done (the abort listener is on `res` ('close' with `!res.writableEnded`), since `readBody` had already consumed `req`)
- **Subsystem:** `agent`
- **Locations:** `agent/bridge.mjs:204, 224-227`
- **Problem:** `await readBody(req)` consumes `req` to completion, emitting `'end'`/`'close'` before `req.on('close', ...)` is registered on line 224. Aborting the SSE fetch or closing the browser tab never aborts the agent.
- **Fix:** Listen on `res.on('close', () => { if (!res.writableEnded) { closed = true; session?.abort(); } })`.

#### `CR-36` — Receive-only ranks silently dropped from Per-rank traffic (`Ranks.svelte`)
- [x] **Status:** Done (FULL JOIN with coalesce, and the join now includes `unit` (`out` is grouped by it))
- **Subsystem:** `ui`
- **Locations:** `ui/src/panels/Ranks.svelte:16-31`
- **Problem:** `FROM out o LEFT JOIN inn i ...` omits ranks with zero outgoing traffic (e.g., gather/reduce sinks).
- **Fix:** Use `FULL OUTER JOIN` with `coalesce(...)` for `rank`, `transport`, `unit`, `sent`, `ops`, `peers`, and `recv`.

#### `CR-37` — Multi-transport `comm_edges` rows overwrite each other in `Dendrogram.svelte`
- [x] **Status:** Done (one row per pair: `max(bytes)` across transports. The panel asks whether two ranks talk, and summing across transports would add NVLink stores to InfiniBand bytes (the Ranks panel's rule). On llm-scaleout, 304 edge rows collapse to 256 pairs, so 48 cells had been overwritten)
- **Subsystem:** `ui`
- **Locations:** `ui/src/panels/Dendrogram.svelte:10-23`
- **Problem:** Queries `comm_edges` across all transports without filtering or summing, so `vecs[...][...] = Math.log1p(num(r.bytes))` overwrites earlier transports in arbitrary row order.
- **Fix:** Either aggregate (`SUM(bytes) GROUP BY src_rank, dst_rank`) or add a transport selector consistent with `Matrix`/`Graph`.

---

## Medium

### Core Engine (`libcomshu/`)

#### `CR-38` — Out-of-bounds `domain` indexing in `comshu_engine_intern_str()`
- [x] **Status:** Done (the same domain bound as `intern_key`, plus a null string with a non-zero length returns 0)
- **Locations:** `libcomshu/src/engine.cpp:550-558`
- **Problem:** Missing `if (domain >= comshu::rec::kInternDomainCount) return 0;` check (present in `comshu_engine_intern_key()` at line 572).
- **Fix:** Add the same bounds check at the top of `comshu_engine_intern_str()`.

#### `CR-39` — Integer overflow in bounds check & unchecked `in.read()` in `InternDictionary::load()`
- [x] **Status:** Done (the bound is `off > size || len > size - off` (cannot wrap), and a blob shorter than its header says is diagnosed and truncated to what was read. `tests/intern_damage_test.cpp` damages one index entry so `off + len` wraps: the old check let `substr()` throw out of the reader (mutation-checked), and now that one name is refused and the rest resolve)
- **Locations:** `libcomshu/include/comshu/rec/intern_reader.hpp:126-140`
- **Problem:** `off + len > blob.size()` can overflow `uint64_t` (throwing uncaught `std::out_of_range` in `blob.substr`), and `in.read()` does not check `gcount()` or stream failure.
- **Fix:** Check `off > blob.size() || len > blob.size() - off` and verify `in.gcount() == expected` after `in.read()`.

#### `CR-40` — Unchecked `in.read()` in `load_stream()` silently returns zero-filled records on short read
- [x] **Status:** Done (after the read, `out` is cut to the whole records `gcount()` delivered, and a short read is diagnosed and marks the stream truncated. Not unit-tested: the file has to shrink between the size check and the read)
- **Locations:** `libcomshu/include/comshu/rec/stream_reader.hpp:139-144`
- **Problem:** Does not check `in.gcount()` or `!in`; a short read returns zero-initialized phantom records with `truncated == false`.
- **Fix:** Check `in.gcount()`, resize `out` to whole records actually read, and mark `found->truncated = true` on short read.

#### `CR-41` — Throwing `std::filesystem` calls and unsigned underflow in `unhandled_streams()`
- [x] **Status:** Done (`is_regular_file(ec)` and `file_size(ec)`, and the payload is 0 when the file is shorter than a header)
- **Locations:** `libcomshu/include/comshu/rec/version_map.hpp:94, 107`
- **Problem:** `entry.is_regular_file()` and `entry.file_size()` use throwing overloads; `file_size - sizeof(StreamHeader)` underflows if the file shrank below 32 bytes.
- **Fix:** Use `ec` overloads and guard `sz >= sizeof(StreamHeader)`.

#### `CR-42` — `mprotect(..., PROT_READ)` on partial tail page of `PT_GNU_RELRO` can write-protect `.data`/`.bss`
- [x] **Status:** Done (`in_relro` uses `relro.hi & pmask`, which is glibc's own `_dl_protect_relro` end, so a cell on the partial tail page (writable, shared with .data/.bss) is patched without mprotect and that page is never re-protected read-only)
- **Locations:** `libcomshu/src/libcuda_cells.cpp:185-190`
- **Problem:** Tests raw `m.relro.hi` instead of `ALIGN_DOWN(m.relro.hi, pagesz)`. Re-protecting a partial tail page shared with writable `.data`/`.bss` causes SIGSEGV.
- **Fix:** Align `relro.hi` down to `pagesz` (or restore the original page protection parsed from `/proc/self/maps`).

#### `CR-43` — Unsynchronized function pointers & missing `is_driver_fn` check in `dmabuf_registry.hpp`
- [x] **Status:** Done (the three cached pointers are `std::atomic` (acquire/release), `install()`'s flag is atomic, and the dlsym door for `cuMemGetHandleForAddressRange` adopts `next` only if `is_driver_fn(next)`, otherwise declining so the chain is unchanged: the same positive test the getproc doors use)
- **Locations:** `libcomshu/include/comshu/core/dmabuf_registry.hpp:107-114, 184, 223-226, 241-264`
- **Problem:** Function pointers are plain non-atomic globals read/written across threads; `install()`'s `cuMemGetHandleForAddressRange` callback does not check `is_driver_fn(next)` before storing `real_get_handle()`.
- **Fix:** Use `std::atomic` for cached function pointers and add the `is_driver_fn(next)` check.

#### `CR-44` — Data race on `real_destroy_qp()`
- [x] **Status:** Done (`real_destroy_qp()` is `std::atomic<PfnDestroyQp>`; the lazy fallback stores only a non-null resolution)
- **Locations:** `libcomshu/src/engine.cpp:1459-1490, 1513-1518`
- **Problem:** Plain non-atomic function pointer read/written concurrently by `ibv_destroy_qp_hook()` and `smart_dlsym` callback.
- **Fix:** Store in `std::atomic<DestroyQpFn>`.

#### `CR-45` — `Tracer::emit_intern_records()` iterates `InternTable` without locking `m_recorder->mutex(domain)`
- [x] **Status:** Done (`emit_intern_records(from_signal)` holds each domain's intern mutex while walking it during finalize (the one-time emitter is already stopped there, so nothing can block on it); the signal path only try-locks and walks unlocked when it cannot have the lock, as before, because a dying process must not wait on a lock the faulting thread may hold)
- **Locations:** `libcomshu/include/comshu/tracer.hpp:633-668`
- **Problem:** Iterates `InternTable` containers without acquiring `m_recorder->mutex(domain)`, racing with concurrent `intern()` calls from background threads.
- **Fix:** Acquire `m_recorder->mutex(domain)` during normal `finalize()` (and keep signal path best-effort / lock-free).

---

### Providers & GPU Backends

#### `CR-46` — `decode_create_qp` omits `rq_bytes` offset for GPU-driven QPs
- [x] **Status:** Done (`rq_bytes` is computed once and added to the SQ base given to `hijack::register_qp` as well as sqwatch's. NVSHMEM's IBGDA QPs use an SRQ (`IBGDA_SRQ_TYPE_VALUE`, checked in nvshmem-git ibgda.cpp), so they are unaffected; the fix applies to GPU-driven QPs with a regular RQ)
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/mlx5dv_provider.cpp:370-373` (cf. `389-394`)
- **Problem:** Unlike `sqwatch` (lines 389-394), `hijack::register_qp` passes `wq.gpu_base + wq_offset` without adding `rq_bytes` when `rq_type == 0`, reading WQEs from the RQ instead of the SQ.
- **Fix:** Add `rq_bytes` to the SQ base passed to `hijack::register_qp` when `rq_type == 0`.

#### `CR-47` — Data race on `std::thread` objects between `sqwatch::shutdown()` and `sqwatch::watch()`
- [x] **Status:** Done (`watch()` returns without adding or starting anything once `stop` is set (checked under `s.mutex`); `shutdown()` raises `stop` and moves both thread objects out under the mutex, then joins them outside it)
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/sq_watch.hpp:744-761, 816-833`
- **Problem:** `watch()` spawns threads under `s.mutex` without checking `s.stop`, while `shutdown()` calls `joinable()`/`join()` without holding `s.mutex`.
- **Fix:** Check `s.stop` inside `watch()` under `s.mutex`, and move threads out of `s` under `s.mutex` in `shutdown()` before joining them outside the lock.

#### `CR-48` — `mlx5dv_create_qp` post-hook reads `attr->pd` without checking `IBV_QP_INIT_ATTR_PD`
- [x] **Status:** Done (PD from `attr` only when `comp_mask & IBV_QP_INIT_ATTR_PD`, else `qp->pd`)
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/mlx5dv_provider.cpp:793-804`
- **Problem:** Reads `attr->pd` whenever `attr != nullptr`, reading uninitialized memory if `IBV_QP_INIT_ATTR_PD` is not set in `attr->comp_mask`.
- **Fix:** Guard with `(attr->comp_mask & IBV_QP_INIT_ATTR_PD)` as in `ibverbs_provider.cpp:1153`.

#### `CR-49` — `hijack::intercept_host_get_device_pointer` `fail_open()` leaks CUDA registration and races on `s.fake_pages`
- [x] **Status:** Done (fail_open erases `skipped[p]` once the real register succeeds, so the unregister reaches the driver (also CR-76); the fake page insert re-checks `fake_pages` under the lock, and the loser joins its repeater, frees its page and returns the winner's device pointer)
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/uar_hijack.hpp:903-962, 921-928, 975-983`
- **Problem:** `fail_open()` registers `p` with CUDA but leaves `p` in `s.skipped`, so `intercept_host_unregister(p)` skips `real_host_unregister(p)`. Concurrent calls for the same UAR `p` can also allocate two fake pages and overwrite `s.fake_pages[p]` without re-checking under `s.mutex`.
- **Fix:** Erase `p` from `s.skipped` in `fail_open()` on successful registration, and re-check `s.fake_pages` under `s.mutex` before inserting.

#### `CR-50` — Data race in `hostdb::platform_refuses_gpu_doorbell()`
- [x] **Status:** Done (the whole probe runs under a function-local mutex; it runs once per UAR allocation)
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/host_doorbell.hpp:111-112, 203-277`
- **Problem:** Non-atomic `cached`, `g_said_unresolved`, and `g_said_inconclusive` are accessed unlocked, and two threads can probe the same `real_reg_addr` simultaneously.
- **Fix:** Guard the probe with `std::call_once` or a mutex.

#### `CR-51` — `ensure_installed` race window exposes partially-patched `ibv_context->ops`
- [x] **Status:** Done (a context being installed is tracked with its installer's thread id: another thread that finds it waits on a condition variable until the install finishes, while the installer's own re-entry (`record_device` -> ibv_query_device -> ensure_installed) still returns at once)
- **Subsystem:** `providers/ibverbs`
- **Locations:** `providers/ibverbs/ibverbs_provider.cpp:1821-1836`
- **Problem:** Inserts `ctx` into `g_installed_ctxs` and releases the mutex before `install(ctx)` finishes patching `ctx->ops.*`.
- **Fix:** Hold a per-context or install mutex across `install(ctx)` before marking `ctx` installed.

#### `CR-52` — Missing re-check between `_umonitor`/`_mm_monitorx` and `_umwait`/`_mm_mwaitx`
- [x] **Status:** Done (`wait_mem(addr, seen)`: both MWAITX and UMWAIT re-check the word after arming the monitor and return at once if it already changed)
- **Subsystem:** `providers/mlx5dv`
- **Locations:** `providers/mlx5dv/memory_repeater.cpp:47-80`, `providers/mlx5dv/memory_repeater.hpp:84-90`
- **Problem:** `wait_mem()` arms the monitor and immediately enters `_umwait`/`_mm_mwaitx` without re-checking `*hijack_addr == current`, missing stores that arrive between the prior load and `_umonitor` and stalling ~40 µs per miss.
- **Fix:** Pass `current` into `wait_mem()` and check `if (*hijack_addr != current) return;` after arming the monitor and before waiting.

#### `CR-53` — Stale `g_qp_lid` and `qp_srq_map` entries on QP destruction / device close
- [x] **Status:** Done (QP creation (both doors) calls `note_qp_created`, which drops the QP's stale `g_qp_lid` entry and erases `qp_srq_map[(ctx, qpn)]` for a QP without an SRQ (it used to keep the previous QP's possibly destroyed SRQ); `forget_context` drops both maps' entries for the context (`QpPort` now remembers its ctx))
- **Subsystem:** `providers/ibverbs`
- **Locations:** `providers/ibverbs/ibverbs_provider.cpp:132-148, 303-319, 1928-1934`
- **Problem:** Neither map is evicted on QP destroy or `forget_context`, causing stale LID/port or dangling `ibv_srq*` on handle/qpn reuse.
- **Fix:** Evict on `ibv_destroy_qp` and `forget_context`.

#### `CR-54` — `ucp_atomic_op_nbx` records element `count` as operand byte size `op_size`
- [x] **Status:** Done (the nbx operand width is decoded from `param->datatype` (contiguous class: size << UCP_DATATYPE_SHIFT) times count, or 0 when the caller gives no datatype. Case 22 gains `atomic_operands_are_words.sql` (op_size must be 0, 4 or 8))
- **Subsystem:** `providers/ucx`
- **Locations:** `providers/ucx/ucx_provider.cpp:736-745`, `providers/ucx/ucx_records.hpp:256`
- **Problem:** `UcxAtomic::op_size` is operand byte width (4 or 8), but `ucp_atomic_op_nbx` passes `count` (typically `1`) instead of decoding `param->datatype` element size.
- **Fix:** Extract element size from `param->datatype` (e.g. `ucp_dt_length` / datatype shift for contiguous types) and record `elem_size` (or `elem_size * count` as appropriate).

#### `CR-55` — `nvbit_at_cuda_event` ignores `CUresult*` and skips null-checks on alloc/VMM exits
- [x] **Status:** Done (an exit callback whose `*status != CUDA_SUCCESS` returns before any handler runs, and the alloc handlers null-check `dptr`)
- **Subsystem:** `gpu-backends/nvbit`
- **Locations:** `gpu-backends/nvbit/nvbit_backend.cu:1173-1174, 1197-1205, 1226-1245, 1254-1282`
- **Problem:** Failed `cuMemAlloc*` / VMM calls still register garbage handles/pointers and emit bogus `MemObject` records; `p->dptr` is dereferenced without null checks.
- **Fix:** Name and check `CUresult* res` (`if (res && *res != CUDA_SUCCESS) return;`) on exit events, and null-check output pointers.

#### `CR-56` — NVBit injects `comshu_kernel_stamp` and separate `launch_id`s into non-kernel `__device__` callees
- [x] **Status:** Done (one launch id per launch, shared by the kernel and all its related functions (`nvbit_set_at_launch` for each), and `comshu_kernel_stamp` is injected only where `nvbit_is_func_kernel`; the two belong together, because a callee's entry stamp under the kernel's id would count as a block. The id sequence is still drawn whether or not anything is instrumented)
- **Subsystem:** `gpu-backends/nvbit`
- **Locations:** `gpu-backends/nvbit/nvbit_backend.cu:563-601, 852-904`
- **Problem:** Related non-inlined `__device__` functions get `comshu_kernel_stamp` injected (without matching `EXIT` since they end with `RET`) and receive distinct `g_grid_launch_id++` values that have no `GpuKernel` metadata row.
- **Fix:** Inject `comshu_kernel_stamp` only when `nvbit_is_func_kernel(ctx, f)` is true, and pass the *kernel's* `launch_id` to related `__device__` functions instead of incrementing `g_grid_launch_id` per callee.

#### `CR-57` — Null-pointer dereference in `gpumem::Engine::process()` when `declare_provider()` fails
- [x] **Status:** Done (`process()` skips a null pipeline; the release goes through EngineHold (CR-58), so it happens only if acquire did)
- **Subsystem:** `providers/gpumem`
- **Locations:** `providers/gpumem/engine.hpp:223-231, 426, 435-438`
- **Problem:** Early return leaves `m_pipeline == nullptr` and skips `comshu_engine_acquire()`, yet `process()` dereferences `m_pipeline->process(batch)` and `shutdown()` calls `comshu_engine_release()`.
- **Fix:** Guard `if (!m_pipeline) return;` in `process()` and track `m_acquired` before calling `comshu_engine_release()` in `shutdown()`.

#### `CR-58` — Engine reference-count (`Tracer::m_active`) mismatches in CUPTI, UCX, UCX-deep, and NCCL
- [x] **Status:** Done (`comshu/provider/engine_hold.hpp`: acquire at most once, release at most once and only if held, with internal linkage so each provider .so keeps its own. All seven providers/backends use it, including ibverbs and mlx5dv, which the ticket did not list but which had the same double release (launcher shutdown() and .so destructor). CUPTI's destructor now releases on its never-armed early return)
- **Subsystem:** `gpu-backends/cupti`, `providers/ucx`, `providers/ucx_deep`, `providers/nccl`
- **Locations:** `gpu-backends/cupti/cupti_backend.cpp:587, 604-605, 623`; `providers/ucx/ucx_provider.cpp:376-379, 934-949`; `providers/ucx_deep/ucx_deep_provider.cpp:759-763, 811-826`; `providers/nccl/nccl_provider.cpp:151-155, 452-467`
- **Problem:** `cupti_backend` acquires unconditionally but skips release when `!g_on`; `ucx`/`ucx_deep`/`nccl` skip acquire when `declare_provider()` fails yet still release on `.so` unload, and can double-release if `shutdown()` is called before unload.
- **Fix:** Track an explicit `bool acquired` flag per provider/backend so every `comshu_engine_acquire()` is paired with exactly one `comshu_engine_release()`.

---

### CLI, Reader, SQL & UI Backend

#### `CR-59` — Unescaped file paths and table names interpolated into DuckDB SQL
- [x] **Status:** Done (`duck::sql_str` (doubles ') and `duck::sql_ident` (doubles ") in duck_mount.hpp, used at every site that splices a path or a table name into SQL: duck_mount, main.cpp (export, link, sql, temp_directory) and comshu-ui. `sql_cli_rule.sh` runs `comshu sql` and `comshu link` on a directory named `it's a "dir"`)
- **Subsystem:** `comshu-read`, `comshu-ui`
- **Locations:** `comshu-read/duck_mount.hpp:111-118, 135-142, 166-172`; `comshu-read/main.cpp:2607, 2641, 2651, 2669, 2701, 2729, 2850-2851, 2921-2922`; `comshu-ui/main.cpp:213, 218-219`
- **Problem:** File paths and table stems are concatenated into `'...'` and `"..."` SQL literals without escaping embedded quotes.
- **Fix:** Add helper functions `sql_quote_str()` (doubling `'`) and `sql_quote_ident()` (doubling `"`) and use them everywhere paths/names enter SQL.

#### `CR-60` — `link_health.wr_flight_backwards` ignores `_wr_pairable` and omits selective-signalling QPs
- [x] **Status:** Done (`_wr_flight_pairs` is both branches' pairs with no time filter; `_wr_flight` is its forward rows and `wr_flight_backwards` counts its backward ones, so the two cannot disagree. `wr_flow_rule.sh` adds qp 11, unpairable, whose completion carries the same wr_id but predates the sends: the old per-wr_id count reported it (2), and now only qp 7's real backwards pair counts (1))
- **Subsystem:** `comshu-read`
- **Locations:** `comshu-read/link_sql.hpp:3880-3900` (cf. `_wr_flight` at `173-253`)
- **Problem:** Runs branch-1 pairing across all QPs without `_wr_pairable` (false positives on selectively-signalling QPs) and omits branch 2 (false negatives).
- **Fix:** Mirror `_wr_flight`'s two-branch `_wr_pairable` logic or query `_wr_flight` directly for `done_ns < posted_ns`.

#### `CR-61` — Fan-out / miscount in `object_access.p2p` and `link_health.unattributed_p2p_accesses` on reused base addresses
- [x] **Status:** Done (`object_access.p2p` gives each access ONE extent, the smallest that contains it (`QUALIFY` over a per-access row id), and `unattributed_p2p_accesses` counts an access only when NO candidate extent contains it. `object_access_rule.sh` adds a buffer reallocated at the same base (one id, two sizes): an access in both yields one row, and one only in the larger is placed rather than counted. Without the QUALIFY the placements go red (mutation-checked))
- **Subsystem:** `comshu-read`
- **Locations:** `comshu-read/link_sql.hpp:2001-2016, 3669-3678`
- **Problem:** `am` selects `DISTINCT rank, id, range_base, range_size FROM mem_object`. Reallocating a different-sized buffer at the same base produces multiple rows with the same `(rank, id)`, duplicating `object_access.p2p` rows and falsely counting valid accesses as unattributed against the smaller extent.
- **Fix:** Match extents by lifetime window (or deduplicate/constrain to the live extent at access time).

#### `CR-62` — `run_sql()` nondeterministically overwrites `.parquet` views with `.csv` views
- [x] **Status:** Done (`run_sql` mounts in two passes, Parquet first, then CSV only for stems not yet mounted)
- **Subsystem:** `comshu-read`
- **Locations:** `comshu-read/main.cpp:2838-2854`
- **Problem:** `fs::directory_iterator` order is unspecified; when both `<table>.parquet` and `<table>.csv` exist, whichever is visited second wins.
- **Fix:** Mount `.parquet` files first (recording mounted table names) and only mount `.csv` for stems not already mounted.

#### `CR-63` — Unsigned underflow in `build_trace()` timestamp subtraction & `uint64_t` overflow in `SendRate`
- [x] **Status:** Done (`ns()` pins an event before `origin` to 0 and counts it (`before_origin`, reported beside `unanchored`) instead of wrapping; the rate is `bytes * 1000 / window_ns` (MB/s exactly) in 128-bit)
- **Subsystem:** `comshu-read`
- **Locations:** `comshu-read/main.cpp:3133-3135, 3273, 3286-3287`
- **Problem:** `wall_at(t.a.ticks) - origin` underflows `uint64_t` if an event precedes `origin` or uses an unanchored clock domain; `win_bytes * 1000000000ull / window / 1000000ull` overflows `uint64_t` above ~18.4 GB per window.
- **Fix:** Clamp/handle events `< origin` safely (or skip unanchored timestamps), and simplify `SendRate` to `win_bytes * 1000ull / (window ? window : 1)` (or use `double` / `__uint128_t`).

#### `CR-64` — Unchecked `std::fopen()` in `export_csv()` / `write_mem_owner()` & incomplete JSON escaping in `comshu-ui`
- [x] **Status:** Done (every fopen in export_csv/write_mem_owner is checked and every fclose too; failures name the file and exit 1, so an incomplete export no longer reports success. `json_escape` emits \\u00XX for all control characters. `csv_export_contract.sh` exports into an existing read-only directory: refused by name, where it used to segfault)
- **Subsystem:** `comshu-read`, `comshu-ui`
- **Locations:** `comshu-read/main.cpp:2229-2232, 2455-2457, 2484-2485`; `comshu-ui/main.cpp:53-77`
- **Problem:** `export_csv()` opens 65+ files without checking for `nullptr` before `fprintf`/`fputs`; `comshu-ui`'s `json_escape()` leaves control chars `0x00..0x1F` (other than `\n`, `\r`, `\t`) unescaped, producing invalid JSON.
- **Fix:** Check `fopen()` return values and return an error cleanly; escape all `static_cast<unsigned char>(c) < 0x20` as `\u00XX` in `comshu-ui`'s `json_escape()`.

---

### UI & Agent

#### `CR-65` — Agent & UI edge-case correctness bundle
- [x] **Status:** Done (all ten items: (1) agent SQL lexer respects string literals; (2) null encoding refused, transform 'as' fields not demanded; (3) counting aggregates not coerced to numbers; (4) Chart finalizes a view rendered after unmount; (5) Timeline: hash timer cleared on unmount, long-only hover, slop on long spans, qview in the density tooltip, no Math.max spread; (6) Graph: unresolved dst excluded from peers and highlights, facetBytes 0 guarded; (7) Objects: stale queries dropped, bin clamped, base via BigInt; (8) Devices: null nodes never 'same host', verbs view passes owner_devices; (9) Chat: only the current request clears busy/controller; (10) Sankey: verbs and mlx5 opcodes decoded/namespaced. Tests: agent/test/lib.test.mjs, ui/test/chart.test.mjs)
- **Subsystem:** `agent`, `ui`
- **Locations & Fixes:**
  1. `agent/lib/query.mjs:11-19, 33, 82` — `stripComments()` and `;` check do not respect SQL string literals (`'--'`, `'/*'`, `';'`). Lex string literals before stripping comments / checking semicolons.
  2. `agent/lib/spec-guard.mjs:88-92, 166-178` & `agent/comshu-agent.ts:141-147` — Guard `view.encoding === null` before `Object.entries()`, and exclude transform-derived `as` field names from the SQL-column existence check.
  3. `ui/src/lib/chart.js:76, 84-92` — Don't coerce fields to `Number` when `aggregate` is `'count'` or `'distinct'` (causes string columns to become `NaN`).
  4. `ui/src/lib/Chart.svelte:14-32` — Finalize `View` if `dead` is true when `await render(...)` resolves after unmount.
  5. `ui/src/panels/Timeline.svelte:138-144, 311, 334-343, 400, 474` — Clear `writeHash` timer on unmount; don't bail on `!rows.length` when `longRows.length > 0`; apply 6 px slop check to `bestLong`; use `qview` in density tooltip; replace `Math.max(...ns)` with a loop to avoid stack overflow on 100k rows.
  6. `ui/src/panels/Graph.svelte:109, 328-333, 397` — Filter `dst_rank >= 0` in `highlighted` and `peers` set; guard `facetBytes === 0` against `NaN%`.
  7. `ui/src/panels/Objects.svelte:109-155, 118-121, 360` — Add cancellation tokens to both `$effect` queries; clamp `bin` to `<= 63` for 0-byte writes at `w.hi`; format 64-bit `o.base` with `BigInt(o.base).toString(16)`.
  8. `ui/src/panels/Devices.svelte:392-398, 643-644, 666` — Check `an === null || on === null` in `reach(r)` so unknown nodes don't match as `'same host'`; pass `devices` to `actorLabel()` in the verbs view.
  9. `ui/src/panels/Chat.svelte:83-93` — Guard `finally` cleanup (`busy = false; controller = null;`) against clobbering a newer in-flight request after `stop()`.
  10. `ui/src/panels/Sankey.svelte:22-26` — Namespace `verbs` vs `devx` opcodes (or decode them to names) so incompatible opcode numbers don't collide in stage 1.

---

## Low

- [x] **`CR-66`** (`libcomshu/src/cuda_alloc_doors.cpp:252-289, 421`) — TOCTOU race between `was_created_here(h)` and `map_is_first(h, ptr, size)` acquiring `g_created_mutex` separately. Combine into a single locked operation. **Done 8c01b47:** one `map_is_object()` under one lock.
- [x] **`CR-67`** (`libcomshu/include/comshu/core/mem_attr.hpp:51-60`) — Data race on `static CuPointerGetAttributeFn cached` on first resolution; make atomic or use `std::call_once`. **Done:** atomic cached pointer, stored only once resolved (the cache-on-success rule is kept).
- [x] **`CR-68`** (`providers/ibverbs/ibverbs_provider.cpp:1631-1635, 1978-1980, 2025-2027, 2060-2065`) — `trace_unmatched_destroy_cq` and `trace_unmatched_dereg` always print `installed=yes` because `ensure_installed()` runs right before them. Check membership before calling `ensure_installed()`. **Done:** destroy_cq/dereg_mr read `context_installed()` BEFORE `ensure_installed()` and pass it in.
- [x] **`CR-69`** (`comshu-read/link_sql.hpp:130-136`) — `_wr_pairable` selects `s.rank, s.qp_num` after `FULL JOIN ... USING (rank, qp_num)`, emitting `NULL` for completions-only QPs. Select unqualified `rank, qp_num`. **Done:** unqualified `rank, qp_num` after the USING join.
- [x] **`CR-70`** (`comshu-read/link_sql.hpp:2515, 2522`) — `kernel_sends` omits the `n.texts = 1` ambiguity guard when joining `_intern_name`. **Done:** the `_intern_name` join requires `texts = 1`; ambiguous names read '?'.
- [x] **`CR-71`** (`agent/bridge.mjs:57, 74, 159, 170-179, 256-261`) — `startupError` is never cleared once `capture_ok` recovers; `new URL(...)` on malformed `Host` is outside `try/catch`; IPv6 loopback `new URL('http://[::1]:8777').hostname` is `'[::1]'` rather than `'::1'`. **Done:** /health builds the system prompt once the capture answers and clears `startupError`; a malformed Host gets a 400; '[::1]' is loopback.
- [x] **`CR-72`** (`agent/eval.mjs:58, 104, 188-189`) — `digits()` strips spaces/newlines and concatenates distinct numbers; `--verbose` crashes with `TypeError: r.queries is not iterable` if a case threw. **Done:** `digits()` removes only , and _ between digit groups (spaces kept — '4 128' must not become 4128); verbose output tolerates a case with no queries.
- [x] **`CR-73`** (`ui/src/panels/Matrix.svelte:96`, `ui/src/panels/Graph.svelte:255`) — Empty `comm_edges` leaves both panels stuck on `"loading…"` forever instead of rendering an empty state. **Done:** Matrix and Graph carry a `loaded` flag; loaded-and-empty shows an empty-state line.
- [x] **`CR-74`** (`agent/lib/preamble.mjs:26-27`) — Empty `names` list produces invalid SQL `SELECT t, n FROM () ...` in `tableCounts()`. **Done:** an empty table list returns [] before building SQL.
- [x] **`CR-75`** (Meta / Regression Coverage) — Add targeted unit/regression tests for each Critical/High fix above as tickets are closed (e.g., `query_guard_test`, `fork_smoke` verifying child `ClockAnchor`/`NodeIdentity`, concurrent `PreloadHookManager` resolution test, `sankey.js` unit test, `Objects.svelte` boolean-string check). **Done:** covered by the tests added with the fixes: query_guard_test, hook_smoke (registry concurrency, re-install), fork_smoke (child anchors), crash_identity arm 5, arena_drain_race, dlvsym_next, intern_damage, cq_ring_cache, srq_walk, nccl_event_pool, ucx_rkey_abi, csv_export_contract, bake_rule, sql_cli_rule (quoted paths), wr_flow/object_access/capture_integrity fixtures, ui/test (sankey, query bool, chart), agent/test (session, lexer, spec guard). NOT covered: concurrent `PreloadHookManager::register_real` resolution, which hook_smoke documents cannot resolve from a test binary; it needs an interposing .so harness.
- [x] **`CR-76`** (`providers/mlx5dv/uar_hijack.hpp` `intercept_host_get_device_pointer` fail-open, `intercept_host_unregister`) — After a fail-open, the UAR page IS registered with the real driver (`reg(p, intent.bytesize, intent.flags)`), but `skipped[p]` stays, so `intercept_host_unregister` answers success and never calls the real `cuMemHostUnregister`: the registration leaks for the life of the process. Record the fail-open (e.g. erase `skipped[p]` after the real register succeeds, or mark the intent as really registered) so the unregister is forwarded. **Done:** with CR-49: fail_open erases `skipped[p]` after a successful real register
