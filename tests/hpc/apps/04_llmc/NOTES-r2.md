# 04_llmc round 2 — environment traps, on top of NOTES.md

Round 1's `NOTES.md` still applies in full (CUDA-13 patch, cublas in math_libs,
OpenMPI in /usr/local, `-b 2` for the val split). These are the ones that are new
because round 2 adds `-p kernel` and the derived layer.

## The build you want on du04

`/DATA/missa18/pcomm/build-duck` has DuckDB but **no providers**, and round 1's
`~/pcomm-llmc/build` has the nccl provider but no DuckDB. Neither can do this
study alone. `/DATA/missa18/build-llmc-r2` is configured with all three:

```bash
cmake -B /DATA/missa18/build-llmc-r2 -S /DATA/missa18/pcomm -DCMAKE_BUILD_TYPE=Release \
      -DPCOMM_BUILD_TESTS=OFF -DPCOMM_DUCKDB=ON -DPCOMM_PROVIDER_NCCL=ON -DPCOMM_CUPTI=ON \
      -DCUDA_RT_INCLUDE_DIR=/usr/local/cuda/include -DNCCL_INCLUDE_DIR=/usr/include
```

`-DPCOMM_CUPTI=ON` is what gives `-p kernel`; it builds to
`gpu-backends/cupti/libcupti_trace.so`, **not** under `providers/`.

## `PCOMM_PROVIDER_PATH` takes a colon-separated list

Because of that split, a two-provider run needs both directories:

```bash
export PCOMM_PROVIDER_PATH="$BUILD/providers/nccl:$BUILD/gpu-backends/cupti"
```

With only the first, `pcomm run -p nccl,kernel` prints
`kernel provider library not found` and **exits 1** (it does not silently produce
an empty capture — that failure mode is NVBit's, not CUPTI's). Under `mpirun`,
forward it with `-x PCOMM_PROVIDER_PATH -x LD_LIBRARY_PATH`.

## The derived layer is Parquet-only

`pcomm link` on a `--csv` export refuses with
`no raw .parquet tables in <dir> — run 'pcomm export --parquet' first`. So:

```bash
pcomm export OUT --parquet OUT.pq && pcomm link OUT.pq      # derived tables
pcomm export OUT --csv     OUT.csv                          # decoded enums + names
```

You need both. Parquet has `comm_edges`/`rank_identity`/`link_health` but leaves
`op`, `datatype` and `name` as numbers and intern handles; CSV decodes all three
but has no derived tables. Assertions that must run on either should compare
`CAST(op AS VARCHAR) IN ('3','reduce_scatter')` and resolve names through
`COALESCE(<intern_text lookup>, CAST(name AS VARCHAR))` — `intern_text` exists
only in the Parquet export.

## `pcomm sql` rejects a trailing semicolon

It wraps the query in `COPY (...) TO '<tmp>.csv'`, so `... ORDER BY x;` is a
`Parser Error`. Every assertion in `tests/hpc/cases/` ends with one. `check-r2.sh`
strips both the trailing `;` and the leading comment block (that one is the
documented "first token must not be a comment" wart).

## Table existence lives in `duckdb_views()`, not `duckdb_tables()`

An export is a set of views over the parquet files; `duckdb_tables()` returns
zero rows. `check-r2.sh` implements the suite's `-- requires:` this way.

## Counting anything `DISTINCT` across ranks is a trap

Intern handles are per rank. `count(DISTINCT stream) FROM nccl_collective` reads
**3** on a workload with exactly two streams per rank, because rank 0 interned
`main_stream` as 77 and ranks 1-3 as 149. Group by rank first, always.

## CUPTI cost and reporting

`[pcomm/cupti] kernels=1251 dropped=0 armed_by=constructor` on stderr per rank —
read it, it is the only place the provider says whether it lost anything. Cost
measured at ~2 % of step time on 1251 kernels/rank; nothing like NVBit's ~440 s
module lift. `-p nccl,kernel` runs as **one pass** (`2 providers -> 1 pass`), so
the intent-to-execution join is within a single execution.

## Getting a second opinion out of NCCL itself

`mpirun -x NCCL_DEBUG=INFO -x NCCL_DEBUG_SUBSYS=TUNING` prints one line per
collective with its size, algorithm and protocol. It is the only independent
oracle available for anything below the NCCL API, and it is what showed that a
fused group runs three protocols under one kernel name (REPORT-r2.md §5). Cheap:
grep for `-> Algo`.
