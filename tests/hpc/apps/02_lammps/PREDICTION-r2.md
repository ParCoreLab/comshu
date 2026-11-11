# 02_lammps round 2 — pre-registered prediction

Written 2026-09-02 on the FreeBSD workstation, **committed before `job-r2.sbatch`
had ever been submitted and before any round-2 capture existed.** Every number
below is derived from three things and nothing else:

* LAMMPS's own source, at the exact revision the module ships —
  `/apps/ACC/LAMMPS/SRC/lammps-patch_10Sep2025/src` on MN5, which is upstream tag
  `patch_10Sep2025`, the source of module `lammps/10Sep2025`;
* the input deck this study writes (`in.lj`, reproduced in §1), which is
  `bench/in.lj` from that same tree with three lines added;
* the derived-table column contracts in `apps/BRIEF-BLACKBOX.md`.

Nothing here is read off a capture — not round 1's, not anybody's. Round 1's
`REPORT.md` is used exactly once, in §3.5, as a *retrospective check on the
geometric model*, and it is flagged there as such.

Black-box: no file under `providers/`, `libcomshu/` (bar `schema/`),
`comshu-read/`, `comshu-ui/`, `gpu-backends/`, `tools/`, `plans/` or
`BUGS-FOUND.md` was read while writing this.

---

## 0. What round 2 is for, and why it is not round 1 again

Round 1 ran LAMMPS at 8 ranks over 2 nodes with `-p ib,ucx` and found that
**`comm_edges` was empty** — three independent closures at once (`REPORT.md` D1,
D2, D3): the verbs lane had no sends because UCX rings its own doorbell, the UCX
lane did not exist in the derived layer, and the hand-built UCX join was blocked
because `ucp_worker_query` was not a hooked door.

Two of those three have since changed, and **neither has ever been pointed at
LAMMPS**:

* `ucp_worker_query` is hooked, so `ucx_ep_connection -> ucx_worker_address`
  resolves and `comm_edges` has a UCX lane in rank space;
* `comm_edges.scope` (added 2026-09-01) labels each edge `intra-node` /
  `network` / `unknown` **from node membership**, and `transport_mix.scope`
  labels each transport `intra-node` / `network` / `unknown` **from `tl_name`**.

Those are two independently-derived answers to the same question. LAMMPS is the
instrument that can separate them, because a Cartesian domain decomposition
makes the true answer computable in closed form, per edge, before the run.

So round 2 asks one question with a numeric answer:

> **Given a decomposition whose every communication edge is known in advance,
> and a rank-to-node placement chosen so that the answer is known in advance,
> does comshu put the right bytes on the right side of the node boundary?**

And it asks it **twice, with the decomposition held byte-for-byte fixed and only
the placement changed**, so that a right answer cannot be a coincidence.

---

## 1. The run

16 MPI ranks, 4 nodes, 4 ranks and 4 H100 per node, MN5 `acc_debug`.
`lammps/10Sep2025`, KOKKOS/CUDA, Open MPI 4.1.5 -> **system UCX 1.20.0**
(`/lib64`, `ucx-1.20.0-1.20251022.03898fede`) -> NDR InfiniBand.

Input deck — `bench/in.lj` verbatim except the three marked lines:

```
variable x index 2 / y index 2 / z index 2      # -> 40 x 40 x 40 fcc cells
units lj ; atom_style atomic
processors      2 2 4 map xyz                   # <- ADDED: the oracle
lattice         fcc 0.8442
region          box block 0 40 0 40 0 40
create_box 1 box ; create_atoms 1 box ; mass 1 1.0
velocity        all create 1.44 87287 loop geom
pair_style      lj/cut 2.5 ; pair_coeff 1 1 1.0 1.0 2.5
neighbor        0.3 bin
neigh_modify    delay 0 every 20 check no
fix             1 all nve
thermo          100                             # <- ADDED: one thermo block
run             100
```

`processors 2 2 4 map xyz` is the whole point. `map xyz` is not decoration: it
replaces `MPI_Cart_create` (the `map cart` default) with an explicit,
source-defined rank-to-grid function, so the neighbour set is a *statement in
LAMMPS's source* rather than something Open MPI's Cartesian re-ordering
happened to produce. `src/comm.cpp:423-431` accepts it; `src/procmap.cpp:409`
implements it.

**Two arms, one decomposition.**

| arm | launcher | node of rank r | why |
|---|---|---|---|
| `block` | `mpirun -np 16 --map-by ppr:4:node` | `r / 4` | the ordinary placement |
| `cyclic` | `mpirun -np 16 --map-by node` | `r % 4` | the same job, dealt round-robin |

