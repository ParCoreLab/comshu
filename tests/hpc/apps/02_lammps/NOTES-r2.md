# 02_lammps round 2 — environment traps, on top of NOTES.md

Round 1's `NOTES.md` still applies in full. These are new, and they are ordered
by what each one cost.

---

## 1. The ucx_deep provider was compiled against the wrong UCX, and `-DUCX_INCLUDE_DIR` did not stop it

**Cost one queue slot** (job 45329318), and it is the trap most likely to catch
the next person.

LAMMPS on MN5 runs against the **system** UCX 1.20.0 in `/lib64` (package
`ucx-1.20.0-1.20251022.03898fede`), not against HPC-X's. `ucx_deep` patches
`uct_iface_ops_t` — an installed struct with no ABI promise — and guards the
patch by comparing the layout it was compiled against with the one it finds. So
the provider must be built against 1.20.0's headers.

`/usr/include/ucp/api/ucp.h` exists (`ucx-devel` is installed), so the obvious
configure is

```bash
cmake -B build-ucx -DCOMSHU_PROVIDER_UCX=ON \
      -DUCX_UCP_LIB=/usr/lib64/libucp.so -DUCX_INCLUDE_DIR=/usr/include
```

**and it silently does not take.** glogin's login profile auto-loads
`UCX/1.15.0`, which exports

```
CPLUS_INCLUDE_PATH=/apps/GPP/UCX/1.15.0/INTEL/include
```

and **gcc drops a `-I` that names a standard system directory**, so
`-I/usr/include` is discarded and `CPLUS_INCLUDE_PATH`'s 1.15.0 headers win.
Nothing warns. The build succeeds. The failure appears only at run time, in a
record most people would not think to read:

```
$ comshu dump <capture> 806
ticks, compiled_version, runtime_version, ops_size, accepted, forced
...    1.15.0            1.20.0           400       0         0
```

`accepted = 0` on all sixteen ranks, `uct_send` absent from the export,
`transport_mix` present and empty, and every assertion naming `uct_send`
reporting `[skip]`.

**The fix:**

```bash
module unload UCX
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH CPATH
mkdir -p ~/ucx120-include && cp -a /usr/include/{ucp,uct,ucs} ~/ucx120-include/
cmake -B build-ucx120 -DCOMSHU_PROVIDER_UCX=ON \
      -DUCX_UCP_LIB=/usr/lib64/libucp.so -DUCX_INCLUDE_DIR=$HOME/ucx120-include
```

A private include directory rather than `/usr/include` is the load-bearing part:
it is not a standard system directory, so `-I` survives. Verify with
`grep UCP_API_MINOR $HOME/ucx120-include/ucp/api/ucp_version.h` before building
and with `comshu dump <capture> 806` after running.

**Read `uct_binding` first, every time.** It is the only thing that tells an
empty `transport_mix` apart from a quiet fabric, and this case now ships
`asserts-r2/uct_binding_was_accepted.sql` so that the distinction is red rather
than grey.

## 2. Build against the system UCX and round 1's biggest trap disappears

Round 1's `NOTES.md` trap 1 was a SIGSEGV in `MPI_Init`: `libucx_trace.so`
carried an absolute RUNPATH to HPC-X 2.22.1's UCX, the loader built a hybrid of
HPC-X `libucp` with system `libuct`/`libucs`, and `ucp_ep_create` died inside
`ucp_wireup_select_transport`. The workaround was to append `/lib64` to
`LD_LIBRARY_PATH`.

Built against the system UCX the provider carries **no UCX in its RUNPATH at
all**:

```
RUNPATH: [$ORIGIN/../../libcomshu:.../build-ucx120/libcomshu:]
```

so there is nothing to form a hybrid with. `job-r2.sbatch` still appends
`/lib64` — appending costs nothing and guards against a module that puts some
other `libucp` on the path — but the crash is gone at the root rather than
worked around. **Still append, never prepend**: `/lib64` ahead of gcc-13.2
shadows its `libstdc++` and every comshu binary dies with
`GLIBCXX_3.4.30 not found`, which reads as a different bug entirely.

## 3. `a / b` on two integers is FLOAT division in DuckDB

**Cost the same queue slot**, and it is invisible in the failure message.

```sql
WHERE a.rank / 4 = b.rank / 4      -- 14/4 = 3.5, 15/4 = 3.75 -> FALSE
```

Every node-membership test written that way is false for two ranks that *are* on
the same node. The symptom was `rank_identity` reporting the wrong node grouping
in the `block` arm and the right one in `cyclic` — because `cyclic` used `%`,
which is integer. `rank_identity` was perfect in both.

