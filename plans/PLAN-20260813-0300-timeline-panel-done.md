# A timeline panel that is actually a timeline

Replaces the `Timeline.svelte` stub, which said *"deliberately not a timeline —
handed to Perfetto"*. That was the right call while the UI had no time axis at
all. It stops being the right call the moment you need to answer "why did rank
17 stall at t=2.4s" without leaving the tool, exporting a `.pftrace`, and
uploading it somewhere.

**Perfetto stays.** `pcomm-read --pftrace` is not deprecated by this and the
hand-off line stays in the panel footer. The split is:

| | this panel | Perfetto |
|---|---|---|
| for | *is the shape right?* — light debugging, in-tool | *what exactly happened at t?* — forensics |
| cost | one canvas, one parameterised query | export + upload + a second tool |
| has | pan, zoom, density→spans, hover | filtering, search, pinning, flows, SQL, ftrace |

We are not building a second Perfetto. We are building the 5% of it that
removes the export step from the common case.

---

## What the capture already gives us, measured

Probed against `~/waste/pcomm/corpus/scale_hubspoke_ib-44542856` (64 ranks, the
capture regenerated in `0d254f4`):

- **2,987,732 spans** across `ib_post_send` / `ib_poll_cq` / `ib_post_recv` /
  `ib_recv_cqe`.
- Every one of those streams is `temporal_kind = 2` (**Interval**) — `a` and `b`
  are both populated. We have real durations, not just instants. Median
  `ib_post_send` is **244 ticks ≈ 122 ns**; max is 168,542 ticks ≈ 84 µs.
- The aligned window is **7.0 s** wide; the send-active part is 3.11 s.
- `mlx5_*` and `gpu_access` are empty here (verbs-only capture). The panel must
  read that as *"this capture has no such lane"*, not as a bug — same policy
  `duck_mount.hpp` already applies to absent streams.

### The constraint that decides the whole design

122 ns spans in a 7 s window is **1 : 57,000,000**. At 1400 px, one pixel is
5 ms and holds ~40,000 events on a busy lane. Drawing spans at that zoom is not
"slow", it is *meaningless* — every lane is a solid bar.

So the panel has **two rendering modes over one query shape**, switched on
events-per-pixel:

- **density** (zoomed out): per `(rank, stream, pixel)` → `count`, `max
  duration`, `sum bytes`. Colour is the log ramp from `viz.js`.
- **spans** (zoomed in, ≤ ~2 events/px): the rows themselves, drawn as rects
  with their real width, hoverable.

The switch is automatic and the mode is *stated in the caption*, because "each
mark is one message" and "each mark is 40,000 messages" look identical and are
not the same claim. This is the same rule `Devices.svelte` follows for its two
actor sources.

---

## Time alignment is a derived-layer problem, not a panel problem

Raw records carry `temporal_a_ticks` in a `domain` — TSC for the host streams.
**TSC is not comparable across nodes.** Measured on the corpus: min tick across
ranks is `101546585120773` while a rank-0 anchor sits at `1677281742642415`, a
16× gap. Plotting raw ticks on a shared axis would scatter a 64-rank job across
a fake 8-hour window.

`clock_anchor` is the fix and it is already captured: **two anchors per rank**
(128 rows / 64 ranks), each pairing `local_ticks` with `wall_ns`. Two points
define the line.

Per the standing rule — *"if a panel needs a table that does not exist, it
belongs in `pcomm link`, not in the panel"* — this goes in `link_sql.hpp`:

- **`_clock`** (internal view): per `(rank, domain)`, fit `t0`, `w0`,
  `ns_per_tick` from the anchor extremes. Measured `ns_per_tick ≈ 0.49998728`,
  i.e. a 2.0 GHz TSC, and it differs in the 8th digit *per rank* — which is
  exactly why the fit is per-rank and not a constant.
- **`timeline_span`** (kTable): `rank, stream, ep, bytes, t0_ns, t1_ns`, in
  wall-clock ns, unioned across every event stream and both transports.

Three traps, all hit for real during the build:

1. `temporal_a_ticks` is **UBIGINT**. `ticks - t0` underflows to ~1.8e19 for any
   tick below the first anchor, landing the event 584 years away. Subtract in
   `HUGEINT`.
