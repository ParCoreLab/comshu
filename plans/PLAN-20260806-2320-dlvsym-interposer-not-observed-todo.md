# PLAN — our `dlvsym` interposer appears not to run, and every explanation so far is dead

- **Created:** 2026-08-06 23:20 +03
- **Status:** TODO — five hypotheses eliminated by measurement. The next step is
  one line of instrumentation, deliberately not a sixth hypothesis.
- **Blocks:** the 33 unmatched `ib_mem_region` closes on cases 02 and 06
  (`entity_close_accounted`, marked `expect_fail` with evidence).
- **Related:** `PLAN-20260806-2140-dmabuf-registered-regions` (the search this
  came out of).

## The question, stated precisely

It is **not** "should we capture `dlvsym`". We already do, and that was verified
twice rather than assumed:

- `libpcomm_engine.so` exports `dlvsym` GLOBAL, alongside `dlsym`;
- the interposer body calls `on_symbol_load` on both the `RTLD_NEXT` and the
  ordinary-handle path, identically to `dlsym`.

The question is why, in job 44367535, **no `ibv_*` lookup was ever traced**,
on a run that demonstrably used InfiniBand.

## What is established

| fact | how |
|---|---|
| NCCL resolves `ibv_*` via `dlvsym`, never `dlsym` | source: `nccl src/misc/ibvsymbols.cc`, dynamic branch |
| the versions it asks for all exist | `readelf` on `/lib64/libibverbs.so.1`: `ibv_reg_mr@@IBVERBS_1.1`, `ibv_reg_mr_iova2@@IBVERBS_1.8`, `ibv_reg_dmabuf_mr@@IBVERBS_1.12` — so the lookups SUCCEED and return non-null |
| our `dlvsym` is exported and routes to `on_symbol_load` | `readelf` on the engine + the interposer body |
| `RegisteredSymbols` is shared engine↔provider | the provider's symbol is `UND`, binding to the engine's definition |
| the traced run really used IB | 8 ranks × `100.pcs`, 26880 sends, `mpirun exit=0` |
| **zero** `ibv_*` lines were traced | job 44367535 output: 24 lines, all `cu*` |

## Hypotheses eliminated

1. **A dlsym-hidden symbol** — the diagnostic was built for this; it found only
   `cu*`.
2. **NCCL links ibverbs directly** — no UND `ibv_*` in the SDK's `libnccl.so.2`.
3. **Split registry** — provider's `RegisteredSymbols` is UND, so it binds to the
   engine's; the interposer does see provider registrations.
4. **Wrong NCCL** — I first checked `/lib64/libnccl.so.2`, which case 02 does not
   load. Rechecked against the HPC-SDK build; conclusion unchanged.
5. **Symbol versioning** — the tempting one, and symmetric evidence kills it:
   libnccl references **both** `dlsym@GLIBC_2.2.5` and `dlvsym@GLIBC_2.2.5`, and
   the engine defines **both** unversioned. Whatever versioning does, it does the
   same thing to each — yet `dlsym` traced and `dlvsym` did not.

Five dead. That is the same count case 08 reached before the answer turned out
to be a category error rather than a wrong guess, and the resemblance is the
reason for the next step.

## Step 1 — one line, at the very top

A print as the **first statement** of the `dlvsym` interposer, before every
guard, gated on `PCOMM_TRACE_DLSYM`:

```c
extern "C" void* dlvsym(void* handle, const char* symbol, const char* version) {
    trace_entry("dlvsym", symbol, version);   // FIRST. Before init(), before anything.
    ...
```

It answers one question with no interpretation: **is our `dlvsym` entered at
all?**

- **Entered, with `ibv_*` names** — then the loss is downstream, inside
  `on_symbol_load`, and the early returns are the place to look. Note
  `on_symbol_load` bails when `real_symbol` is null *before* the trace, which is
  the third time in this project a diagnostic has been blinded by a guard placed
  for an unrelated reason.
- **Not entered** — then the interposition itself is not happening for
  `dlvsym`, despite the symbol being exported, and the question becomes a
  link/resolution one. `LD_DEBUG=bindings` on `dlvsym` specifically would then be
  the right follow-up, being a question about *resolution*, which is what that
  tool actually answers.

Do the same for `dlsym` in the same build. Its lines are the positive control:
if neither prints, the instrument is broken rather than the finding real — and
this probe already mis-stated its own control once.

## Constraints

- **Do not add a sixth hypothesis before step 1 runs.** Five have died; the cost
  of each has been a job and a round of reasoning, and the measurement is cheaper
  than any of them.
