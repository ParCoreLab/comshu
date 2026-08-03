# PLAN — gpumem records must say when they *might* have happened, not lie about when they did

- **Created:** 2026-08-03 10:21 +03
- **Status:** TODO
- **Finding:** `things-to-circle-back-to.md` — "gpumem stamps drain time as an
  Instant". This plan is the fix; the finding is the evidence.

## The problem

Every `GpuAccess` in every capture carries `TimeKind::Instant` at a host TSC
reading taken **when the drain thread dequeued it from the NVBit channel**. The
chain, all verified in the tree:

| Step | Code | What it contributes |
|---|---|---|
| device instrumentation | `inject_funcs.cu` | pushes `pcomm_gpu_access_t` — **no clock field** |
| device→host packet | `dev_common.h` | `{dev_id, lane_id, opcode_id, instr_index, access_size, addrs[32], thread_index}` — **no time** |
| host drain | `nvbit_backend.cu:445` | `g_engine->process(batch)` after `recv()` |
| pipeline | `attr/pipeline.hpp:29` | `m_rec->record(rec)` — the **one-argument** overload |
| recorder | `recorder.hpp:61` | `record(const R& r) { append(now_instant(), r); }` |

`now_instant()` is read on the drain thread, at drain time. So the timestamp is
wrong in three independent ways:

1. **Wrong moment.** It is when the host dequeued the record, not when the SM
   issued the access. The channel ring is 1 GiB and there is a *device-side*
   buffer in front of it (`ChannelDev::buff` + doorbell in
   `nvbit-1.7.7.1/utils/channel.hpp`), so the lag is unbounded.
2. **Wrong precision claim.** `Instant` asserts "this happened exactly at `a`".
   Nothing in the system knows that.
3. **Wrong order, silently.** A whole batch is stamped within microseconds of
   each other in *flush* order. Accesses far apart on the device collapse to
   near-identical ticks; accesses from different warps are ordered by when the
   channel happened to drain, not by when they occurred.

`Temporal::window` — *"known only within [a, b], uncertainty made explicit"*
(`core/time.hpp:17`) — is the constructor for exactly this, and it has **zero
call sites** in the tree. The model already had the right concept; the producer
never used it.

**Why this has been invisible.** No gpumem golden queries time: cases 03, 04,
05, 06, 07 assert counts, sizes, op kinds and `run_dev`/`mem_dev` pairs — all
order- and time-independent (verified: no `ticks` or `SELECT *` in any of their
`.sql`). The suite cannot see this, and would not have caught it if it were
worse.

## The end of the window is free. The start is the whole problem.

**Upper bound — already in hand.** The access provably happened *before* the
drain thread dequeued it. That is precisely the timestamp the code takes today.
So the current (wrongly-labelled) value is kept, unchanged, as `b`. Nothing is
lost by this change; a false claim becomes a true one.

**Lower bound — three candidates, only one is sound:**

### Rejected: previous drain time

Tempting and free: `[t_prev_drain, t_this_drain]`, no device change at all.
**Unsound.** `ChannelDev` buffers pushes on the *device* (`buff`,
`buff_write_head_ptr`, doorbell) and ships them when full. An access can occur
at T, sit in the device buffer, and reach the host ring long after
`t_prev_drain` — putting the true time *outside* the window. A bound that can
exclude the real value is worse than no bound, because it looks rigorous.

### Rejected: device `%globaltimer` per access

Exact, and the `ClockAnchor` machinery (`DomainId`, offset+skew fitting in
`pcomm-read`) already exists to map a GPU clock domain onto the host timeline.
But reading `globaltimer` inside the injected function costs a clock read on
*every instrumented instruction*, on the hot path of the thing being measured —
and case 07 already demonstrates that this instrumentation is delicate enough
that one extra local-memory access wedges NVSHMEM's proxy transport. Not worth
it for a bound that `[launch, drain]` gets to within a kernel duration.

Revisit only if per-access ordering inside a kernel is ever actually needed.

### Chosen: kernel launch time

The access cannot precede the launch of the kernel that issued it. Sound by
construction, and **the plumbing is already 90% built**:

- `handle_launch()` (`nvbit_backend.cu:292`) assigns `g_grid_launch_id++` per
  launch and pushes it to the device with `nvbit_set_at_launch` (line 329).
- the instrumentation passes it back as argument 0 of every injected call
  (line 281, `nvbit_add_call_arg_launch_val64(instr, 0)`).
- `pcomm_instrument_mem` **already takes `uint64_t grid_launch_id`** as a
  parameter (`inject_funcs.cu:13`) — and then never stores it.

So each access already knows which launch it belongs to. What is missing is one
field in the packet, one assignment on the device, and a TSC read at launch.

**Known looseness, stated rather than papered over:** `handle_launch` fires when
the host *enqueues* the launch, not when the GPU begins executing it. With a
deep stream queue the true start is later, so the window is wider than
necessary. It remains sound — that is the requirement — and it is tighter than
anything else obtainable without device clocks. `[enqueue, drain]` is the
honest statement of what we know.

