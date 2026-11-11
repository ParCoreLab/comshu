# PREDICTION — OSU Micro-Benchmarks 7.5, one-sided set, under `-p ucx,ucx_deep`

**Written 2026-08-29, before any capture of this workload exists.** Nothing in
this file was derived from a comshu output; every number below comes from OSU's
own source, cited by file and line, and from the command lines in `job.sbatch`.
MN5 was unreachable (ssh timeout on `alogin2` from ~17:00) for the whole time
this was written, so there is no capture to have peeked at.

---

## Why this application

The brief's preference order is NWChem/CP2K over Global Arrays, then
ARMCI-MPI's or GA's own tests, then the OSU one-sided set. I chose **OSU**, and
the reasons are in that order:

1. **NWChem / CP2K via GA/ARMCI-MPI.** Round 1 enumerated MN5's modules and
   found `gromacs/2025.2-gcc-nvhpcx`, `lammps/10Sep2025`,
   `quantumespresso/7.5-nvidia-nvhpcx` — no NWChem, no CP2K, no Global Arrays.
   Whether one exists could not be checked, because MN5 has been refusing SSH
   all evening, and MN5 has no outbound network so a source build would have to
   be fetched here and rsync'd — NWChem is a multi-hour build with a
   ScaLAPACK/GA dependency chain, spent on the chance that its RMA even reaches
   MPI (GA prefers ComEx, whose default on OpenMPI is *progress ranks* using
   two-sided messages — in which case there would be no MPI_Put at all and the
   whole study would be void).
   **And it has no oracle.** Nothing in an NWChem input deck determines how many
   `MPI_Put`s the SCF will issue; the count is a function of the task scheduler.
   A capture from it can be described, not checked.
2. **ARMCI-MPI / GA test programs.** Real third-party code, buildable, and its
   tests do document their semantics. But ARMCI-MPI is a *translation layer*: an
   `ARMCI_Put` becomes some number of `MPI_Put`s decided by ARMCI's own
   batching, its IGNORE-buffer copy path and its window-per-allocation policy.
   To derive the oracle I would have to read ARMCI's implementation, which puts
   me in the same position as reading comshu's — an oracle that is a second
   implementation's internals is not an independent oracle.
3. **OSU one-sided.** Chosen. **The oracle is exact and it is on the command
   line.** The number of one-sided operations, their size, their direction and
   the rank that issues them all follow from four arguments and twenty lines of
   OSU source. A capture can be checked to the byte, and if it disagrees, the
   disagreement is attributable.

Falling back to (3) is what the brief permits and I think it is the right call
on the merits, not just on availability: this study exists to test **`ucx_rkey`
/ `ucx_rkey_pack` and the `_nbi` / atomic doors**, which nothing third-party has
ever driven. For that question an exact count is worth more than a famous name,
and OSU is not a toy — it is the benchmark every MPI vendor tunes against, and
it is code nobody in this project wrote.

Source: `osu-micro-benchmarks-7.5.tar.gz`,
sha256 `1cf84ac5419456202757a757c5f9a4f5c6ecd05c65783c7976421cfd6020b3b3`,
from `mvapich.cse.ohio-state.edu`. Line numbers below are into that tarball.

---

## The workload, and where each number comes from

Four benchmarks, all `nprocs == 2` (enforced at `osu_put_bw.c:117`), one rank
per node so the pair is **inter-node** and UCX cannot answer with shared memory.

### A/B — `osu_put_bw` and `osu_get_bw`

```
-m 512:8192 -W 8 -i 20 -x 5     # -w allocate -s flush are the defaults
```

Defaults, from `osu_put_bw.c:36-37` / `osu_get_bw.c:34-35`: `options.win =
WIN_ALLOCATE`, `options.sync = FLUSH`. So `run_put_with_flush`
(`osu_put_bw.c:269`) / `run_get_with_flush` runs, and it is this loop:

```c
for (size = min; size <= max; size = size * 2) {          /* :287  */
    allocate_memory_one_sided(..., size * window_size, type, &win);  /* :289 */
    if (rank == 0) {
        MPI_Win_lock(MPI_LOCK_SHARED, 1, 0, win);                    /* :304 */
        for (i = 0; i < options.skip + options.iterations; i++) {    /* :305 */
            for (j = 0; j < window_size; j++)
                MPI_Put(sbuf + j*size, size, MPI_CHAR, 1,
                        disp + j*size, size, MPI_CHAR, win);         /* :314 */
            MPI_Win_flush(1, win);                                   /* :317 */
        }
        MPI_Win_unlock(1, win);                                      /* :333 */
    }
    free_memory_one_sided(...);                                      /* :348 */
}
```

