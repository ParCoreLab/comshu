# PLAN — an access belongs to the buffer that was live when it happened

- **Created:** 2026-11-18 21:30 +03
- **Status:** SHIPPED (4996bc6). MN5 board 49210863: every runnable case green
  but 12_ibgda_doorbell, whose failure is NOT this change — the reader at
  6f27184, before it, gives the identical 672 unresolved sends on that capture.
  It is a sender-side qp_num collision (see below), latent until this run's QP
  numbering happened to collide.
- **Closes:** the item PLAN-20260805-0053 (entity lifetimes) ended on — "the
  reader does not use the window, and address reuse is real" — for the DuckDB
  layer and the UI. The CSV reader's mem_owner was settled in 3bbed23.
- **Follows:** PLAN-20261118-1930, whose "Not in this plan" named it.

## The measurement

LAMMPS r2 rerun, MN5 job 49207164, with the engine's CUDA allocation doors.
Kokkos frees a 140,384-byte halo buffer and allocates a 326,192-byte one at the
same base. A MemObject's id is interned from its base, so both are object 510 on
rank 0, and every object_access branch resolved an address against "the
smallest extent ever recorded there". Objects listed two rows for that address
and credited all 630 of the rank's halo sends to each; Devices sized the buffer
by the larger.

## Design

1. **`_obj_life`** (materialised, internal) and **`object_life`** (public): one
   row per incarnation. An open's close is the first MemObject close of its
   handle at or after it (ASOF per (rank, id)); opens of the same extent sharing
   a close are one incarnation seen by several emitters (`emitters`), which is
   the rule the CSV reader's settle_lives() applies too. `open_ns`/`close_ns`
   are wall clock via `_clock`, so a window can be compared with an event on
   another rank.
2. **`object_access` picks the incarnation live at the access**, innermost among
   the live, else the nearest in time with `in_window = FALSE`. Two new columns,
   `life` and `in_window`. Times: the post (verbs), the decoded send (devx), the
   completion (verbs_recv, `_recv_attr`), the call (ucx_rma, ucx_send).
   Same-rank comparisons have no slack; cross-rank ones (a put names the PEER's
   buffer) allow 1 ms for NTP skew. p2p has no usable time — a gpu_access tick is
   a drain time — so `life` is set only when the extent had one incarnation and
   `in_window` is NULL.
3. **UI keys objects by (rank, object, life).** Objects lists incarnations with
   a lifetime column ("2 of 2 · 532 ms") and states how many accesses name an
   object but no incarnation; Devices sizes by the incarnation.
4. **Suite:** `access_inside_its_object` checks an offset against the
   incarnation it was credited to. **Tests:** `object_life_rule.sh`, a fixture
   whose ranks' clocks are 5 s apart (raw-tick comparison fails it), with two
   emitters of one extent and a stray second close, puts in the gap and inside
   the slack. Mutation-checked: removing the slack fails it.

## Measured after

LAMMPS r2 rerun: rank 0's address resolves to life 1 (140,384 B, 205 ms, 8
sends — the setup exchange) and life 2 (326,192 B, 532 ms, 622 sends); all
10,080 sends `in_window`; 32 incarnations with accesses = 16 ranks x 2.

## Not in this plan

- A `link_health` count of `in_window = FALSE` rows: link_health cannot read a
  derived table (they are copied to Parquet, not registered), so the signal is
  the column and the Objects subtitle.
- p2p incarnations, which need an access-time stamp gpumem does not record
  (PLAN-20260803-1021).

## Found on the way: a device send names a qp_num, and qp_num is per HCA

MN5 job 49210863, case 12 (host-doorbell arm). Rank 1 holds QPs on four HCAs
(LIDs 7821, 7830, 7837, 7848), and qp_num is HCA-local, so (rank, qp_num) is not
a QP: 61301 is both a CPU proxy QP (devx obj 467, gpu_driven 0, loopback on
7837) and the GPU-driven QP the device sends came from (obj 1340, gpu_driven 1,
connected on 7821 to rank 0's 69664); 61302 likewise, three times over.
Mlx5DeviceSend and Mlx5DevxQpConnect carry only qp_num. The derived layer refuses
to choose (all 672 of rank 1's sends -> dst -1, so gpu_puts_land_in_the_app_buffer
places none of its 81,920 puts); the CSV reader chooses, and chose wrong for
61301 (49 sends reported as rank 1 -> rank 1). Previous boards passed because the
numbers did not collide.

The fix is at capture: the decoder knows which devx QP object a doorbell's WQE
belongs to, and the connect is decoded from a modify on a known object, so both
records can carry the QP's devx `obj`, and the readers can join on (rank, obj)
instead of (rank, qp_num). Its own plan; not done here.

## Follow-up: the qp_num collision, fixed at capture (ee75636)

Mlx5DeviceSend and Mlx5DevxQpConnect now carry `qp_obj`, the queue pair's
interned DEVX object; readers key queue pairs on (rank, qp_num, qp_obj) and fall
back to the number only when one queue pair on the rank has it.
`tests/qp_namesake_rule.sh` is the 61301 case in miniature.

MN5 board 49220686: SUITE GREEN (09/10 SKIP, no PeerMappingOverride). Case 12:
every one of 1,347 device sends carries a nonzero qp_obj and resolves
(0→1 675, 1→0 672, 0 unresolved). This run's numbering did NOT collide (no
rank had two connects under one number), so the collision branch on real
hardware is still covered only by the local rule test; a board whose numbers
collide again will exercise it.
