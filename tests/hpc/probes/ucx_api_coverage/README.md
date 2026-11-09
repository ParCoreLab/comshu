# UCX API coverage probes

Stage-0 instruments for
`plans/PLAN-20260815-1540-ucx-provider-and-attribution-done.md`.
They measure the public UCP surface before those calls become permanent pcomm
records.

`job.sbatch` builds `ucx_calls.cpp` as a disposable preload counter and runs
`mpi_calls.c` on exactly two MN5 nodes. The summary reports call counts and the
sum of each UCP call's raw `count` argument. That argument is intentionally not
called bytes: its interpretation depends on the datatype in
`ucp_request_param_t`.

Measured twice on UCX 1.20.0: jobs 44629699 and 44629730. Lifecycle and tagged
send/receive counts matched exactly; worker-progress counts varied substantially
and are telemetry, not a correctness oracle. See the plan for the table.

This probe is not a provider and writes no capture. Its purpose is to make an
empty future record table distinguishable from an API the installed MPI never
called.

## The nesting arm: ANSWERED, and retired

`nesting.sbatch` asked whether a verbs work request could be attributed to the
UCP call it was posted beneath, by running the same workload under pcomm's
ibverbs provider with `PCOMM_TRACE_UCX_NESTING=1`: the preload library exported
the innermost operation scope and the `post_send` callback bucketed each WR by
it.

**Job 44635003 answered it: no, not on this transport.** Both ranks reported
`summary outside=0` — not "every WR fell outside a UCP call" but *no WR was
posted at all*. The capture corroborates it: verbs setup, QP/CQ, memory and
`ib_post_recv` streams are all present, and `ib_post_send` (type 110) is absent
entirely. The recv side firing while the send side stays empty is what rules out
"the provider was not loaded" and leaves only the real explanation — MN5's UCX
mlx5 transport posts its send WQEs below the public verbs door.

So `ibv_post_send` is the wrong observation point here, and the bridge that
asked the question is **gone**: the `trace_ucx_nesting` / `dump_ucx_nesting`
instrument has been removed from `providers/ibverbs/ibverbs_provider.cpp`, and
`nesting.sbatch` with it. A gated diagnostic that can only ever print zeros is
the kind of door held open for a record nobody writes that this tree has spent
real time paying down.

`ucx_calls.cpp` keeps `pcomm_ucx_api_probe_current` — the scope export costs
nothing, is used by no provider, and is what the next arm would need once an
observable host-side mlx5 send/doorbell door is identified. See the plan.