`-i 20` sets **both** `iterations` and `iterations_large`
(`osu_util.c:360-370`); `-x 5` sets **both** `skip` and `skip_large`
(`osu_util.c:326-336`). That kills the `size > LARGE_MESSAGE_SIZE` re-set at
`:296`, so the counts do not change across the sweep. `MPI_CHAR` throughout, so
an MPI element count equals bytes.

| size B | windows | ops | bytes |
|---|---|---|---|
| 512  | 1 | 200 | 102,400 |
| 1024 | 1 | 200 | 204,800 |
| 2048 | 1 | 200 | 409,600 |
| 4096 | 1 | 200 | 819,200 |
| 8192 | 1 | 200 | 1,638,400 |
| **total** | **5** | **1,000** | **3,174,400** |

ops per size = `(skip + iterations) * window_size` = `(5 + 20) * 8` = 200.
**Every one of them is issued by rank 0**; rank 1 executes only `MPI_Barrier`
(`:337`). The sweep starts at 512 B deliberately: MPI's own window
synchronisation traffic is 8- and 64-bit words, so no control operation can
collide with a size in this sweep, and "count the rows whose size is one of
mine" is therefore an exact filter rather than an approximation.

Five `MPI_Win_allocate` / `MPI_Win_free` cycles (`osu_util_mpi.c:2933`), one per
size — that is the number of windows the rkey assertions use.

### C — `osu_acc_latency -m 4:4 -T MPI_INT -i 20 -x 5`

`run_acc_with_flush` (`osu_acc_latency.c`) does one `MPI_Accumulate(sbuf, count,
dtype, 1, disp, count, dtype, MPI_SUM, win)` per iteration with `count = size /
dtype_size` = 4/4 = **1 element of 4 bytes**, inside one lock/unlock, for one
size. `-T MPI_INT` is accepted because acc/fop set `options.show_validation = 1`
before `process_options`, which is what puts `T:` in the option string
(`osu_util.c:601`). **25 accumulates, 4 bytes each, all from rank 0, one
window.**

### D — `osu_fop_latency -m 4:4 -T MPI_INT -i 20 -x 5`

`run_fop_with_flush` (`osu_fop_latency.c:306`) has **no size loop**: one window,
then `skip + iterations` × `MPI_Fetch_and_op(sbuf, tbuf, dtype, 1, disp,
MPI_SUM, win)` — **25 fetching atomics of one 4-byte int, all from rank 0.**

### E — the contrast arm: `osu_fop_latency` with the default `MPI_CHAR`

Identical in every way except the datatype, which is 1 byte. This is the knob.

---

## What the capture must contain

Two `rank-*` directories. Everything below is for the **primary arm**,
`-p ucx,ucx_deep`, on `osu_put_bw` (the `osu_get_bw` capture is the same claim
with the direction reversed, which is why one `meta.json` serves both).

### The UCP layer — `ucx_rma`

1. **Exactly 200 rows per size**, for each of 512/1024/2048/4096/8192, and
   **exactly 5 distinct sizes ≥ 512**. `assert rma_op_count_matches_cli`,
   `rma_sweep_is_complete`.
2. **`sum(count) = 3,174,400`** over those rows, if `count` is bytes.
   `MPI_CHAR` was chosen so that a UCP datatype count and a byte count coincide
   whichever way the column is meant (see QUESTIONS.md #2 — the schema does not
   say which it is for `UcxRma`).
3. **All of them on one rank**, and it is the rank that also holds the sends;
   rank 1 is a passive target and issues no one-sided operation.
   `rma_is_origin_only`.
4. **No row with `status < 0`.** `ucp_put_nbi` returns `UCS_OK` or
   `UCS_INPROGRESS` (0 or 1); anything negative is a refused operation, and
   1,000 refused puts would still produce 1,000 rows.
