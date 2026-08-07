# PLAN — what is next, now that the context-coverage gap is closed

- **Created:** 2026-08-07 01:30 +03
- **Status:** TODO. Written after the close-accounting hunt ended (see
  `PLAN-20260806-2320-dlvsym-interposer-not-observed-done`), to say what that
  hunt left behind and what is worth doing next, in order.

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

## 1. Close the residual window (highest value)

First-sight install is a **floor, not a guarantee**: a context is instrumented at
its first preload call, so object-ops before that are missed. API ordering makes
this narrow — a send needs a QP, a QP needs a PD, both are preload doors — but
`poll_cq` on a CQ created before any PD is genuinely exposed.

Two steps, in this order:

- **Measure it.** Record which door first saw each context, and how many
  object-ops calls (if any) preceded installation. If the answer is zero on every
  case, the window is theoretical and should be documented as measured rather
  than argued. This is one counter and one line, and it settles it.
- **Then** interpose `mlx5dv_open_device` in the mlx5dv provider, so the common
  case instruments at open time. Note this cannot help cases 02/06/07 — they run
  `providers: ['ib']`, so the mlx5dv interposer is not loaded — which is exactly
  why first-sight install had to be the general mechanism and this is only an
  optimisation.

## 2. Audit the other providers for the same defect

The bug class is "per-object instrumentation installed at one door". The mlx5dv
provider installs object-ops the same way, and nothing has checked whether IT
sees every context. Cases 09-11 are SKIPPED on MN5 (no `PeerMappingOverride=1`,
no `nvidia_peermem`), so that path is unproven on any board here — a green board
covers less than it looks.

## 3. Shared `MemObject` identity

Still open from the memory-attribution plan: gpumem mints its own dense counter
in `AllocRegistry::add` while ibverbs/mlx5dv intern on the base address, so the
`id` field is provider-local and cannot be joined on. One line in gpumem makes
the identity shared, and then a `GpuAccess` can resolve to a region ibverbs
registered — impossible today.

## 4. Smaller, still open

- The dmabuf **inode** identity idea (fd reuse hazard) — documented, not measured.
- Case 06's `ib_entities` count, worth revisiting now that closes exist and are
  complete.
- Allocation tracking through the measured CUDA door list (`cuMemAlloc*`,
  `cuMemFree*`, `cuMemMap`, `cuMemRelease`).
- Case 05 runs only in a non-`--quick` board; a full run was started after the
  markers were dropped. If it is red, that is the first thing to look at.

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
