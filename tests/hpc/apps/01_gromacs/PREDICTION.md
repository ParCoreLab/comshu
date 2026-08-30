# PREDICTION — round 2, GROMACS against `transport_mix`, `tag_flow` and the UCT layer

Written 2026-08-29/30 by an agent who has not read `providers/`, `libpcomm/`
(beyond `schema/`), `pcomm-read/`, `tools/`, `plans/` or `BUGS-FOUND.md`, and
**before any round-2 capture exists**. MN5 has been off the air all evening, so
nothing here can have been back-filled from a result; that is the point of the
ordering, not an excuse for it.

Everything below is derived from three sources, all cited to file and line:

* **GROMACS 2025.2** — `gitlab.com/gromacs/gromacs`, tag `v2025.2`.
* **Open MPI 4.1.x** — `github.com/open-mpi/ompi`, branch `v4.1.x`. This is the
  MPI `gmx_mpi` is actually linked against (round 1's `NOTES.md`: `libmpi.so.40`
  out of HPC-X 2.16, inside NVIDIA HPC SDK 23.11).
* **`libpcomm/schema/pcomm.fbs`** and the derived-table column contracts in
  `tests/hpc/apps/BRIEF-BLACKBOX.md`.

Round 1 (`REPORT.md`) judged this application against `comm_edges` and the raw
UCX tables, and found `comm_edges` structurally empty on the HPC-X stack (F1)
and the UCX cross-rank join broken (F2). Round 2 does not re-run that. It aims
at the five things that did not exist then: `ucx_deep`, `transport_mix`,
`tag_flow`, `tag_recv_resolved`, `ucx_rkey`.

---

## The run this predicts

Same application and input as round 1, so the two are comparable:
benchRIB, 2,136,412 atoms, 2 nodes x 4 H100 = 8 MPI ranks, `-npme 1 -dd 7 1 1
-notunepme -dlb no -nstlist 100 -update auto`, Open MPI 4.1.5 / UCX 1.15
(HPC-X 2.16) over NDR InfiniBand, `mpirun --map-by ppr:4:node`.

**`-nsteps 200`, not round 1's 1000.** The UCT layer records one row per *wire*
operation rather than per application call, and an intra-node rendezvous that
fragments in the shared-memory transport could produce two orders of magnitude
more rows than `ucx_tag_send` did. Every oracle below is a **ratio or a set**,
not an absolute count, so the step count does not enter any of them.

Providers: `-p ucx,ucx_deep`. Not `ib`: round 1 established (F1) that UCX on this
stack reaches the NIC through `mlx5dv_devx_*` and never calls `ibv_post_send`, so
the verbs data plane contributed nothing and `comm_edges` was a 196-byte header.
`ucx_deep` is the named replacement, and this is the first time it is pointed at
an application.

---

## P1 — the decomposition, and which of its edges cross the node boundary

### P1.1 Rank 7 is the PME rank, and DD cell index equals MPI rank

`-npme 1` gives `numPPRanks = 7`, `numRanksDoingPme = 1`. GROMACS's default
`-ddorder` is `interleave`, so the PME rank list comes from
`domdec/domdec.cpp:552 dd_interleaved_pme_ranks()`, over
`domdec/domdec.cpp:540 ddindex2pmeindex()`:

```
ddindex2pmeindex(i) = (i*npme + npme/2) / npp = (i*1 + 0)/7 = 0   for i = 0..6
```

so in the loop at `:558` the condition `p1 > p0` never fires and only the
terminal `i + 1 == numPPRanks` does, at `i = 6`, giving `pmeRanks[0] = 6+1+0 =`
**7**. PP ranks are therefore 0..6, the single PME rank is world rank 7, and
because the PME rank is last, DD cell index `i` is world rank `i`. *This is the
same answer `-ddorder pp_pme` would give, so the prediction does not depend on
which ordering the module's defaults select — worth stating, because round 1
derived it from the `pmeRanks.empty()` branch, which is the other one.*

With `-dd 7 1 1` the grid is one-dimensional along x and DD cell `i` sits at
`x = i`.

