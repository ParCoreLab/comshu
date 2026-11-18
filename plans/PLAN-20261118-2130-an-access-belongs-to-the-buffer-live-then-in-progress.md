# PLAN — an access belongs to the buffer that was live when it happened

- **Created:** 2026-11-18 21:30 +03
- **Status:** IN PROGRESS — implemented and locally tested; the MN5 board is the
  remaining check.
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