The application, the deck, the decomposition, the peer set, the message sizes
and the byte totals are **identical** in the two arms. Only which peer is on
which node changes. That is the experiment.

The bare arm prints `hostname` per rank, so the rank-to-node map is established
by the *operating system* before any capture exists, not by `rank_identity`.

---

## 2. The decomposition, derived

### 2.1 The grid map — `src/procmap.cpp:409-455`

`ProcMap::xyz_map` with `xyz = "xyz"`:

```cpp
grid2proc[i][j][k] = k*procgrid[1]*procgrid[0] + j*procgrid[0] + i;
```

so with `procgrid = (2,2,4)`

    rank r  <->  (i, j, k) = ( r % 2 , (r / 2) % 2 , r / 4 )

and the neighbours come from `grid_shift` on each axis (`procmap.cpp:437-453`):

| axis | procgrid | neighbour(s) of rank r |
|---|---|---|
| x | 2 | `minus == plus == r XOR 1` — **one** peer, reached by two swaps |
| y | 2 | `minus == plus == r XOR 2` — **one** peer, two swaps |
| z | 4 | `minus = (r-4) mod 16`, `plus = (r+4) mod 16` — **two** peers, one swap each |

**P1.1 — every rank has exactly four distinct MPI communication partners:**

    peers(r) = { r^1 , r^2 , (r+4) mod 16 , (r-4) mod 16 }

and no others. (Collectives are a separate population; see P1.5.)

**P1.2 — `k = r / 4` is the z coordinate AND, in the `block` arm, the node
index.** That is the coincidence the arm is built on: in `block` the z axis is
exactly the node axis, so the two z peers are the two network peers and the x
and y peers are intra-node. In `cyclic`, node(r) = `r % 4`, which is invariant
under `r ± 4` and flips under both `r^1` and `r^2` — so the assignment
**inverts**: x and y become network, z becomes intra-node.

| arm | intra-node peers of r | network peers of r |
|---|---|---|
| `block` | `r^1`, `r^2` | `(r±4) mod 16` |
| `cyclic` | `(r±4) mod 16` | `r^1`, `r^2` |

### 2.2 The swap table — `src/comm_brick.cpp:172-420`

```
maxneed[d] = (int)(cutghost[d] * procgrid[d] / prd[d]) + 1      # :262-264
nswap      = 2 * (maxneed[0]+maxneed[1]+maxneed[2])             # :351
```

`cutghost = cutneighmax = 2.5 + 0.3 = 2.8` (pair cutoff plus skin).
`prd = L = 40 a`, `a = (4/0.8442)^(1/3) = 1.679595`, so **L = 67.1838**.

    maxneed[0] = int(2.8*2/67.1838)+1 = int(0.0834)+1 = 1
    maxneed[1] = 1
    maxneed[2] = int(2.8*4/67.1838)+1 = int(0.1667)+1 = 1

**P1.3 — `nswap = 6`, one pulse per direction per axis, on every rank.** If the
run reports anything else, every volume below is void and the job script must
say so.

`nswap` is global (`comm_brick.cpp:342-351` Allreduces `recvneed`), so the halo
*message count* is identical on all 16 ranks even though the sizes are not.

**P1.4 — the six swaps are folded onto four peers as 2 : 2 : 1 : 1.**
`comm_brick.cpp:383-396`: `ineed` even sends to `procneigh[dim][0]`, odd to
`procneigh[dim][1]`. For x and y those are the same rank, for z they are two
different ranks. So of a rank's halo *messages*:

    r^1 : 1/3    r^2 : 1/3    r+4 : 1/6    r-4 : 1/6

**P1.5 — a fifth population exists and is small.** Open MPI's collectives
(LAMMPS Allreduces thermo output, and `Comm::setup` Allreduces `recvneed`), plus
UCX wireup. These produce edges outside `peers(r)`. Predicted: **under 1 % of
each rank's bytes**, and no non-neighbour edge above 1 % individually. Every
assertion below is written so that this population cannot make it pass.

---

## 3. The volumes, in closed form

### 3.1 The model

LAMMPS acquires ghosts one axis at a time and **forwards ghosts acquired in
earlier axes** (`comm_brick.cpp:383-396` and the `slablo/slabhi` construction
above it), so the slab a swap carries grows as the axes are consumed. With
subdomain extents `lx, ly, lz` and ghost cutoff `c`, and writing `Lx = lx+2c`
etc., the six slabs are

    x swaps (2)  :  c . ly . lz                each
    y swaps (2)  :  Lx . c . lz                each
    z swaps (2)  :  Lx . Ly . c                each