Use `//` (or `div(a,b)`). Do **not** reach for `CAST(a/b AS BIGINT)`: DuckDB
rounds half away from zero, so `CAST(3.5 AS BIGINT)` is 4.

## 4. `params` as a leading CTE collides with an assertion's own `WITH`

The runner materialises `params` as `WITH params AS (SELECT ...)` and appends
the assertion. An assertion that opens with its own `WITH` then produces

```
WITH params AS (...) WITH universe AS (...)
       ^ Parser Error: syntax error at or near "WITH"
```

which killed five of nine assertions on the first run. `run-asserts-r2.sh` now
splices the two CTE lists — the file's leading `WITH` becomes a comma — so the
`.sql` files stay ordinary SQL.

## 5. `sum()` over zero rows is NULL, so a ratio assertion passes on an empty table

`network_share_of_wire_bytes.sql` passed on a `transport_mix` with **no rows**,
because its guard clause was `WHERE net + shm = 0` and `sum(...)` over nothing is
NULL, not 0. `COALESCE(sum(...), 0)`.

This is the single defect `BRIEF-BLACKBOX.md` names as the most common in this
suite, and the only reason it was caught is that the first job produced a
genuinely empty `transport_mix` (trap 1) — i.e. the delete-the-input test ran
itself. **Two sibling assertions had the same shape** and now carry explicit
`WHERE (SELECT count(*) FROM comm_edges) = 0` guards: any assertion that reaches
its evidence through an inner join is vacuous when the evidence is missing.

## 6. `comshu types` does not list type 528, and the schema declares it

Measured on the login node, no capture needed:

```
$ comshu types | wc -l
60                      # 58 types plus a header
$ comshu types | grep -E '^(524|526|528|530)'
524  artifact  node_identity
526  entity    peer_mapping
530  event     gpu_kernel
```

while `libcomshu/schema/comshu.fbs:210` says *"type 528, Artifact. What a device
ORDINAL actually refers to, on this rank"*. Rebuilt from the current checkout
(`a0b242e`), whose commits include the GpuDevice schema and its emit path; the
list is byte-identical to the older shared build's. See `QUESTIONS.md` Q4.

## 7. Home is at 78.6 of 80 GB — put captures on scratch

`bsc_quota` for `etur02`: `gpfs_home 78.58 GB / 80.00 GB`. `~/comshu-suite` alone
is 57 GB. A 16-rank `ucx_deep` capture is not what the last gigabyte is for.
`/gpfs/scratch/etur02/` is group-writable with ~400 GB free; this case writes to
`/gpfs/scratch/etur02/koc296797-apps/`.

## 8. `--map-by node` really does deal round robin, and it is worth printing

The whole round-2 experiment is that the two placements differ. Confirmed from
the operating system, both arms, before any capture:

```
block:   rank 000..003 -> as05r5b10   004..007 -> as05r5b12  ...
cyclic:  rank 000 -> as05r5b10  001 -> as05r5b12  002 -> as05r5b13  003 -> as05r5b14
         rank 004 -> as05r5b10  ...
```

so `node(r) = r/4` and `node(r) = r%4` respectively. Print it. An assertion about
`scope` that takes its node map from `rank_identity` is comshu marking its own
homework.

## 9. Do not `module purge`, and source `common.sh` first

Round 1's trap 3 stands: the `lammps/10Sep2025` modulefile activates a conda env
through miniforge and needs `bsc/1.0` alive, so a purge leaves `lmp_mpi` off
PATH with rc=0. But a job submitted from **glogin** also inherits the GPP module
tree and glogin's auto-loaded `impi`, which `common.sh` purges for you. The
order that works is: `source common.sh` **first** (it purges and repoints
`MODULEPATH` while nothing of yours is loaded), then the LAMMPS prereqs, then
`lammps/10Sep2025`, then `gcc/13.2.0`. Not `comshu_load_runtime` — that puts the
SDK's `mpirun` ahead of the one LAMMPS is linked against.

## 10. Version pins that mattered

| thing | value |
|---|---|
| LAMMPS module | `lammps/10Sep2025` -> upstream `patch_10Sep2025`, source at `/apps/ACC/LAMMPS/SRC/lammps-patch_10Sep2025` |
| MPI | Open MPI 4.1.5 (`openmpi/4.1.5-gcc`) |
| UCX at runtime | system **1.20.0** (`/lib64`), *not* HPC-X's |
| UCX the providers are built against | **1.20.0**, from `/usr/include` copied to `~/ucx120-include` |
| comshu | `~/proj/comshu/build-apps06`, private to this study, at checkout `a0b242e` |
| jobs | 45329318 (providers mis-built, still produced the comm_edges result), 45330017 (the study) |