## Design

```
Temporal::window(t_launch[grid_launch_id], t_drain)
```

Both are host TSC, same clock domain as everything else in the capture, so no
new clock domain and no `ClockAnchor` work.

**Crucially, this changes no on-disk layout.** `Stamped<R> = {Temporal
temporal; R payload}` (`thread_local_buffer.hpp:28`) — the `Temporal` is already
in every record, 48 B, with `kind`, `a`, `b`, `value`. This change writes
different *values* into fields that already exist:

| field | before | after |
|---|---|---|
| `temporal.kind` | `Instant` (1) | `Window` (3) |
| `temporal.a` | drain tick | launch tick |
| `temporal.b` | `{0,0}` | drain tick |

**No `.fbs` edit. No `kSchemaHash` bump. No new type_id. No record version.**
`schema_conformance` and `schema_check.py` should both pass untouched — which is
itself a check on this claim: if either complains, the change is bigger than
this plan thinks it is.

## Steps

1. **Carry the launch id to the host.**
   - `dev_common.h`: add `uint64_t launch_id` to `pcomm_gpu_access_t`.
   - `inject_funcs.cu`: `ma.launch_id = grid_launch_id;` (the parameter is
     already there).
   - This is a device↔host ABI *inside one `.so`*, not a capture format. Both
     sides are rebuilt together by `build-gpumem`.
   - NB the packet grows by 8 B (296 → 304). It is warp-coalesced (one push per
     32 lanes), so this is ~0.25 B per access, but it does change channel
     throughput slightly — worth watching case 05's runtime, which is
     NVBit-module-bound and should not move.

2. **Time the launches.**
   - `nvbit_backend.cu`: in `handle_launch`, `t_launch[launch_id] =
     pcomm::helpers::TscClock::now()`.
   - Storage: the ids are dense and monotonic (`g_grid_launch_id++`), so a
     `std::deque`/ring indexed by id beats a map. Entries must be reclaimed —
     a long run launches millions of kernels. Bound it (e.g. keep the last N,
     fall back to "unknown" below) rather than growing without limit.
   - Already under `g_mutex` in the callback, so no new locking.

3. **Carry the window through the pipeline.**
   - `attr/interfaces.hpp`: add the two ticks to `RawAccess` (not the launch id
     — the backend resolves it, so `libpcomm` never learns what a launch is).
   - `attr/pipeline.hpp:29`: `m_rec->record(temporal, rec)` — the two-argument
     overload that already exists.
   - `nvbit_backend.cu` recv loop: fill them when building the batch.

4. **Handle the unknown case honestly.** An access whose launch id has no
   recorded time (id evicted, or a launch path not intercepted — e.g. graph
   node launches, `nvbit_at_graph_node_launch` is currently an empty stub at
   line 491) must **not** silently get a fabricated lower bound. Emit
   `Temporal::window({0,0}, t_drain)` — an open lower bound — and count these,
   reporting the count at shutdown. A capture that is mostly open-bounded should
   say so.

5. **Expose it in the reader.**
   - `pcomm-read`: `gpu_access.csv` currently has one `ticks` column printed
     from `temporal.a.ticks`, formatted identically to `ib_post_send`'s `ticks`,
     which is a *measured* call-entry time. Rename to `ticks_lo,ticks_hi` so the
     two cannot be silently joined. Safe: no golden queries `gpu_access.ticks`
     (verified), and `sql_check.sh` binds by column name.
   - `RankClock::dur_ns` (`main.cpp:439`) tests `kind == Interval` and returns 0
     otherwise, so a Window renders as a zero-duration instant. Make the Window
     case explicit rather than incidental — gpu accesses are not currently
     exported to Perfetto at all, so this is a comment plus a guard today, and
     the place to decide on uncertainty rendering when they are.

6. **Add a test that would have caught this.** A case-03 check asserting
   `ticks_hi >= ticks_lo` and that gpu accesses are window-kinded. Cheap, and it
   pins the property that the whole plan is about. Without it the next
   refactor can quietly revert to `record(rec)` and no board turns red — which
   is exactly how this survived.

## Gate

Full `tests/hpc/run_on_mn5.sh`. Expected: **all 8 cases still PASS with goldens
unchanged**, because no golden queries time. If a golden moves, that is a real
signal — it means something time-dependent was being asserted without anyone
realising, and it should be understood before it is regenerated.

Also confirm case 05's runtime does not grow: it is dominated by NVBit module
processing (~440 s, measured stable across message sizes), and step 1 slightly
enlarges the channel packet.

## Out of scope

- Per-access ordering *within* a kernel. That needs device clocks; see the
  rejected option.
- `TimeKind::Counter`, the other unused kind. Nothing produces one, nothing
  reads `temporal.value`, and no producer is proposed. It stays declared and
  unused; that is a separate decision from this one.
- The ibverbs provider, which already emits genuine `Interval`s from the hook
  engine's call-entry/exit stamps and is not affected.
