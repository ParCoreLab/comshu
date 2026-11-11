# 02_lammps — environment traps

Written so the next agent does not pay for these again. Ordered by how much time
each one cost.

---

## 1. `libucx_trace.so` drags HPC-X's UCX into a process that is using the system UCX — SIGSEGV in `MPI_Init`

**The biggest one.** `-p ucx` against MN5's `openmpi/4.1.5-gcc` crashes before
LAMMPS prints a line:

```
ucp_wireup_select_transport()  .../hpcx-v2.22.1/ucx-b00d4c4b.../src/ucp/wireup/select.c:630
ucp_ep_create()                .../ucp/core/ucp_ep.c:1198
comshu::hook::CompositeHookManager<ucx::Profiler, ...>::invoke_preload<ucx::ep_create_tag, ...>
mca_pml_ucx_add_procs()        .../openmpi-4.1.5/ompi/mca/pml/ucx/pml_ucx.c:448
ompi_mpi_init()                ... signal 11
```

Cause. `libucx_trace.so` carries an **absolute RUNPATH** to the UCX it was built
against:

```
RUNPATH: [$ORIGIN/../../libcomshu:.../build-ucx/libcomshu:
          /apps/ACC/NVIDIA-HPC-SDK/25.7/.../hpcx-2.22.1/ucx/lib:]
```

while this OpenMPI's `mca_pml_ucx.so` links the **system** UCX 1.20.0 in
`/lib64`. Left alone the loader produces a hybrid that cannot work:

```
libucp.so.0 => .../hpcx-2.22.1/ucx/lib/libucp.so.0     <- HPC-X
libuct.so.0 => /lib64/libuct.so.0                      <- system
libucs.so.0 => /lib64/libucs.so.0                      <- system
```

HPC-X's `ucp` calling the system's `uct`/`ucs`. `common.sh`'s `comshu_find_ucx`
comment already warns that build-UCX and run-UCX must match; it anchors to the
UCX under *`mpirun`'s* SDK, which is right for the suite's own HPC-X cases and
wrong for a module-provided MPI.

Fix, one line:

```bash
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/lib64"
```

**Append, never prepend.** New dtags mean *any* position in `LD_LIBRARY_PATH`
beats a RUNPATH, so appending is enough to win. Prepending also shadows
gcc-13.2's `libstdc++` with `/lib64`'s, and every comshu binary then dies with
``version `GLIBCXX_3.4.30' not found`` — which looks like a completely different
bug and cost a second round.

## 2. Do not use `comshu_load_runtime` with a module-provided MPI application

It loads `nvidia-hpc-sdk/25.7`, which puts the SDK's own `mpirun` ahead of the
one the application is linked against (LAMMPS here needs
`/apps/ACC/OPENMPI/4.1.5/GCC`'s `libmpi.so.40`). All comshu actually needs from
it is gcc-13.2's libstdc++, so load that alone:

```bash
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"   # $COMSHU, COMSHU_PROVIDER_PATH
module load gcc/13.2.0                           # NOT comshu_load_runtime
```

## 3. `module purge` breaks `lammps/10Sep2025`

The modulefile does `load("miniforge")` and `execute{cmd="source activate
mace-lammps-deepmd"}`, and needs `bsc/1.0` alive. After a purge the module
"loads" with rc=0 and silently prepends nothing — `which lmp_mpi` finds nothing
and the failure reads like a bad module name. Load the prereqs and then the
module, without purging:

```bash
module load cuda/12.6 openmpi/4.1.5-gcc fftw/3.3.10-gcc-ompi gsl/2.8-gcc mkl
module load lammps/10Sep2025
```

## 4. `comshu sql` rejects a TRAILING SEMICOLON as well as a leading comment

`tests/hpc/apps/README.md` documents the leading-comment wart. There is a second
one from the same cause — the text is wrapped in `COPY (<query>) TO '...'`:

```
Parser Error: syntax error at or near ";"
LINE 6: ORDER BY 1, 2;) TO '/scratch/tmp/comshu-sql-670016.csv' (FORMAT CSV, ...
```

Ordinary `.sql` files end in `;`. Strip both at the runner rather than writing
non-SQL SQL:

```bash
sed '/^[[:space:]]*--/d; /^[[:space:]]*$/d' "$f" | sed '$ s/;[[:space:]]*$//'
```

## 5. `sql_check.sh`'s `params` vocabulary is closed, so an app case cannot use it

Its workload block is validated against a fixed column list — `ranks, nodes,
gpus, iters, msg_bytes, elems, elem_bytes, app_buffer_bytes, regions_per_rank,
umem_binding, gpudirect, rx_depth, colls_per_iter, p2p_per_iter,
groups_per_iter` — and anything else rejects the **whole** meta.json:

```
unknown workload key(s) ['procgrid_x', 'reneighbour_every', 'halo_min_bytes', ...]
02_lammps: bad workload block in meta.json
sql_check_exit=2
```

A real application's oracle is a processor grid and a neighbour-rebuild period,
not `iters` and `msg_bytes`. Widening a shared suite file from an app study is
out of scope (`apps/README.md`: do not edit outside your own case directory), so
this case materialises `params` from the same `meta.json` block itself and runs
the same `asserts/*.sql` through `comshu sql`. See `run_asserts()` in
`job.sbatch`. **Whoever promotes any of these studies to a gate has to decide
whether that column list becomes open.**

## 6. `alogin2` has four H100s — almost none of this needed the queue

```
$ nvidia-smi -L
GPU 0..3: NVIDIA H100
```

Everything except the eight-rank two-node run was done there for free: the
module survey, `lmp -h`, all the `nm`/`strings` work, the managed-memory probe,
a 2-rank `comshu run -p ib,ucx` smoke test that found trap 1 *before* it could
waste an allocation, and every iteration of every SQL query (the captures are
files; `comshu sql`, `export` and `link` need no GPU and no allocation). One
queue slot was spent in total, and the assertions were still being debugged
after it finished. Budget the slot for the *run*, not for the analysis.

## 7. A negative assertion over an empty relation passes, and says nothing

Three of this case's assertions (`halo_peers_grid_adjacent`, `halo_symmetric`,
`no_self_halo`) are "no offending rows" checks over the peer-resolved edge
relation. That relation is empty here (REPORT.md D1), so all three pass while
checking nothing. An earlier `halo_peer_fanout` had the same shape — it grouped
the joined edges and compared the group count, so with zero edges there were
zero groups and the `HAVING` never fired.