- **Do not "add dlvsym support".** It exists. Any change here is a fix to
  something already built, not a new capability, and framing it as new work would
  hide whatever the real defect is.
- Whatever the answer, it likely affects `dlsym` too — the two share
  `on_symbol_load` — so re-verify both boards, not just the failing case.

## RESOLVED (2026-08-06, jobs 44368260 + 44368331)

**Our `dlvsym` works.** Job 44368260: it is entered on all 8 ranks (first symbol
`pipe2`), and **never once with an `ibv_*` name** — and neither is `dlsym`.
Nothing in that process resolves ibverbs dynamically at all. So the premise that
started this hunt was wrong: NCCL's `dlvsym`-based `ibvsymbols.cc` is not the
path being taken here, and the sixth hypothesis would have been wrong too.

The question was never "which symbol". It was **"which caller"**, which no symbol
tool and no resolution trace can answer. `PCOMM_TRACE_MR_CALLERS=1` (dladdr on
`__builtin_return_address(0)` in the three MR interposers) answered it in one job:

| door | callers |
|---|---|
| `reg_mr_iova2` | `libnccl-net.so`, `libuct_ib.so.0`, `hmca_mcast_vmc.so` |
| `reg_dmabuf_mr` | `libnccl-net.so`, `libuct_ib.so.0` |
| `dereg_mr` | the above **plus `libuct_ib_mlx5.so.0`** |

**`libuct_ib_mlx5.so.0` is DEREG-ONLY.** It is UCX's mlx5 transport, and it
references `ibv_reg_mr@IBVERBS_1.1` — the legacy symbol
`ibverbs_provider.cpp` deliberately does **not** interpose — plus
`mlx5dv_devx_umem_reg`, which case 02 cannot see because it runs
`providers: ['ib']`.

Two corrections to things this project believed:

1. **The traffic was never pure NCCL.** Case 02 has been read as NCCL throughout;
   NCCL reaches IB through `libnccl-net.so` (the HPCX rdma-sharp plugin, not
   libnccl itself), and **UCX and hcoll register memory in the same process**.
   The 33 unmatched closes never had to come from the library being watched.
2. **Legacy `ibv_reg_mr` does not always route through `ibv_reg_mr_iova2`.** The
   no-legacy-hook decision rests on a case-01 measurement that it does. No
   `libibverbs.so.1` appears as a caller of `reg_mr_iova2` in this run, so here
   it did not. The comment at `ib_hooks.hpp:54` states the double-count as
   general; it is conditional, and the condition is unknown.

### Round 2 (2026-08-07): the legacy hook is correct and changed nothing

`ibv_reg_mr` is now hooked, with a thread-local depth guard so the nested
`iova2` call cannot double-count. It is the right change and it did **not**
close the gap:

- **Case 02 is byte-identical across it** — 1013 regions, 2333 closes, the same
  11 reported rows. On this rdma-core the legacy symbol *does* route through
  `iova2`, so those registrations were already captured; the guard just moved
  which hook records them.
- **Job 44368667**: legacy `reg_mr` is called only by hcoll
  (`hmca_mcast_vmc.so`). **`libuct_ib_mlx5.so.0` is still dereg-only** — it
  references `ibv_reg_mr@IBVERBS_1.1` but does not call it here. Referencing a
  symbol is not calling it, which is the case-08 lesson arriving for the third
  time, now on the ELF side.

So the hook buys robustness against an rdma-core that does not route (UCX's
reference proves that path exists) and buys nothing on this board. Keeping it,
labelled as such, rather than pretending it was the fix.

