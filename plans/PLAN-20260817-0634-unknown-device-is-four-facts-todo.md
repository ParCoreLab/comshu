# `kUnknownDevice` is four different facts wearing one number

Status: **todo**
Created: 2026-08-17 06:34
Touches: `libpcomm/include/pcomm/core/mem_attr.hpp`,
`libpcomm/include/pcomm/core/records.hpp`, `libpcomm/include/pcomm/core/vocab.hpp`,
`providers/ibverbs/ibverbs_provider.cpp`, `libpcomm/src/engine.cpp`,
`pcomm-read/link_sql.hpp`

## The problem

`device_of()` returns `kUnknownDevice` (0xFFFF) from four return sites covering
**five distinct situations**, and the capture cannot tell them apart:

| # | site | what actually happened | what it implies |
|---|---|---|---|
| 1 | `mem_attr.hpp:85` (`!fn`) | `dlopen("libcuda.so.1", RTLD_NOLOAD)` found nothing | **we never asked** |
| 2 | `mem_attr.hpp:85` (`!addr`) | null address | a caller bug |
| 3 | `mem_attr.hpp:91` (`rc != 0`) | not a CUDA pointer | **genuinely host memory** |
| 4 | `mem_attr.hpp:93` (`type != DEVICE`) | CUDA knows it, it is host/managed/array | host, but CUDA-visible |
| 5 | `mem_attr.hpp:96` (ordinal query failed) | device memory, GPU unnameable | **a real attribution failure** |

Rows 1, 3 and 5 imply completely different follow-up work — fix the load order,
nothing, and chase a driver bug respectively — and they are byte-identical on
disk. The only disambiguator today is `trace_device_attr()`
(`mem_attr.hpp:68-78`), which prints the **first attempt only**, to stderr, gated
on `PCOMM_TRACE_HOOKS=1`. Nothing survives into a capture.

This is not hypothetical. `mem_attr.hpp:5-10` records that the first copy of this
function shipped with `kAttrDeviceOrdinal = 4` (that is
`CU_POINTER_ATTRIBUTE_HOST_POINTER`, not `DEVICE_ORDINAL`) and **the bug survived
the provider's entire life** for exactly this reason: every failure mode looks
like every other one. The header already says so. This plan is the part that
makes the claim checkable rather than commented.

## The evidence that prompted it

Measured 2026-08-17 on `hubspoke_ib-44578395`, cross-checked on
`pipeline_ib-44578688` and `skew_ib-44580750`:

- **487 of 507** `mem_object` rows carry `device = 65535`; 20 carry `device = 0`.
  The split is **identical on all three captures and on all four ranks**
  (121-122 unknown + 5 attributed each) — it is bootstrap footprint, invariant
  of the workload.
- Joined to `ib_mem_region` on `(rank, range_base)`, the partition is perfectly
  clean: **100 %** of `origin = RegMr` (`ibv_reg_mr`) regions are unattributed;
  **100 %** of `Dmabuf` + `nvshmem_malloc` objects resolve to device 0. Nothing
  is mixed.
- Address classes of the 487: 216 at brk-heap addresses (8 B – 3 KiB) and 271
  mmap'd `0x7f…` chunks in UCX mpool shapes (27 × 38 MiB, 24 × 532 KiB, …).
- Of 18,441 `ib_post_send` rows that resolve to a peer object on
  `pipeline_ib`, **zero** land in a 65535 object.

So the current reading — these are host transport buffers, case 3 above — is
almost certainly right. **That is the point.** We believe it from address shapes
and a p2p control capture, not from anything the tracer recorded. The same
evidence would look the same if a fraction of them were case 1 or case 5.

## The ordering hazard, stated honestly

`tests/hpc/asym_patterns.cu` calls `MPI_Init` (line 90) **before**
`cudaSetDevice` (line 98). Measured on `hubspoke_ib-44578395`: on every rank,
only **1 of ~121** unknown registrations occurs after the first successful
device resolution (rank 0: unknowns span ticks 1832567813318811 →
1832572196656851; the first `device = 0` object is at 1832572194718923). Every
other one happens inside the MPI/UCX bootstrap window.

The honest bound on this: a *device* pointer cannot exist before libcuda is
loaded, so a GPU buffer being misfiled as host is not the likely failure. And
`cu_pointer_get_attribute()` deliberately **caches only on success**
(`mem_attr.hpp:48-59`), so an early miss does not latch — that class of bug was
already fixed once.