The fix that generalises: **drive the aggregate off the rows that must exist**
(`ucx_tag_send`), not off the join result, so a rank that owes three peers and
resolves none produces a row instead of an absence. And ship an explicit
resolvability guard (`peer_rank_resolvable`) beside any family of checks that
depends on a join, so the vacuity is visible rather than silent.

## 8. LD_PRELOAD counters: read *all* the lines, and count the async doors

Two ways to get a confident zero out of an allocation probe:

- **Wrong pid.** The first run reported all-zeros. `mpirun` leaves a helper
  process that never touches CUDA, and its `ALLOC_PROBE` line was the one that
  survived a `tail`. `grep ALLOC_PROBE` the whole log, do not tail it.
- **Wrong door.** Kokkos 4.x defaults `Kokkos_ENABLE_IMPL_CUDA_MALLOC_ASYNC=ON`
  in some configurations, in which case `CudaSpace` allocates through
  `cudaMallocAsync` and a probe watching only `cudaMalloc` reads zero. Count
  `cudaMallocAsync`, `cudaMallocFromPoolAsync`, `cuMemAllocAsync` and
  `cuMemAllocFromPoolAsync` too, so a zero on the plain door is informative
  rather than ambiguous. (On this build it is plain `cudaMalloc`: 228-240 calls
  per rank, async doors zero.)

## 9. `nm -D --undefined-only | grep cudaMallocManaged` is a false positive on any Kokkos/CUDA binary

Kokkos compiles `CudaUVMSpace::impl_allocate` whenever the CUDA backend is on,
so the import is present in builds with zero UVM Views. Use the build flag (the
`"  using unified memory"` literal), the instantiated-View symbols
(`LAMMPS_NS` × `CudaUVMSpace`), or a runtime counter. See REPORT.md §A.

## 10. Version pins that mattered

| thing | value |
|---|---|
| LAMMPS module | `lammps/10Sep2025` → upstream tag `patch_10Sep2025` |
| Kokkos | 4.6.2 (`lmp -h`); source read at `kokkos-4.6.02` |
| LAMMPS build | KOKKOS + GPU packages, CUDA backend, double precision, view layout **legacy** |
| MPI | OpenMPI 4.1.5, `--with-cuda --with-ucx`, `opal_built_with_cuda_support:true` |
| UCX at runtime | system **1.20.0** (`/lib64`), *not* the SDK's HPC-X 2.22.1 |
| comshu on MN5 | `$HOME/proj/comshu/build`, DuckDB present (`comshu sql` works) |
| job | 45082034, 2 × (4× H100), `acc_debug`, account `etur02` |