### P1.2 Each PP rank has exactly two halo neighbours

`domdec/domdec.cpp:1062 setup_neighbor_relations()` sets, for each DD dimension,

```
tmp[dim] = (ci[dim] + shift + numCells[dim]) % numCells[dim];   shifts = {1,-1}
```

so with one dimension and 7 periodic cells the neighbours of cell `x` are
`(x±1) mod 7` and nothing else. GROMACS 2025 exchanges the halo directly with
each such domain (`domdec/haloexchange.cpp:126` `MPI_Irecv(..., receive.rank(),
...)`), so there are no forwarded diagonal zones to add partners.

**This holds only while the halo reaches one cell.** The log line to check is
`domdec/domdec.cpp:2326`, printed because `-dlb no` takes the non-DLB branch:

```
The initial number of communication pulses is: X 1
```

Predicted `X 1`: benchRIB's box is roughly 28 nm on a side, so a cell is ~3.9 nm
against a pair-list radius under 1.5 nm. **If that line says `X 2`, this whole
prediction is void** and the partner set becomes `(x±1), (x±2)`. The job script
prints the line for exactly this reason.

### P1.3 The 13 edges, and the 6 that cross

`mpirun --map-by ppr:4:node` puts ranks 0-3 on node A and 4-7 on node B.

| edge | kind | node A / node B | crosses |
|---|---|---|---|
| 0-1, 1-2, 2-3 | halo | A-A | no |
| 4-5, 5-6 | halo | B-B | no |
| **3-4, 6-0** | halo | A-B | **yes** |
| 4-7, 5-7, 6-7 | PME | B-B | no |
| **0-7, 1-7, 2-7, 3-7** | PME | A-B | **yes** |

Six of thirteen. **This is the split `transport_mix` claims to see and
`comm_edges` cannot** — and on this stack `comm_edges` cannot see anything at
all, so `transport_mix` is not merely better here, it is the only witness.

### P1.4 The per-rank partner tables, which is what `transport_mix` is keyed by

`transport_mix` is keyed by `rank`, not by edge, so the oracle has to be
per-rank. From P1.3:

| rank | same-node partners | cross-node partners |
|---|---|---|
| 0 | 1 | **6, 7** |
| 1 | 0, 2 | **7** |
| 2 | 1, 3 | **7** |
| 3 | 2 | **4, 7** |
| 4 | 5, 7 | **3** |
| **5** | **4, 6, 7** | **— none —** |
| 6 | 5, 7 | **0** |
| 7 (PME) | 4, 5, 6 | **0, 1, 2, 3** |

**P1.4a — THE HEADLINE. Rank 5 talks to nobody off its node.** Its two halo
neighbours are 4 and 6 and its PME rank is 7; all three are on node B. Every
byte of rank 5's application traffic must appear on a shared-memory transport
and none on the fabric. Nothing rank 5 does is special — it is simply the
interior cell of the node-B slab — and that is what makes it a clean oracle: it
is a consequence of the decomposition, not of anything GROMACS or UCX chose.

**P1.4b — every rank has at least one same-node partner.** No rank is
fabric-only. So a capture in which any rank shows zero shared-memory bytes has
lost the intra-node transports.

**P1.4c — ranks 1 and 2 send no halo over the fabric.** All of their cross-node
traffic is PME. Ranks 4 and 6 are the mirror image: all of their cross-node
traffic is halo.

---

## P2 — the number `transport_mix` must produce

### P2.1 A bound that needs no measurement at all

Per force evaluation, write `p` for the total PP<->PME volume over all ranks and
`h` for the total halo volume over all ranks, both directions counted.

* Of the 7 PME spokes, **4 cross** (P1.3). Cross-node share of `p` = 4/7.
* Of the 7 halo edges, **2 cross**. Cross-node share of `h` = 2/7.

So the fabric's share of all application bytes is

```
f = (4/7 · p + 2/7 · h) / (p + h)
```