2. **A wall-clock ns does not fit in a float64, and this one shipped before it
   was caught.** The first `timeline_span` computed `(w0 + delta*rate)::BIGINT`
   with `w0` a `DOUBLE`. Every duration in the capture came back **0 ns**.
   1.79e18 needs 19 digits, float64 carries ~15.9, so the ulp there is **256
   ns** — `(1786575955340045312::DOUBLE + 122)::BIGINT` returns its input
   unchanged. Median `ib_post_send` is 122 ns, so the entire span layer was
   being flattened by an addition that read as harmless. Fix: integers hold the
   absolute time, the `DOUBLE` only ever scales a *delta* (~1e10 ticks, where
   float64 resolves far below a ns).

   The same trap then appeared **again on the client**, because a JS number is
   the same float64: `t - view.lo` on two absolute ns quantises to 256 ns, and
   the panel's deepest zoom is a 100 ns window. The panel therefore works
   entirely in ns-since-`origin`, and `origin` is kept as the raw string and
   never passed through `Number()`. Verified after the fix: median 122 ns,
   exactly `244 ticks × 0.4999872`.
3. Some records may carry a `domain` no anchor covers. The join is **INNER**, so
   those rows are dropped rather than plotted at a lie — and a
   `timeline_unanchored` row is added to `link_health` so the drop is visible
   instead of silent. Same reasoning as `unresolved_nvlink`: a capture with no
   node identity once reported a clean bill of health while 100% of its traffic
   was unattributed.

### Measured cost

Measured end to end against the served capture, at the zoom levels the panel
actually walks through (density above ~2 events/px, spans below):

| window | events | ev/px | mode | query | payload |
|---|---|---|---|---|---|
| 6.996 s (full) | 2,987,732 | 2134 | density | 70 ms | 298 KB |
| 699.6 ms | 40,960 | 29.3 | density | 28 ms | 18 KB |
| 69.96 ms (dense region) | 635,391 | 454 | density | **73 ms** | **1016 KB** |
| 6.996 ms | 10,704 | 7.7 | density | 24 ms | 54 KB |
| 699.6 µs | 1,082 | 0.77 | **spans** | 22 ms | 62 KB |
| 70.0 µs | 109 | 0.08 | spans | 20 ms | 6 KB |
| 700 ns | 1 | 0.00 | spans | 20 ms | 124 B |

Eight orders of magnitude of zoom, worst case 73 ms and 1 MB. That is inside an
interactive budget with room to spare, so **the viewport drives the query** — no
client-side index, no pre-bucketing, no WASM. Pan and zoom re-query, debounced
at 90 ms with no overlapping requests (a drag fires dozens of frames, and a
queue of stale 70 ms queries would land out of order and paint the wrong
window).

The 1 MB row at 69.96 ms is the one to watch: it is a *dense* region at a zoom
where bins are still fine, so it maximises distinct `(rank, stream, px)` groups.
A 512-rank capture would multiply it by 8, which is where the follow-up
filtering work stops being cosmetic.

---

## Lanes

`rank` is the row. Each rank row holds one thin sub-lane per *present* stream
(post_send / poll_cq / post_recv / recv_cqe / device_send / doorbell /
gpu_access). 64 ranks × ~28 px = ~1800 px, vertically scrolled. Empty streams
take no sub-lane, so a verbs capture does not render three dead stripes.

Sorting by rank and not by activity, deliberately: rank order is the one
ordering the user already holds in their head from the Matrix and Graph panels,
and a timeline that reorders itself as you pan is unreadable.

---

## Interaction — the scope for this pass

**In:**
- horizontal pan (drag, shift-wheel)
- zoom about the cursor (wheel), clamped to the capture window
- vertical scroll over ranks
- hover readout: rank, stream, t, and either `n events / max dur / bytes`
  (density) or the single span's `qp`, duration, bytes
- `?` / reset-zoom control
- window state in the URL fragment, matching the existing `#time` convention, so
  a zoom is linkable

**Explicitly out of this pass — the follow-up list:**
- **filtering** — by rank set, stream, opcode, QP. The one most likely to be
  wanted first; the query already groups by `(rank, stream)` so a `WHERE` is the
  whole change.
- **search** — "find the longest `ib_poll_cq` on rank 17", jump-to-result.
- **pinning / bookmarks** — mark a t, keep it across zooms, measure between two
  pins.
- **flow arrows** — `ib_post_send` on rank A → `ib_recv_cqe` on rank B. The join
  already exists in `_recv_attr`; drawing it is the work.
- **lane collapse / grouping by node** — at 64 ranks the vertical scroll is
  tolerable; at 512 it will not be.