The residual risk is narrower but real:

- The lookup is by the single soname `libcuda.so.1`. A process holding the
  driver under a different soname, or behind a namespace, resolves to nothing.
- Case 5 (ordinal query fails on real device memory) is indistinguishable from
  case 3 and would be invisible.

The cost that actually justifies work here is not a known bug — it is that the
claim "these 487 are host memory" is **unfalsifiable from the capture**, in a
function whose comment header exists because that property already cost us once.

## Design

Record the *outcome of the attempt*, not only its result.

**1. An enum in `vocab.hpp`**, beside `kUnknownDevice`:

```
enum class AttrStatus : std::uint8_t {
    Device        = 0,  // resolved; `device` is the ordinal
    HostNotCuda   = 1,  // cuPointerGetAttribute said "not a CUDA pointer"
    HostCudaKnown = 2,  // CUDA knows it, memory type is not DEVICE
    DriverAbsent  = 3,  // RTLD_NOLOAD found no libcuda — we never asked
    NullAddr      = 4,  // caller passed nullptr
    OrdinalFailed = 5,  // device memory, ordinal query failed
};
```

**2. `device_of()` returns both.** Either a small `struct { DeviceId device;
AttrStatus status; }` or an out-parameter. Prefer the struct: an out-param that
callers may ignore reintroduces exactly the silent-drop this plan is about.
Keep the header CUDA-free and `RTLD_NOLOAD`-only — both properties are load-bearing
(`mem_attr.hpp:12-15, 43-47`) and neither changes.

**3. One field on `MemObject`** (`records.hpp:93`): `AttrStatus attr_status;`
after `DeviceId device;`.

**4. Call sites carry it through**: `ibverbs_provider.cpp:580` (`reg_mr_iova2`),
`:622` (`reg_mr`), `:911-914` (dmabuf — status is `Device`, sourced from
`dmabuf_registry.hpp:127`), `engine.cpp:516` (`nvshmem_malloc` hook).
`ibverbs_provider.cpp:633` (`reg_dm_mr` / `alloc_null_mr`) still emits no
`MemObject` — on-NIC memory is not a host allocation and that stays deliberate.

**5. The derived layer learns the difference.** `link_sql.hpp:439` and `:450`
(`WHERE device <> 65535` in `_rank_device` / `_device_owner`) keep filtering —
host memory must not vote on which GPU a rank owns. What changes is that
`link_health` gains a finding:

```
attribution_never_attempted   n  objects whose device query never ran (no libcuda at registration time)
attribution_failed            n  device memory whose GPU ordinal could not be read
```

`HostNotCuda` / `HostCudaKnown` are **not** findings — they are the expected,
healthy answer for transport buffers, and reporting 487 of them as a problem is
how a health view teaches people to ignore it. The UI can then say *host* rather
than *unknown*, which is the user-visible payoff.

## Options rejected

- **Keep the stderr probe, document harder.** This is the status quo, and the
  status quo already hid a wrong constant for a provider's entire lifetime. A
  first-attempt-only stderr line cannot answer a question about the 487th
  registration.
- **A sibling record** (`MemAttrOutcome`, joined on range). One extra stream and
  one extra join to carry one byte that has exactly the same lifetime and
  cardinality as the row it describes. `MemObject` is the right place.
- **More sentinels in `device`** (`0xFFFE` = driver absent, `0xFFFD` = query
  failed, …). Tempting because it needs no schema change — and wrong. It
  overloads a field whose declared meaning is "which GPU", and every existing
  `device <> 65535` filter would silently start counting driver-absent objects as
  real devices. A field that means two things is the bug this plan is fixing.
- **Infer it in `pcomm-read` from address shape** (brk vs mmap vs heap range).
  That is the heuristic we used to *reach* this conclusion by hand; promoting a
  forensic guess to a recorded fact is the failure mode `link_health` exists to
  prevent.

## Steps

1. **Measure the stride first.** `mem_object` is stride 80 today; the body is
   `Handle id` + `Handle name` + `MemRange` + `DeviceId` = 26 B, so there should
   be tail padding a `uint8` can occupy. **Verify this** — if the stride is
   unchanged, every existing capture keeps decoding with a hash warning rather
   than a per-stream refusal, and the corpus does not have to be re-run for
   unrelated work. If it does grow, that is acceptable
   ([[pcomm-prealpha-prefers-correct-design-over-compatibility]]) but should be a
   known cost, not a surprise — see `tests/hpc/corpus/README.md` on how stride
   drift degrades a capture in pieces.
