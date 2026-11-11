# NOTES — traps paid for in advance, so the next agent does not pay again

No capture exists yet: MN5 refused SSH (`connect to host alogin2.bsc.es port 22:
Operation timed out`) for the entire window this study was written in, and the
brief forbids submitting without being told the slot is yours. Everything below
is from the offline work — reading OSU's source, deriving the oracle, and
writing the scripts — and all of it is checkable without a queue slot.

## Getting OSU onto MN5

MN5 has **no outbound network**. Fetch on a networked machine and rsync:

```bash
curl -LO https://mvapich.cse.ohio-state.edu/download/mvapich/osu-micro-benchmarks-7.5.tar.gz
rsync -av osu-micro-benchmarks-7.5.tar.gz koc296797@alogin2.bsc.es:~/
bash -lc 'bash ~/proj/comshu/tests/hpc/apps/05_osu_onesided/build_osu.sh'   # login shell is tcsh
```

`build_osu.sh` checks the tarball's sha256 and **refuses a different one**. That
is not ceremony: every number in `meta.json` was derived from that tree, and OSU
changes its defaults between minor releases — `WINDOW_SIZE_LARGE` alone has
moved twice in the 5.x–7.x line.

Autotools, `CC=mpicc CXX=mpicxx`, and 7.x installs the one-sided binaries at
`$PREFIX/libexec/osu-micro-benchmarks/mpi/one-sided/`, not `$PREFIX/bin`.

## Four OSU behaviours that will bite anyone deriving an oracle

1. **`-i N` sets `iterations` *and* `iterations_large`; `-x N` sets `skip` *and*
   `skip_large`** (`osu_util.c:360`, `:326`). Without both, the size loop
   silently switches to the large-message counts the moment `size > 8192`
   (`osu_put_bw.c:296`) and the per-size row counts change mid-sweep. Any
   assertion that assumed one number would fail on half the sizes for a reason
   that has nothing to do with the tracer.
2. **`-W` (window size, i.e. operations per iteration) exists only for the `BW`
   subtype.** The option string is built at `osu_util.c:593` and `W:` is
   appended only `if (options.subtype == BW)`. `osu_acc_latency` and
   `osu_fop_latency` will reject it.
3. **`-T MPI_INT` exists only where `show_validation` is set** — the same
   builder appends `cT:` only then, and only `osu_acc_latency`,
   `osu_fop_latency`, `osu_cas_latency` and `osu_get_acc_latency` set it before
   `process_options`. `osu_put_bw` is `MPI_CHAR` and says so at `:72`.
4. **`osu_fop_latency` has no size loop at all.** It allocates
   `options.max_message_size` once and issues one single-element
   `MPI_Fetch_and_op` per iteration (`:357`). `-m` there controls the *window*,
   not the operation. Reading `-m 4:4` as "4-byte messages, swept" would be
   wrong in a way that produces a plausible number.

## Why the sweep starts at 512 B and not at 1 B

MPI's own window synchronisation moves 8- and 64-bit words over the same doors
as the workload. A sweep that includes 8 B cannot separate the two by size, and
size is the only discriminator available: the `op`/`door` vocabulary is not
published anywhere a black-box user may read (QUESTIONS.md #1). Starting above
the control traffic is what makes "count the rows whose size is one of mine" an
exact filter instead of an estimate.

## Toolchain, inherited from 01_gromacs and still true

Build the benchmark with the **same** `mpicc` the job will launch with.
`comshu_load_runtime` puts `nvidia-hpc-sdk/25.7` on PATH (HPC-X 2.22.1); a
binary built against a different HPC-X and launched by this one mixes two Open
MPI releases in one process, and the UCX provider — `LD_PRELOAD`ed, so resolved
first — then wins the `libucp.so.0` SONAME for a `pml_ucx` that was built
against a different UCX. `build_osu.sh` and `job.sbatch` both print
`ldd | grep -E 'libmpi|libucp|libucs'` so a mismatch is visible in the log
rather than in the results.

## `--mca osc ucx` is set twice, on purpose

Once as an `mpirun` flag and once as `OMPI_MCA_osc` in the environment. If the
launch ever has to move to `srun --mpi=pmix` — which the MN5 module banner and a
memory entry disagree about — the flag disappears and the environment variable
does not. Getting this wrong does not produce an error: it produces an empty
`ucx_rma`, which is indistinguishable from a tracer that saw nothing.

## The assertions were tested before the job existed

All seventeen `.sql` files were run against a hand-built SQLite mock of the record
schema: once on a clean capture (all silent) and once per defect they name
(each fires). That is how two of them were found to pass vacuously on an *empty*
table — `sum()` over no rows is NULL, and `NULL < 3174400` is NULL, not true —
which is why `rma_sweep_is_complete` and `the_wire_carried_the_bytes` wrap their
sums in `coalesce(..., 0)`. An assertion that goes quiet on the emptiest
possible capture is the worst kind there is, and a mock is enough to find it.