which for any `p, h > 0` lies **strictly between 2/7 = 28.6 % and 4/7 = 57.1 %**,
whatever the halo actually weighs. No log line, no measurement, no input deck
beyond the decomposition enters this. It is the cleanest oracle in the study.

**Predicted: fabric share of wire bytes in [25 %, 60 %]**, the bound widened by a
couple of points for wireup and collectives, which are small and spread over all
ranks.

### P2.2 The point estimate, and where its one measured input comes from

`p` is exact from the input file. Every PP rank sends its *home* atoms'
coordinates to the PME rank each step and the home sets partition the system
(`ewald/pme_pp.cpp:296`), and the forces come back the same way, so per step

```
p = 2 · 12 B · 2,136,412 = 51,273,888 B
```

`h` is not derivable from the tpr — it depends on the cutoff and the cell
geometry — so the point estimate below borrows the one number round 1 measured
from GROMACS's own log, `Av. #atoms communicated per step for force: 2 x
708,354.5`, giving `h = 2 · 708,354.5 · 12 = 17,000,508 B/step`. **The bound in
P2.1 does not use it; only the point estimate does.**

```
f = (4/7 · 51,273,888 + 2/7 · 17,000,508) / 68,274,396 = 0.5003
```

**Predicted point value: 50.0 % of wire bytes leave the node.**

### P2.3 The per-rank fingerprint

Same inputs, resolved per rank. Halo out per rank = 2 · h/14 = 2,428,644 B/step
(coordinates to one neighbour, forces to the other); PME coordinates out per PP
rank = 12 · N/7 = 3,662,420 B/step; PME rank 7 returns 12 · N = 25,636,944 B/step.

| rank | out bytes/step | of which fabric | **fabric %** |
|---|---:|---:|---:|
| 0 | 6,091,064 | 4,876,742 | **80 %** |
| 1 | 6,091,064 | 3,662,420 | **60 %** |
| 2 | 6,091,064 | 3,662,420 | **60 %** |
| 3 | 6,091,064 | 4,876,742 | **80 %** |
| 4 | 6,091,064 | 1,214,322 | **20 %** |
| **5** | 6,091,064 | **0** | **0 %** |
| 6 | 6,091,064 | 1,214,322 | **20 %** |
| 7 | 25,636,944 | 14,649,682 | **57 %** |

The shape `{80, 60, 60, 80, 20, 0, 20, 57}` is a fingerprint of the
decomposition. A capture that reproduces it is saying something no other table
in pcomm can say on this stack; a capture that returns a flat profile is saying
the transport classification is not working, however plausible its totals look.

### P2.4 Which transport names, in UCX's vocabulary

The assertions are written against **UCX's** transport names, not pcomm's
`scope` column, so that they test pcomm rather than agreeing with it:

* fabric — `rc_mlx5`, `rc_verbs`, `dc_mlx5`, `ud_mlx5`, `ud_verbs`
  (`uct_iface.dev_name` like `mlx5_%`)
* intra-node — `self`, `sysv`, `posix`, `cma`, `knem`, `xpmem`,
  `cuda_ipc`, `cuda_copy`, `gdr_copy`

`scope` is then checked *separately* against those names. If `scope` were used to
define the split and also to verify it, the assertion would be a tautology.

---

## P3 — what the direct-GPU-comm knob does to the transport, and to the tag

Round 1's central negative result was that the knob moved no bytes and no
messages, and that at the UCX layer the capture could not tell a host-staged
halo exchange from a GPU-direct one (F4, F5). Two new discriminators exist now,
and neither needs a new record — one is a table round 1 did not have, the other
was in `ucx_tag_send` all along.

### P3.1 The transport name changes (needs `ucx_deep`)

With direct GPU communication **ON**, GROMACS hands MPI a *device* pointer for
the intra-node halo (`domdec/gpuhaloexchange_impl_gpu.cpp:489,492`). UCX moving
device memory between two processes on one node uses `cuda_ipc`, staging through
`cuda_copy` where it cannot. With it **OFF**, GROMACS copies to a host buffer
itself and MPI never sees a device pointer, so the intra-node lanes are
`sysv`/`posix`/`cma`.

> **Predicted: `cuda_ipc` and/or `cuda_copy` appear in `uct_iface` in the ON arm
> and carry intra-node payload; in the OFF arm they carry none.**

That is the discriminator round 1 went looking for at the ibverbs registration
door and did not find. It should be visible one layer up, as a transport name.

### P3.2 The MPI tag changes (needs only `ucx_tag_send`)

This is the sharper of the two, and I did not expect it. GROMACS uses a
**different MPI tag** for the two PP->PME coordinate paths:

```
ewald/pme_pp_communication.h:54   enum { eCommType_ChargeA=0, ChargeB=1,
                                         SQRTC6A=2, SQRTC6B=3, SigmaA=4,
                                         SigmaB=5, NR=6, COORD=7,
                                         COORD_GPU=8, CNB=9 }