2. `AttrStatus` in `vocab.hpp`; `device_of()` returns the pair; the four return
   sites set it.
3. `attr_status` on `MemObject`; thread it through the four emitters.
4. `link_sql.hpp`: surface the column, add the two `link_health` findings.
   Leave the `<> 65535` filters alone.
5. Re-run the corpus. **Expected**: all 487 come back `HostNotCuda`, both new
   `link_health` findings are 0, and the 20 attributed objects are `Device`.
   Any other distribution is itself the finding.
6. **A negative test that must fail loudly.** The suite has to prove the new
   states are reachable, or they are decoration. Cheapest is a host-only run
   (MN5 core, no CUDA — `mem_attr.hpp:12-15` says the suite already has to stay
   green there): every object should report `DriverAbsent`, not `HostNotCuda`.
   That single assertion is what makes the distinction real.

## Verification

The plan succeeds when this question is answerable **from a capture alone**, with
no stderr and no p2p control:

> Of the objects with no device, how many did we ask about and get "host", and
> how many did we never ask about?

Today that requires re-running the workload under `PCOMM_TRACE_HOOKS=1` and
reading the first line of stderr.

## Addendum, 2026-08-18: say *where* the host memory is

Raised while preparing the lab talk, and it is the other half of the same
question. Once `attr_status` lets the UI say **host** instead of **unknown**, the
obvious next question from anyone looking at the Objects panel is *"host memory
on which host?"* — and today that is answerable from the capture but not from the
screen.

**The data is already there.** `node_identity` (type 524) is emitted once per
rank by the engine, and `rank_identity` (`link_sql.hpp:977`) already renders the
`n0 · GPU 0` label that the chat pane uses. Measured on `demo-out` (job
`44748943`, 16 ranks): 4 distinct `node_hash` values, 4 ranks each, clean.

**What is missing is one join.** `ui/src/panels/Objects.svelte:50` selects
`m.rank, m.id, base, size, m.device` and the header row at `:255` is
`['', 'rank', 'device', 'base', 'size', 'touched by', 'via', 'accesses']`.
`rank_identity` is never joined, so the panel can tell you a 38 MiB object is on
rank 9 with no device, and cannot tell you rank 9 is on `n2` — which is the
entire content of "where does this live".

So, as a step after the ones above:

7. **Join `rank_identity` in the Objects panel** and show the node. Preferably
   *as* the rank column (`rank 9 · n2`) rather than a seventh column — the table
   is already wide, and rank and node are one fact about the process, not two.
   The detail pane at `:186` (`<b>{bytes}</b> object on rank {sel.rank},
   {dev(sel.device)}`) should read `on rank 9 · n2, host` once `attr_status`
   exists.

**A boolean `is_device` was considered and is the wrong shape**, for the reason
already in "Options rejected" above: the question has five answers, not two, and
`HostNotCuda` / `DriverAbsent` differ by whether we ever asked. A boolean would
record the *guess* we currently make in prose and freeze it into the schema —
`DriverAbsent` would have to be filed as either "host" (a lie) or "device" (a
worse lie). `AttrStatus` collapses to a boolean for display whenever a reader
wants one; a boolean does not expand back.

**Scope note.** This step is UI-and-derived only and does not depend on steps 1-6
— the node label is available today. It is listed here rather than as its own
plan because shipping it *before* `attr_status` would let the panel say "rank 9 ·
n2" next to a device column reading `unknown`, which invites exactly the
misreading ("so it is nowhere?") that prompted this. Ship them together.

## Related

- `things-to-circle-back-to.md:900-935` — the `kAttrDeviceOrdinal = 4` bug.
- `plans/PLAN-20260806-1329-generic-memory-attribution-done.md` — why `device_of`
  was extracted before its second copy existed.
- The IB corpus carries no gpumem provider, so p2p's other ~34 device
  allocations per rank have **no `MemObject` row at all** in an IB capture. That
  is absence, not misattribution, and it is out of scope here — but it is why an
  IB capture knows about only 5 GPU objects per rank.