and their sum is exactly `Lx.Ly.Lz - lx.ly.lz`, the ghost shell — which is the
model's internal consistency check and also the bridge to LAMMPS's own log
(§3.4). Atom counts are these volumes times the number density 0.8442, and byte
counts are atom counts times a per-atom payload that is the same for every swap
in a given call, so **every ratio below is independent of the payload.**

### 3.2 The numbers, `processors 2 2 4`

    lx = ly = L/2 = 33.5919      lz = L/4 = 16.79595      c = 2.8
    Lx = Ly = 39.1919            Lz = 22.39595

| swap pair | peer | volume, both swaps | share of a rank's halo bytes |
|---|---|---|---|
| x | `r^1` | 2 x 1579.78 = **3159.56** | **20.45 %** |
| y | `r^2` | 2 x 1843.15 = **3686.29** | **23.86 %** |
| z- | `(r-4) mod 16` | 1 x 4300.81 = **4300.81** | **27.84 %** |
| z+ | `(r+4) mod 16` | 1 x 4300.81 = **4300.81** | **27.84 %** |
| | | total **15447.5** | 100 % |

**P2.1 — the four peers of every rank carry 20.45 / 23.86 / 27.84 / 27.84 % of
that rank's halo bytes**, in that order, and the two z peers carry equal
volumes.

**P2.2 — the count ratio and the byte ratio are different numbers, and both are
derivable.** Messages split 33.3 / 33.3 / 16.7 / 16.7 % (P1.4); bytes split
20.45 / 23.86 / 27.84 / 27.84 %. The peer that gets the *most* messages gets the
*fewest* bytes. A capture that got one of these right and the other wrong would
be caught; a capture that reported "four peers" and nothing else would not.

### 3.3 The headline — the fabric share, and it inverts

**P2.3 — the share of application bytes that leaves the node is**

| arm | network share | intra-node share |
|---|---|---|
| `block` | 4300.81 x 2 / 15447.5 = **55.68 %** | **44.32 %** |
| `cyclic` | (3159.56 + 3686.29) / 15447.5 = **44.32 %** | **55.68 %** |

**They are exact complements of each other, from the same decomposition.** This
is the claim the study exists for. It is stated for two derived columns that are
computed in two different ways from two different inputs:

* `comm_edges.scope`, from node membership — must give 55.68 / 44.32 and then
  44.32 / 55.68;
* `transport_mix.scope`, from `tl_name` — must give the same two numbers,
  because UCX must choose a shared-memory transport for the intra-node peers and
  an mlx5 transport for the network ones, and it must make that choice
  differently in the two arms.

Tolerance: **+-5 percentage points**, which allows for P1.5's collectives, for
wireup, and for the eager/rendezvous protocol split differing between shared
memory and IB. The two bands, [50.7, 60.7] and [39.3, 49.3], **do not overlap**,
so the knob is a discriminator and not a re-measurement.

`transport_mix` is summed as `out_bytes + in_bytes` and **never** with
`advertised_bytes` folded in — the brief's table of 2026-08-30 says an
advertisement is offered rather than sent, and adding it would bill a rendezvous
buffer twice, once as the sender's advertisement and again as the peer's Get.
The three payload columns partition the ops, and `in_bytes` is recorded at the
rank that issued the Get, so a global `out + in` counts each transfer exactly
once even when shared memory pulls and InfiniBand pushes.

### 3.4 An oracle from LAMMPS's own log, independent of both

The ghost shell volume is `Lx.Ly.Lz - lx.ly.lz = 15447.5`, and LAMMPS prints the
ghost count it actually built.

**P2.4 — LAMMPS's own end-of-run statistics must report
`Nghost: ~13041 ave` (= 0.8442 x 15447.5) and `Nlocal: 16000 ave`
(= 256000/16).** This is printed by the *bare* run, before any capture exists.
If it disagrees, the geometric model is wrong and P2.1-P2.3 are void — and I
would rather find that out from LAMMPS than from a capture.

### 3.5 One retrospective check, declared as such

The same model applied to round 1's configuration (`processors 2 2 2`, 8 ranks,
same box) predicts per-peer shares of **28.3 / 33.1 / 38.6 %**. Round 1's
`REPORT.md` §C measured 14 447 776 / 16 966 296 / 19 816 936 B on rank 0, i.e.
**28.2 / 33.1 / 38.7 %**. Agreement to about a tenth of a percentage point.

This is *not* the oracle and nothing here depends on it. It is recorded because
a model that reproduces an already-published measurement it was not fitted to is
worth more than one that does not, and because the reader is entitled to know
that the +-5 point band in P2.3 is generous rather than lucky.

---

## 4. What each derived table must hold