ewald/pme_pp.cpp:296              MPI_Isend(x.data(), n*sizeof(rvec), MPI_BYTE,
                                            pme_nodeid, eCommType_COORD, ...)   // tag 7
ewald/pme_pp_comm_gpu_impl_gpu.cpp:256
                                  MPI_Isend(sendptr_x, sendSize*DIM, MPI_FLOAT,
                                            pmeRank_, eCommType_COORD_GPU, ...) // tag 8
```

and Open MPI puts that tag in the top 24 bits of the UCP tag
(`ompi/mca/pml/ucx/pml_ucx_request.h:42`):

```
PML_UCX_MAKE_SEND_TAG(tag, comm) = (tag << 40) | (comm->c_my_rank << 20) | c_contextid
PML_UCX_TAG_BITS 24   PML_UCX_RANK_BITS 20   PML_UCX_CONTEXT_BITS 20
```

> **Predicted: `ucx_tag_send.tag >> 40 = 8` occurs in the direct-ON arm and
> never in the OFF arm.** If it occurs in neither, the GPU PME<->PP path did not
> engage at all and the knob's effect in round 1 was halo-only.

Only the *coordinate* send changes tag. The force return is tag 0 on both paths
(`pme_pp.cpp:566`, `pme_pp_comm_gpu_impl_gpu.cpp:186`), which is why the
assertion looks for tag 8 specifically and not for a change in the reverse
direction.

**And note what this does *not* imply.** Round 1 measured the PME rank's
outbound bytes as identical to the byte across the knob and read that as the
GPU path perhaps not engaging. P3.3 below supersedes that reading: the two paths
differ in MPI datatype (`MPI_BYTE` vs `MPI_FLOAT`) and Open MPI pre-multiplies
the count by the datatype size for both, so `count` is bytes either way and
byte-identity is exactly what a *working* GPU path also produces. The tag is the
only discriminator; the byte total never was one.

### P3.3 A prediction about `UcxTagSend.count` that follows from the same place

The schema says `count` is "the raw UCP datatype count". Open MPI 4.1.x makes
that equal bytes for most datatypes but not all, and the reason is precise:

```
ompi/mca/pml/ucx/pml_ucx.c:824      ucp_tag_send_nbx(ep, buf,
                                        mca_pml_ucx_get_data_size(op_data,count),
                                        tag, param)
ompi/mca/pml/ucx/pml_ucx_datatype.h:87   return count << op_data->size_shift;
ompi/mca/pml/ucx/pml_ucx_datatype.c:201  is_contig_pow2 = is_contig && is_pow2(size)
                                          -> size_shift = log2(size), datatype left contig(1)
                                          -> else size_shift = 0, datatype = contig(size)
