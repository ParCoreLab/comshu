# UCX API coverage probes

Stage-0 instruments for
`plans/PLAN-20260815-1540-ucx-provider-and-attribution-in-progress.md`.
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

`nesting.sbatch` adds the next question without adding records: it runs the same
workload under pcomm's ibverbs provider with `PCOMM_TRACE_UCX_NESTING=1`. The
UCX preload library exports the innermost operation scope, and the verbs
`post_send` callback reports whether each WR was posted beneath tagged send,
tagged receive, worker progress, another UCP call, or no public UCP call. This
diagnostic is inert unless explicitly enabled.
