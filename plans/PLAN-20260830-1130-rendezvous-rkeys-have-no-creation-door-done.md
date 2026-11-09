# Rendezvous rkeys are destroyed through a door we hook and created through one we don't

> **Closed 2026-11-09 (backlog triage).** ucp_ep_rkey_unpack_internal is hooked with a runtime version gate (providers/ucx/ucx_provider.cpp); case 21 passes. The status below is historical.

**Status:** todo. Case 21 is RED on `entity_close_accounted` because of this, and
it is red for a true reason — pcomm really does not record these keys.

## What was measured

Job 45198862, case 21 (`ucx_deep`, 2 nodes x 1 rank, HPC-X 2.22.1 / UCX 1.18.0),
commit c862c06:

| fact | rank 0 | rank 1 |
|---|---|---|
| `ucx_rkey` rows (opens) | 4 | 4 |
| `entity_close` opener=626 | 43 | 42 |
| of those, matching an open | 4 | 4 |
| **unmatched closes** | **39** | **38** |
| rendezvous advertisements (`uct_send.op=13`) | 20 | 20 |

The four recorded rkeys each close exactly once — correct. The unmatched closes
land on two interned handles per rank, destroyed 19 and 20 times each: one
`ucp_rkey_h` heap address, freed and reallocated per message.

`iters` is 20, and the sweep's 65536 B and 4194304 B sizes are both above
`rndv_thresh=32768`. So unmatched destroys ≈ 2 per rendezvous, on both ranks.

Case 22 (`ucx_rma`, MPI one-sided, no large rendezvous) has **zero** unmatched
626 closes and passes the same clause in its own `rkey_lifetimes_balance.sql`.
That is the control: the population appears exactly where rendezvous does.

## The door

`ucp_ep_rkey_unpack_internal`, and `src/ucp/rndv/proto_rndv.c` calls it directly
in the rendezvous protocol path. It is **exported** from `libucp.so` as a global
text symbol on the shipped build, so it is interposable by the same mechanism as
every other door in this provider:

```
000000000004ade0 T ucp_ep_rkey_unpack_internal
000000000004b1f0 T ucp_ep_rkey_unpack           <- the public one we already hook
000000000004b450 T ucp_rkey_destroy             <- what fires today, unmatched
```

So: UCX creates these keys through an internal entry point and destroys them
through the *public* `ucp_rkey_destroy` that we interpose. Same shape as the
endpoint-teardown finding in `ucx_hooks.hpp` — UCX reaching its own public
symbols from inside — arriving from the creation side instead.

## Why this is not a one-line hook

**The signature changed after the version we run.** Verified against the tags,
not inferred:

```c
/* v1.18.0 — what HPC-X 2.22.1 ships on MN5 — SIX parameters */
ucs_status_t ucp_ep_rkey_unpack_internal(ucp_ep_h ep, const void *buffer,
                                         size_t length,
                                         ucp_md_map_t unpack_md_map,
                                         ucp_md_map_t skip_md_map,
                                         ucp_rkey_h *rkey_p);

/* master — SEVEN, sys_device inserted before the out-parameter */
ucs_status_t ucp_ep_rkey_unpack_internal(ucp_ep_h ep, const void *buffer,
                                         size_t length,
                                         ucp_md_map_t unpack_md_map,
                                         ucp_md_map_t skip_md_map,
                                         ucs_sys_device_t sys_device,
                                         ucp_rkey_h *rkey_p);
```

`rkey_p` — the only argument we actually want — is arg 6 on the shipped build
and arg 7 on master. Under x86-64 SysV that is `r9` versus the first stack slot.
A hook written against the wrong one does not fail to build and does not crash;
it reads an unrelated qword and interns it as an rkey handle. It would produce a
table that looks populated and is fabricated, which is worse than the hole it
closes.

This is also the first door in this provider that is **not** part of UCX's
public API. Every other interposed symbol is contractually stable; this one has
already moved once between the version we run and the version upstream.

## What to do

1. Gate on the runtime version, via the public `ucp_get_version()` — not on a
   build-time header, since the provider is built once and dlopened against
   whatever libucp the application already loaded.
2. Pin the exact upstream commit that added `sys_device`, and turn the cutoff
   into a named constant with that commit in the comment. Do not guess the
   boundary from the version we happen to have looked at.
3. Refuse to hook — loudly, not silently — on any version outside the range the
   signature has been verified for. An unhooked door leaves case 21 red and
   honest; a mis-hooked one makes it green and wrong.
4. Only then re-run 21 and 22 together. 22 is the control that must stay green.

**No schema change.** These rows are `UcxRkey` (626), which already exists, so
`kSchemaHash` is untouched and existing captures stay readable.

## Do not "fix" this by exempting 626

`tests/hpc/suite/asserts/entity_close_accounted.sql` says it in its own head
comment — an unmatched close is the cheapest detector for a missing creation
door, and the temptation when one fires is to filter it out. This one fired and
the door was found in about ten minutes, which is the argument for the
assertion, not against it. The hcoll precedent (`expect_fail` on one capture) is
not applicable: that was a foreign library leaking its own buffers and nothing
we could hook. This is our door and we can reach it.