5. **`door` is `nbi`** — case 22's static survey of this machine's `mca_osc_ucx`
   found `ucp_put_nbi`/`ucp_get_nbi` and no `_nbx` RMA call at all. I do **not**
   assert this in SQL: the enum vocabulary is not published anywhere a black-box
   user can read and the CSV/Parquet exports decode it differently
   (QUESTIONS.md #1). It is printed by the job script instead. `nbx` here would
   mean the stack changed under this project.

**Pre-registered alternative, so it cannot be rationalised later.** If (2) holds
but (1) does not — the byte total right, the per-size multiplicities wrong —
then `osc/ucx` fragmented an `MPI_Put` into several `ucp_put_nbi` calls, and
that is a finding about **OpenMPI**, not about comshu. I predict it will not
happen: `opal_common_ucx`'s put/get path issues one `ucp_put_nbi` per
contiguous MPI_Put and lets UCP fragment below the API. If it does happen, the
right response is to record it and leave the assertion red, not to weaken it.

### The remote keys — `ucx_rkey`, `ucx_rkey_pack`

This is the headline; these records have never been driven by anything nobody
here wrote.

6. **Every `ucx_rma.rkey` resolves to a `ucx_rkey` row on the same rank.** An
   unresolved key means the row says a transfer happened and cannot say which
   peer region it touched. `every_rma_key_resolves`.
7. **Every `ucx_rkey.endpoint` resolves to a `ucx_endpoint` row on the same
   rank.** The key is unpacked *on an endpoint*; that is the only path from a
   key to a peer. A key naming an endpoint nothing opened is the same
   orphan-entity defect one level up. `rkey_names_a_live_endpoint`.
8. **Lifetimes balance**: no `entity_close` with `opener = 626` for a key that
   was never opened, and no key closed more often than it was opened.
   `rkey_lifetimes_balance`.
9. **At least 5 `ucx_rkey` rows on the origin rank** — one window per size, and
   a window whose target memory was never unpacked cannot be written to. I
   predict **more** than 5, probably 10: `osc/ucx` keeps a *state* window
   alongside each data window and both are exported. I assert only the floor,
   because how many keys an MPI unpacks per window is the MPI's business.
   `a_key_was_unpacked_for_every_window`.
10. **`ucx_rkey_pack` is non-empty on both ranks and every packed `memh`
    resolves to a `ucx_mem_map` row on the same rank.** `MPI_Win_allocate` maps
    the window through `ucp_mem_map` and exports it through `ucp_rkey_pack`; a
    pack naming a handle that no map recorded is an entity referenced by an
    artifact that nothing opens. Nobody has checked this join.
    `packed_key_names_a_mapped_region`.

Note what I **cannot** predict or assert: that rank 0's rkey corresponds to rank
1's packed buffer. The origin's key carries no hash and the target's pack
carries no peer — see QUESTIONS.md #3. This is the study's largest gap and it is
a gap in the records, not in this workload.

### The transport layer — `uct_send` / `transport_mix`

11. **`uct_binding` says `accepted = 1, forced = 0`** on both ranks, or the
    whole transport half is honestly empty and says so.
12. **`transport_mix` shows `put_*` (arm A) / `get_*` (arm B) doors**, and their
    `tl_name` is **never** `self`/`sysv`/`posix`/`cma`/`xpmem`/`knem`: one rank
    per node means there is no shared-memory peer to be had, so a one-sided
    operation on a shm transport would be a misattribution.
    `rma_crossed_the_fabric`.
13. **The wire carried at least the bytes the command line asked for**:
    `sum(out_bytes + in_bytes)` over `put_*`/`get_*` ≥ 3,174,400. Not equality —
    UCP may fragment a bcopy put at the transport's segment size, which
    preserves bytes but not operation counts, and MPI's own state-window traffic
    adds a little. `the_wire_carried_the_bytes`.
14. I do **not** predict which of short/bcopy/zcopy. Thresholds are deliberately
    left at their defaults (case 21 owns the protocol-selection axis and pins
    them; this study must not, or the two cases stop being independent). At
    512–8192 B I *expect* mostly `put_bcopy` with `put_zcopy` appearing at the
    top of the sweep, and no `put_short` (512 B is above any inline limit) — but
    that is a property of the HCA, so it is printed, not asserted.
15. **`link_health` has no row with `n > 0`.** The brief says this table is
    expected to be empty and every row in it is a defect in the link.

### The atomics — arm C and D, `arms/atomic/`

16. **`ucx_atomic` is non-empty and holds at least 25 successful 4-byte
    operations on one rank**, with `op_size` in {4, 8} on *every* row and no row
    with `status < 0`. `atomics_reached_the_atomic_doors`,
    `atomic_op_size_is_a_word`.
17. **Every `ucx_atomic.rkey` resolves to a `ucx_rkey` row**, same claim as (6)
    for the atomic doors. `atomic_key_resolves`.

**Pre-registered alternative, and it is a real possibility.** OpenMPI's
`osc_ucx` has an MCA switch `osc_ucx_acc_single_intrinsic` that is **off by
default**; with it off, `MPI_Accumulate`/`MPI_Fetch_and_op` may take the generic
path — take the accumulate lock, `ucp_get_nbi`, compute locally, `ucp_put_nbi`
— in which case **the workload's own operations never reach the atomic doors at
all** and `ucx_atomic` holds only MPI's internal 64-bit lock traffic. Job
45177421 (case 22) is evidence the workload's atomics *do* reach the door on
this stack: it recorded the probe's own operations failing with status -5 for
asking for a **one-byte** atomic, which cannot happen unless they were passed
down. So I predict (16) holds. If instead arm C/D shows zero 4-byte atomics and
a matching population of 4-byte `ucx_rma` gets and puts, that is **not a comshu
defect** — it is claim type 3, the capture explaining a protocol the MPI
documentation only asserts, and the trace showing a lock-get-put where the
standard says "accumulate".

### The knob — arm E, `MPI_CHAR` fetch-and-op

One variable changed: the atomic width, 4 bytes → 1 byte. UCX's atomic doors
support 4- and 8-byte operands and nothing else, so I predict the two captures
differ **in the trace**, in one of exactly two ways:

- rows in `ucx_atomic` with `op_size = 1` and `status < 0` (the door was
  reached and the transport refused it — what case 22 measured), or
- no workload atomics at all, and 1-byte `ucx_rma` traffic in their place (MPI
  checked the width itself and fell back).

Either is a demonstration that the capture distinguishes a legal MPI program
that gets hardware atomics from a legal MPI program that does not, from the
datatype alone. If the two captures are *indistinguishable*, that is the
interesting negative: the record is not carrying the operand width faithfully.

### The shared pass — arm F, `-p ucx,ib` on `osu_put_bw`

`ucx_context` **and** `ib_mem_region` both non-empty in one capture: two
hook-based providers, one process, one pass. `ib_post_send` is expected to stay
**empty** — UCX registers memory through ibverbs but posts through nothing
exported (apps/README.md), so the memory-region table is the verbs witness and
asserting on sends would fail a correct capture. Printed, not asserted: this arm
has a different provider list and so cannot share the primary arm's `meta.json`.

### What must be empty or near-empty

- `ib_post_send` — always, on this stack.
- `nccl_*`, `gpu_access`, `mem_object` — no GPU, no NCCL, no NVBit provider.
- `ucx_tag_send` — **not** zero: `MPI_Init`, `MPI_Win_allocate`'s address
  exchange and `MPI_Barrier` are two-sided. I expect tens of rows, not
  thousands, and none of them 512–8192 B by coincidence — but I assert nothing
  about them, because how OpenMPI implements a barrier is not this study's
  claim.

---

## What would falsify this, in one line each

| claim | falsified by |
|---|---|
| the count oracle | any size in the sweep with a row count ≠ 200 |
| origin attribution | one-sided rows on more than one rank |
| the key join | any `ucx_rma.rkey` with no `ucx_rkey` row |
| the endpoint join | any `ucx_rkey.endpoint` with no `ucx_endpoint` row |
| the map join | any `ucx_rkey_pack.memh` with no `ucx_mem_map` row |
| one key per window | fewer `ucx_rkey` rows than windows on the origin rank |
| fabric attribution | a `put_*`/`get_*` row on a shared-memory `tl_name` |
| the wire total | `put_*`/`get_*` bytes below 3,174,400 |
| the atomic doors | zero successful 4-byte atomics in arm C/D |
| the knob | arm E indistinguishable from arm D in the capture |

## Committed before the run

This file, `meta.json`, `arms/atomic/meta.json`, all seventeen `.sql` files and
`job.sbatch` are committed in one commit with no capture in existence, on a
machine (FreeBSD) that cannot build or run comshu at all.