Per capture, `-p ucx,ucx_deep`, exported to Parquet and linked.

| table | prediction | why |
|---|---|---|
| `comm_edges` | non-empty, UCX lane, 16 x 4 = **64 heavy directed edges** | P1.1, and `ucp_worker_query` is now a hooked door |
| `comm_edges.scope` | `block`: 32 network + 32 intra-node edges. `cyclic`: the same 64 edges with the labels swapped | P1.2 |
| `comm_edges.scope = 'unknown'` | **0 heavy edges** | every rank has a node identity and every peer resolves |
| `transport_mix` | non-empty; `tl_name` covers at least one of `{sysv, posix, cma, knem, xpmem, self}` and at least one of `{rc_mlx5, dc_mlx5, rc_verbs, ud_mlx5}` | 4 ranks/node and 4 nodes: both sides must be present |
| `transport_mix.scope = 'unknown'` | **< 1 % of wire bytes** | a material share means the link met a transport nobody has classified, and is a finding |
| `rank_identity` | **16 rows, 4 distinct `node_hash`, 4 ranks each** | 4 nodes x 4 ranks |
| `rank_identity.device` | non-null on every rank, and the 4 ranks of a node name 4 *distinct* devices | `-k on g 4` -> `cudaSetDevice(local rank)`; and "`rank_identity.device` now resolves on UCX-only captures" is exactly the claim under test |
| `uct_binding` | `accepted = 1` | the providers for this arm are built against `/usr/include`, i.e. the *same* UCX 1.20.0 the application runs. If it is 0 the layout guard refused and every `transport_mix` claim above is void — the job script reads this first |
| `link_health` | 0 findings with `n > 0` | the rubric, queried as `WHERE n > 0` |
| `gpu_device` (type 528) | **absent** | measured before the run: `comshu types` for this build lists 58 types and 528 is not among them, although `libcomshu/schema/comshu.fbs:210` declares it. Predicting absence; if it appears, `comshu types` is incomplete, which is worse |
| `tag_flow` | rows exist; **no** flow in a matched state | MN5's UCX does not enable ConnectX hardware tag matching by default, so `uct_send.tag` is never written and the tag join has nothing to match on. Same expectation 01_gromacs recorded |

---

## 5. Named failure modes, stated in advance

So that a wrong number is diagnosed rather than explained.

1. **`nswap != 6`** in the bare log — the partner set is bigger than P1.1 and
   every share in §3 is void. The job script greps for it and says so.
2. **`Nghost` far from 13041** — the geometric model is wrong. Everything in §3
   goes with it. Also checked from the bare log.
3. **`uct_binding.accepted = 0`** — `ucx_deep` refused the UCX it met.
   `transport_mix` is then empty and only the `comm_edges` half of the study
   survives. This is a real possibility and it is why the two halves are stated
   against two different tables rather than one.
4. **`comm_edges` empty** — round 1's D3 has not moved after all. That would
   contradict 01_gromacs's F2 addendum, which measured the join closing on
   Open MPI 4.1.5, and would mean the fix is specific to HPC-X's UCX 1.15 and
   does not survive the system UCX 1.20.0. Reported as such, not as a surprise.
5. **The two arms agree with each other.** If `block` and `cyclic` produce the
   *same* network share, then `scope` is not a function of placement and is
   measuring something else — the single most valuable negative outcome
   available here, and the reason the experiment is a pair.
6. **`comm_edges` shows the right shares but `transport_mix` does not**, or vice
   versa. The two columns are derived from different inputs; a disagreement
   between them is a finding about the tool that no single-table study can
   produce.

## 6. Guesses that are NOT assertions

Recorded so that being wrong about them costs nothing.

* I expect `comm_edges.unit` to read `bytes` and `transport` to read `ucx`;
  neither has a published vocabulary, so no assertion names either value.
* I expect the `cyclic` arm to be *slower* in wall clock than `block`, because
  it moves 55.68 % of its halo over IB instead of 44.32 %. LAMMPS prints
  `Loop time`. No assertion depends on it.
* I expect intra-node traffic to arrive on `sysv`/`posix` with `cma` for the
  large messages, and *not* on `cuda_ipc`, because LAMMPS's Kokkos halo buffers
  reach MPI as device pointers only when `gpu/aware on` is in force and this run
  leaves the module default alone. If `cuda_ipc` appears it is interesting and
  it is not an error.
* I expect `ucx_rkey`/`ucx_rkey_pack` to be non-empty even though LAMMPS issues
  no one-sided call, because UCP rendezvous packs a remote key per large
  transfer. Not asserted: whether that internal unpack passes through the public
  symbol a provider can interpose is not something any document I may read says.
