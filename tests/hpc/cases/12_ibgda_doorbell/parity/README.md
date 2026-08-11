# SigmaTrace parity — how to reproduce

Design and full findings: `plans/PLAN-20260810-0102-sigmatrace-parity-done.md`.
This directory holds the one piece of code that has to live somewhere: a dumper
that turns SigmaTrace's binary event files into the same TSV shape the pcomm
side produces, so the comparison is TSV-vs-TSV and neither tool's analysis code
sits in the loop.

`sigma_dump.cpp` is a copy of `SigmaTrace/Tools/sigma_dump.cpp`. It is kept here
because it is *our* comparison harness — it must survive a SigmaTrace checkout
being moved, rebuilt or updated.

## Running it

du04 only (`PeerMappingOverride=1`); reach it with
`ssh -J mktips@100.64.0.8 missa18@172.20.31.90`.

**Both runs must set `NVSHMEM_IB_DISABLE_DMABUF=1`.** SigmaTrace cannot trace
the dmabuf path at all — it has no `devx_umem_reg_ex` hook and discovers the GPU
with `cudaPointerGetAttributes`, which cannot work on the null address a dmabuf
registration carries. Without this the profiled process aborts in
`m_mlx5_qp_info.at()`.

```bash
# SigmaTrace.  NOTE: no `--` before the program — with it the app silently
# receives different arguments, i.e. a different workload.
Profiler --enable-doorbell-hijack --load-cupti=0 -f -o <out> \
         ~/nvshmem_putget_sm80 1024 20

clang++-20 -std=c++20 -O2 -I Events -I Profiler -I Profiler/Events \
  -I Modules/Helpers/include Tools/sigma_dump.cpp -o sigma_dump
./sigma_dump <out> > sigma.tsv

# pcomm, same configuration
pcomm run -p mlx5dv --enable-doorbell-hijack -o <out2> -- ~/nvshmem_putget_sm80 1024 20
pcomm export <out2> --csv <out2>.csv
```

## What to compare, and what not to

**Compare the doorbell stream.** `prod_idx` is a per-QP sequence number, not a
clock, so it needs no timing alignment. Compare the multiset of per-QP
sequences, never keyed on `qp_num` — that is driver-assigned and differs per run.

**Do not compare doorbell counts for equality.** They are not repeatable: two
pcomm runs of the identical workload produced 168 and 162 doorbells, because
NVSHMEM batches warp-combined writes and where a batch ends moves with timing.

**Do not treat SigmaTrace's decoded fields as ground truth.** Measured
2026-08-10 it reports 172 GB moved for a workload whose own parameters say
163,840 B. Use the workload as the oracle instead: `elems x 4 B x iters x PEs`.