```

So for a contiguous datatype whose size is a power of two — `MPI_BYTE`,
`MPI_FLOAT`, `MPI_DOUBLE` — Open MPI pre-multiplies and `count` **is bytes**.
For a contiguous datatype whose size is not a power of two — `MPI_Type_contiguous(3,
MPI_FLOAT)`, 12 bytes — it does not, and `count` is **elements**.

> **Predicted: `count` is bytes on both arms of this run**, because GROMACS uses
> only `MPI_BYTE` and `MPI_FLOAT` on these paths. This is why round 1's
> `12 · natoms · (nsteps+1)` landed to four parts per million with `MPI_FLOAT`
> live in the ON arm.
>
> **And predicted as a gap:** nothing in `ucx_tag_send` records the UCP datatype,
> so *the capture cannot distinguish those two cases*. The row for a 12-byte
> contiguous type looks exactly like a row for `MPI_BYTE` and means something
> twelve times different. See `QUESTIONS.md` Q3.

---

## P4 — the peer that round 1 said was unavailable is in the tag

Round 1's F2: `ucp_worker_query` is unhooked on Open MPI 4.1.x, so
`ucx_ep_connection` resolves the wrong worker and no rank-to-rank matrix can be
built. That finding stands. But for **any** Open MPI application the peer does
not need that join, because Open MPI encodes it in the tag itself
(`pml_ucx_request.h:65`):

```
PML_UCX_TAG_GET_SOURCE(tag) = (tag >> 20) & 0xFFFFF
PML_UCX_TAG_GET_MPI_TAG(tag) = tag >> 40
PML_UCX_SPECIFIC_SOURCE_MASK = 0x800000ffffffffff   (rank bits ARE matched)
PML_UCX_ANY_SOURCE_MASK      = 0x80000000000fffff   (rank bits are NOT matched)
```

Two consequences, both rank-local and needing no cross-rank join at all:

**P4.1 (self-check, and the canary).** On rank `r`, *every* `ucx_tag_send.tag`
must satisfy `(tag >> 20) & 0xFFFFF = r`. This is a property of Open MPI, so a
violation means the recorded tag is not the tag that was passed. It also proves
the decode works at all — if the export renders `tag` as a hex string, arithmetic
on it silently matches nothing and every other tag assertion passes vacuously.
That hazard gets its own clause.

**P4.2 (the topology).** On rank `r`, a `ucx_tag_recv` whose `tag_mask` has the
rank field set names its peer in the same bits. Ranked by volume:

> **Predicted: for each PP rank `r`, the three highest-volume tagged-receive
> peers are exactly `{(r-1) mod 7, (r+1) mod 7, 7}`** — its two DD neighbours and
> the PME rank. For rank 7 the top seven peers are `{0..6}`.

Collectives will add low-volume peers (Open MPI's tuned Allreduce pairs ranks by
recursive doubling), which is why the claim is about the top of the volume
ranking and not about the peer *set*. Halo and PME traffic is hundreds of
megabytes; a reduction of a few dozen doubles at every hundredth step is
kilobytes.

This is a rank-space topology recovered from a capture that round 1 declared
topology-blind, and it is derived entirely from the application stack.

---

## P5 — `tag_flow`, and why it is predicted to be empty of matches

`tag_flow` joins the 600s to the 800s on the tag. `uct_send.tag` is written only
by the hardware-tag-matching doors — `tests/hpc/cases/21_ucx_deep/asserts/
tag_and_addr_are_door_scoped.sql` states this outright and case 21's job script
has to set `UCX_RC_MLX5_TM_ENABLE=y` and pin `UCX_TM_THRESH` to reach them.

MN5's HPC-X does not enable tag offload by default.

> **Predicted, default arms: `tag_flow` contains no `tag_matched` rows.** Every
> UCP send is unmatched and every UCT operation is `wire_only`, because without
> offload UCP does its own matching and the tag travels inside an active-message
> payload the UCT record does not parse.
>
> **Predicted, `tm` arm** (`UCX_RC_MLX5_TM_ENABLE=y`, `UCX_TM_THRESH` pinned
> below GROMACS's message sizes): tag doors fire and matched flows appear.

Both outcomes are informative and they fail differently, which is why the run
carries a third short arm. If the tm arm *also* shows no matched flow while tag
doors are present, the join is wrong rather than the fabric being quiet — that
is a defect. If no tag door fires even with offload requested, that is a fact
about MN5, not about pcomm. The assertion is written to tell those two apart,
after case 21's `tag_offload_was_reached.sql`.

**The stake for a user:** if this is right, `tag_flow` — advertised as the join
between intent and wire — is structurally empty for the most common MPI stack on
this machine unless a UCX variable is set that no application sets for itself.

## P6 — `tag_recv_resolved`

`tag_recv_resolved` resolves a receive that arrived by *message handle* to its
true delivered length, through `ucx_tag_probe` ("`length` is the one true byte
count in the UCX family"). Open MPI reaches that path only from
`MPI_Mprobe`/`MPI_Improbe`; GROMACS calls neither.

> **Predicted: `ucx_tag_probe` has zero rows, and `tag_recv_resolved` therefore
> has no row carrying a `delivered` value.** Any row that does carry one, with no
> probe to have produced it, is a defect.
>
> **Invariant, predicted to hold unconditionally: `delivered <= offered`.** A
> receive cannot deliver more than the capacity it offered. This needs no oracle
> at all and is the cheapest real check in the set.

## P7 — `ucx_rkey` and `ucx_rma`

GROMACS uses MPI point-to-point and collectives only; it creates no MPI window
and issues no `MPI_Put`/`MPI_Get`.

> **Predicted: `ucx_rma` and `ucx_atomic` are empty.**

`ucx_rkey` is the interesting one. UCP's *rendezvous* protocol unpacks a remote
key internally on every large transfer, and GROMACS's messages are large. Whether
that internal unpack passes through the public `ucp_ep_rkey_unpack` symbol the
provider hooks is not something the documentation says.

> **Predicted (low confidence, and stated so): `ucx_rkey` is empty, because UCX
> reaches its internal unpack without going through the public symbol.** If it is
> *not* empty while `ucx_rma` is, the capture holds a population of remote keys
> belonging to no recorded operation — not wrong, but unattributable, and worth
> naming.

---

## P8 — what round 1's findings predict about this capture

Restated as predictions so they are scored rather than assumed:

* `comm_edges` empty (F1) — predicted to hold; `-p ib` is not even loaded.
* `ucx_ep_connection` resolving the wrong worker (F2) — predicted to hold, and
  P4 is the route around it.
* `link_health` — **not** predicted empty. The brief was corrected on 2026-08-29:
  it is a fixed rubric of one row per finding kind, so a clean capture has every
  row present with `n = 0`. Predicted: `unnamed_uct_transport` is the row at risk,
  because it counts UCT operations whose interface cannot be named and this is
  the first application-scale run of the layer.
* `uct_binding.accepted = 1` and `forced = 0` on every rank — predicted, since
  case 21 recorded UCX 1.18 against a 1.19 build with matching `ops_size`; here
  the runtime is UCX 1.15 out of HPC-X 2.16, an *older* UCX than case 21's, and
  the `uct_iface_ops_t` layout guard has never been exercised against it.
  **This is the single most likely thing to make the whole run empty**, and it is
  the first thing to read if `uct_send` has no rows.

---

## Scoring table, to be filled in after the run

| # | prediction | verdict |
|---|---|---|
| P1.1 | PME = rank 7, DD cell = rank, under `interleave` | |
| P1.2 | 1 communication pulse; partners `(x±1) mod 7` | |
| P1.4a | rank 5 has no cross-node partner | |
| P1.4b | every rank has intra-node traffic | |
| P2.1 | fabric share of wire bytes in [25 %, 60 %] | |
| P2.2 | point value ~50 % | |
| P2.3 | per-rank fingerprint {80,60,60,80,20,0,20,57} % | |
| P3.1 | `cuda_ipc`/`cuda_copy` in the ON arm only | |
| P3.2 | MPI tag 8 in the ON arm only | |
| P3.3 | `count` is bytes for these datatypes | |
| P4.1 | sender rank recoverable from its own send tags | |
| P4.2 | top-3 receive peers = two DD neighbours + PME rank | |
| P5 | no `tag_matched` flows without `UCX_*_TM_ENABLE` | |
| P6 | no probes, `delivered <= offered` | |
| P7 | `ucx_rma` empty; `ucx_rkey` empty | |
| P8 | `uct_binding.accepted = 1` against UCX 1.15 | |
