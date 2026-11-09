# PLAN — what is next, now that the context-coverage gap is closed

> **Closed 2026-11-09 (backlog triage).** Items 1-3 done; of item 4, the dmabuf inode identity and the cuMem* allocation doors moved to todos.txt. The status below is historical.

- **Created:** 2026-08-07 01:30 +03
- **Status:** IN PROGRESS. Items 1, 2 and 3 are done and measured; see each
  section for what the measurement said, including where this plan's own
  reasoning turned out to be backwards.

## Where things stand

Board green, `--quick`: 7 PASS, 4 SKIP, **no xfail markers anywhere**. Two
consecutive boards (44372679, 44372782) agree on sends (26880, 8 ranks) and on
504 `device_memory` regions, so the new capture is reproducible and not jitter.

What the last stretch actually bought, in order of importance:

1. **Contexts opened through unhooked doors are now instrumented.** This was
   never really about the 33 closes. Object-ops hooks live inside an
   `ibv_context`, so an uninstrumented context is blind to `post_send` and
   `poll_cq` as well — silently. Those sends are now captured.
2. `IbMemRegion.origin` — which door registered a region — so an assertion can
   distinguish "we could not resolve this address" from "this memory has no host
   address" (DM lives on the NIC).
3. Slot exhaustion degrades instead of `abort()`ing the traced application.
4. Providers link with `-Wl,--no-undefined`.

## 1. Close the residual window — DONE, and the ordering argument was backwards

First-sight install is a **floor, not a guarantee**: a context is instrumented at
its first preload call, so object-ops before that are missed. This plan argued
the window was narrow on API ordering — a send needs a QP, a QP needs a PD, both
preload doors — and flagged `poll_cq` on a CQ created before any PD as the one
genuine exposure. Both halves were measured. The exposure is real and the
"narrow" was wrong in the direction that mattered.

**Job 44396249** (`tests/hpc/probes/ctxinstall`, case 02's configuration,
`PCOMM_TRACE_CTX_INSTALL=1`): 73 contexts per rank, identically on all 8 ranks.
**25 installed at `ibv_open_device`. 48 installed at `ibv_alloc_pd`** — so two
thirds of every rank's contexts were opened through a door this provider does not
interpose, and ran uninstrumented until their first PD.

`ibv_create_cq` is now hooked for coverage — no callback, no record, just
`ensure_installed`. It is the one door an object-op can reach a context through
before a PD exists, because a CQ is created from a *context* and everything else
we hook needs a PD first. **Job 44396301** re-ran the probe: those same 48
contexts per rank now install at `door=create_cq`. UCX creates the CQ *before*
the PD, which is the reverse of what the ordering argument assumed.

The capture did not change — job 44396323's goldens are identical. So the window
was real, it is now closed, and nothing was in fact lost through it in case 02.
Those are three separate facts and the board only settles the third.

`mlx5dv_open_device` is **not** worth interposing on the strength of this. The
door it would replace is now `create_cq`, which fires for the same contexts and
works under `providers: ['ib']` where the mlx5dv interposer is not even loaded.

## 2. Audit the other providers — DONE, the premise was false

This plan said "the mlx5dv provider installs object-ops the same way, and nothing
has checked whether IT sees every context". It does not. `mlx5_event_source.hpp`
says so in its header comment and the code agrees: **there are no
`DEFINE_OBJECT_OPS_HOOK`s in the mlx5dv provider at all**, and the only `install(`
in it is `dmabuf::install()`. Every mlx5dv hook is a preload interposer, which is
context-independent by construction, so the bug class cannot occur there.

That does not make cases 09–11 covered — they SKIP for hardware reasons
(`PeerMappingOverride=1`, `nvidia_peermem`) and that path is still unproven on any
board here. But it is unproven for want of hardware, not for want of this audit.

## 3. Shared `MemObject` identity — DONE

`gpumem`'s `AllocRegistry::add` minted a dense local counter while ibverbs and
mlx5dv interned the buffer's base address, so `MemObject.id` was provider-local
while its name said otherwise — and since intern handles are dense 1-based
`uint32` too, gpumem's object 3 and an interned pointer handle 3 were the same
number naming different buffers.

`on_alloc` now interns the base address in `InternDomain::Pointer` like the other
two. The intern table lives in the Tracer singleton inside
`libpcomm_engine.so` — one per process, shared by every provider — so the same
base address yields the same handle across providers by construction. A
`GpuAccess` can now resolve to a region ibverbs registered.

`pcomm-read` still merges `mem_object` by `(rank, base, size)` on purpose: the
range is the fact every emitter agrees on *without agreeing on a convention*, so
the merge survives a fourth emitter that interns something else. Joining on the
id is a gain for queries, not a reason to move the merge onto it. Board 44396615
green; nothing joins on the id in any assertion (case 07 already joins on
base+size).

## 4. Smaller — one done, the rest still open

- **Case 06's `ib_entities` count — done, and it generalised.** The reason the
  count was excluded from the golden ("dereg emits no close record yet, so it
  tracks allocator churn") no longer holds. On job 44396323 opens and closes are
  **exactly** equal for every entity type on every case that has IB entities:
  pingpong 2/2 regions and 2/2 QPs, send_bw the same, NCCL 1518/1518 and
  816/816, coexist 716/716 and 432/432, multinode 1468/1468 and 928/928. That is
  a much stronger invariant than case 06's rank symmetry and it is not
  case-specific, so it went into `suite/asserts/entity_close_accounted.sql` as
  the mirror clause: an unmatched *close* names a missing creation door, an
  unmatched *open* names a missing destruction door. Case 06's absolute region
  count stays out of the golden — it is still allocator-dependent, and this
  assertion is what makes it unnecessary.
- The dmabuf **inode** identity idea (fd reuse hazard) — documented, not measured.
- Allocation tracking through the measured CUDA door list (`cuMemAlloc*`,
  `cuMemFree*`, `cuMemMap`, `cuMemRelease`).

## Constraints carried forward

- **When several symbol-level hypotheses die in a row, the question is probably
  not at the symbol level.** Six died here; the answer was about which object an
  MR belonged to. `nm`, `readelf`, `LD_DEBUG` and the dlsym trace cannot express
  that, so they kept answering confidently. Following one `ibv_mr*` broke it open
  in one job.
- **Any set keyed on a vendor pointer needs a lifecycle.** Omitting the
  `ibv_close_device` half cost case 02 two of eight ranks, silently, because the
  allocator reuses addresses.
- **Read the per-rank numbers, not the diff's shape.** A golden diff was read
  backwards here and reported as recovered capture when it was lost capture.
- **An argument from API ordering is a hypothesis, not a measurement.** This plan
  reasoned that nothing could reach a context before its first PD, and item 1
  measured UCX creating the CQ first on 48 of 73 contexts per rank. The counter
  that settled it was two lines.
- **A tag defined is not a tag registered.** Adding a `DEFINE_PRELOAD_HOOK`
  without adding the tag to `IbverbsEventSource`'s list compiles until the first
  `invoke_preload` names it, and then fails as a page of `std::get` tuple errors
  that say nothing about the actual mistake.