- **selection → cross-panel** — drag a time range, have Matrix and Flow honour
  it. This is the feature that would make the eight panels one tool rather than
  eight, and it is the reason the viewport lives in the URL from day one.

---

## Kernel lifetimes: we do NOT capture them, and SigmaTrace does

> **SUPERSEDED by `PLAN-20260813-0600-kernel-lifetimes-belong-to-the-engine`.**
> The two open questions below ("which clock", "what cost") were measured on MN5
> and du04 and both are answered — and one answer changes the design: CUPTI
> cannot coexist with NVBit, so the record needs two sources, not one. The
> `type 300` proposal below is withdrawn in favour of an engine-owned type 530.
> The section is kept because the *motivation* still stands unchanged.

Checked, because a timeline is the view that makes the gap obvious.

**SigmaTrace has them.** `Events/Events.hpp` declares `CudaKernelLaunch = 300`
and `CudaKernelExit`; `CUDATrace/CUDAHooks.hpp` defines the pair as
`cuda_kernel_launch(gpu_id, kernel_name)` / `cuda_kernel_exit(gpu_id,
kernel_name)`, emitted from the NVBit, Sanitizer and CUPTI sources alike
(`Profiler/Handlers/CUPTI.cpp` fills both). A launch/exit pair *is* a kernel
lifetime.

**pcomm does not.** There is no kernel record anywhere in `libpcomm/schema/pcomm.fbs`
— the whole type table (`tests/hpc/corpus/types.tsv`) goes from `mlx5_*` at
200–214 to `mem_object`/`gpu_access` at 520/700 with nothing for compute. Our
NVBit backend *sees* every launch — `nvbit_backend.cu` intercepts
`cuLaunchKernel`, `cuLaunchKernelEx` and `cuLaunchCooperativeKernel`, and
already resolves the mangled name — but it uses that purely to decide *whether
to instrument* (`PCOMM_KERNEL_NAME` filtering) and then throws the event away.

So we are one record away, not one subsystem away.

### Why this matters more than it sounds

Without kernel lifetimes the timeline can show *that* rank 17 went quiet for
400 µs, but never *whether it was computing or blocked*. Every interesting
multi-GPU question — is this overlap or serialisation, is the comm hidden behind
compute, is this a straggler or a stall — is a question about comm spans
**against** kernel spans. A comm-only timeline can pose those questions and
cannot answer any of them.

### Proposed record

```
// type_id 300, matching SigmaTrace's CudaKernelLaunch, deliberately
struct GpuKernel {
  temporal:Temporal;   // kind = Interval: a = launch, b = exit
  name:uint32;         // interned symbol (demangled), joins InternString
  device:uint16;
  stream:uint64;       // CUstream — lanes within a device
  grid_x:uint32; grid_y:uint32; grid_z:uint32;
  block_x:uint16; block_y:uint16; block_z:uint16;
  correlation:uint64;  // launch-site id, so async exit rejoins its launch
}
```

Two open questions this plan does not settle, because both need a GPU run:

1. **Which clock.** The launch callback gives a host TSC — that is *submission*,
   not execution, and on an async stream they can be milliseconds apart. Real
   execution bounds need either CUPTI activity records or a device
   `globaltimer` read from an injected prologue/epilogue, and that is a second
   `domain` needing its own `ClockAnchor`. SigmaTrace's CUPTI handler takes the
   first road; our NVBit backend is positioned for the second.
2. **Cost.** An epilogue timestamp on every kernel is not free, and the IBGDA
   work already measured ~+68% wall for a forwarder. Needs measuring before it
   goes on by default.

**Not in this pass.** It is a provider + schema + clock-domain change that
cannot be validated without an MN5 or du04 slot, and it would hold the panel
hostage. The panel is built so the lane appears the day the record does: add
`gpu_kernel` to the `timeline_span` union and it renders, with no panel change.

---

## Steps

1. `link_sql.hpp` — `_clock` view, `timeline_span` kTable, `timeline_unanchored`
   in `link_health`.
2. `ui/src/panels/Timeline.svelte` — canvas, two modes, pan/zoom, hover.
   Data through `query.js` only.
3. Keep the Perfetto hand-off as a footer line.
4. `pnpm build`, run `pcomm ui` against the 64-rank capture, verify against the
   measured numbers above.
5. `ui/README.md` — the Timeline row currently reads *"deliberately not a
   timeline"*. Fix it.