**A bookkeeping error, caught by the same assertion.** Case 07's `expect_fail`
marker was dropped in c5fda14 because job 44363299's board read `8 PASS + 3
SKIP` — but 07 was PASS *because* the marker made the failure expected; that
job's log says `[xfail] entity_close_accounted` outright. Re-running today's SQL
against that same capture reproduces the failure, which is how this surfaced.
The marker is restored. A green board with an xfail on it is not a green
assertion.

**Next candidate:** `mlx5dv_devx_umem_reg`, which `libuct_ib_mlx5` also
references. Case 02 cannot see it — `providers: ['ib']` means that interposer is
not even loaded — so this needs a devx-enabled run, not another case-02 job.

### Round 3 (2026-08-07): the doors are named, hooked, and not yet firing

Following the individual mr instead of the library (job 44368870) split the
unmatched closes into two clean populations, both `addr=NULL`:

| n | size | closed by |
|---|---|---|
| 355 | 32 B | `uct_ib_dereg_mr` (libuct_ib.so.0) |
| 120 | 2048 B | `uct_rc_mlx5_iface_common_dm_cleanup` (libuct_ib_mlx5.so.0) |

`DM` is **device memory** — the NIC's own. `ibv_reg_dm_mr` and
`ibv_alloc_null_mr` are `static inline` over **`verbs_context` function
pointers** (verbs.h:2807, 2830), so no ELF reference names them and no `dlsym`
resolves them. That is the `ibv_create_qp_ex` shape exactly. Every instrument
that answers questions about symbols was always going to report nothing here,
which is why six hypotheses died before one that followed the object.

**Shipped:** both doors hooked as object-ops on their `verbs_context` slots
(generalising `create_qp_ex_slot`), `IbMemRegion.origin` through all five schema
artifacts, `registration_has_object` exempting memoryless regions by origin
rather than by `base=0`. Board is GREEN: 7 PASS, 4 SKIP, and the fixture capture
was refreshed because the stride change broke it loudly, as designed.

**Not yet working, and this is the open item.** Case 02 records **zero**
`device_memory` or `null_mr` regions — origins are 974 `reg_mr` + 40 `dmabuf` —
and no "no usable verbs_context" warning was printed either. So the slots were
considered usable and the trampolines were never dispatched through, on the same
workload where the probe sees 474 memoryless deregs. Unmatched closes are
unchanged (283 → 309 on case 02, noise).

Next measurement, and NOT another hypothesis: `PCOMM_TRACE_HOOKS=1` on the probe
says directly whether the two new trampolines are entered. Entered-and-recording
-nothing and never-entered are different bugs, and the entry trace already
exists for exactly this question — it is what settled case 08.

### Round 4 (2026-08-07): the trampolines are never entered

Job 44369381, `PCOMM_TRACE_HOOKS=1`: **neither `reg_dm_mr` nor `alloc_null_mr`
is entered.** Not once, on the workload that deregisters 474 memoryless MRs. The
trace itself is healthy — 11 doors report, including both existing object-ops
hooks (`post_send`, `poll_cq`), so the object-ops mechanism works.

So the DM/null-MR diagnosis was **wrong, or at least not the whole story**: those
doors exist, have the shape I described, and are not what UCX calls. The hooks
are correct and inert, exactly like the legacy `ibv_reg_mr` hook one round
earlier. Two rounds of "the right-looking door that nothing goes through" is a
pattern worth naming: an unhooked door that *could* explain the evidence is not
the door that *does*, and only entry can tell them apart.

**The control was weak and that should be recorded.** `create_qp_ex` also reports
0 here, so it did not prove the instrument. It is the right control on case 08
(perftest), not on this workload, where creation goes through `ibv_create_qp` —
which does report. The 11 reporting doors are what makes the reading safe; had
the trace printed nothing, this result would have meant nothing.

**Untested lead, flagged as a hypothesis and not a finding:**
`libuct_ib_mlx5.so.0` references **`mlx5dv_open_device`**, and this provider
installs object-ops only from `ibv_open_device`. A context opened the other way
is never patched, so *every* object-ops hook is blind on it — `post_send` and
`poll_cq` included, which would be a much larger gap than the closes. That is
checkable directly: trace entry to `mlx5dv_open_device`, and compare the
`ibv_context*` of an unmatched mr against the contexts we installed into. Note
it does not explain the 355 closes from `libuct_ib.so.0`, which references
`ibv_open_device` — so it is at most half an answer, and possibly none of it.

### What is still open

Hooking legacy `ibv_reg_mr` is the fix, but not unconditionally: where the
routing *does* happen it double-counts, which is what cases 01/06 measured. A
thread-local depth flag makes it exact — record in the legacy hook only when the
inner `iova2` hook did not fire for the same call — and costs nothing.

Do that before assuming either behaviour is universal. And re-run case 06/07 (a
`devx` board) to see whether `mlx5dv_devx_umem_reg` is a second dereg-only door.

## Why this matters beyond the 33 closes

If our `dlvsym` is not interposing, then **every symbol any application resolves
through `dlvsym` bypasses pcomm entirely** — silently, with no error and no
empty table, because the application gets the real function and simply works.
NCCL resolves its whole ibverbs surface that way. That is a capture-completeness
question, not a bookkeeping one, and the 33 unmatched closes would be the only
visible symptom of it.
