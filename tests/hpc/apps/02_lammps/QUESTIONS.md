# 02_lammps round 2 — what the capture and its documentation could not tell me

Written under the black-box rule: `providers/`, `libpcomm/` (beyond `schema/`),
`pcomm-read/`, `pcomm-ui/`, `gpu-backends/`, `tools/`, `plans/` and
`BUGS-FOUND.md` were not read. Every entry below is a place where I had a
question, went to the documentation a real outside user has — `pcomm <verb>`
with no arguments, `pcomm list`, `pcomm types`, `libpcomm/schema/pcomm.fbs`,
`libpcomm/schema/README.md`, `demos/entity-map.html`, the derived-table
contracts in `apps/BRIEF-BLACKBOX.md`, and the assertions in
`tests/hpc/cases/*/asserts/` — and did not find the answer.

Ordered by what each one cost me. Nine of the twelve carry a measurement, so
none of them is a matter of taste.

---

## Q1 — `pcomm types` does not list a type the schema declares, and `pcomm types` is where a user is told to look

**The most consequential one, because it is the one document that is supposed
to be generated rather than written.**

`libpcomm/schema/pcomm.fbs:210` declares, in as many words:

> type 528, Artifact. What a device ORDINAL actually refers to, on this rank.

and goes on to explain why it exists — a bus id repeats across nodes, so a
device ordinal alone cannot identify a GPU in a multi-node job. `pcomm types`
is documented as *"type-id -> table name, for this build"*. Measured, on the
login node, before any capture existed:

```
$ pcomm types | wc -l
60                                  # a header plus 59 rows
$ pcomm types | grep -E '^(524|526|528|530)'
524  artifact  node_identity
526  entity    peer_mapping
530  event     gpu_kernel
```

524, 526 and 530 are there. **528 is not.** I rebuilt the whole tool from the
current checkout to rule out a stale binary — the shared build was an hour older
than `HEAD` — and the new list is byte-for-byte identical to the old one.

So a user reading the schema learns that a record exists which answers exactly
the question a four-node job raises, asks the tool for its table name, and is
told it does not exist. I could not tell from outside whether the record is
unimplemented, implemented but unregistered in the type list, or emitted under
another name; all three look the same from here, and they have completely
different fixes.

**Ask:** either generate `pcomm types` from the schema, or make the schema say
which types this build actually carries. A hand-maintained list of the types a
build has is a list kept in step with the build by nobody.

## Q2 — `comm_edges.unit` reads `datatype-units`, and nothing says what a datatype unit is

Every row of `comm_edges` on a UCX capture reads

```
src_rank,dst_rank,transport,unit,scope,sends,bytes
0,12,ucx,datatype-units,network,120,10239889
```

The column is called `bytes` and the unit is called `datatype-units`. Those are
different claims and the row asserts both.

**Why it matters here and not only in principle.** `ucx_tag_send.count` is
documented as "the raw UCP datatype count", and whether that is bytes depends on
the *caller*: Open MPI pre-multiplies count by the element size for a contiguous
power-of-two datatype (`pml_ucx_datatype.h:87`, `size_shift = log2(size)`) and
does **not** for a contiguous type of non-power-of-two size, where the count
stays in elements. LAMMPS sends `MPI_DOUBLE` throughout
(`comm_brick.cpp`/`comm_kokkos.cpp`), which is 8 bytes and a power of two, so
here the two coincide — by luck. **An application that sends
`MPI_Type_contiguous(3, MPI_FLOAT)` — the natural way to send a coordinate
array, and what GROMACS does — would put element counts in a column named
`bytes`, off by a factor of twelve, with `unit` still reading `datatype-units`.**

I could not determine this from the capture. I determined it by reading Open
MPI's source, which is not pcomm's documentation and works for no other MPI.

**Ask:** state, in the `comm_edges` contract, what `datatype-units` means and
which values `unit` can take; or record the resolved byte length beside the
count so the question does not arise.

## Q3 — The vocabulary of `link_health.finding` is unpublished, so a clean capture is unrecognisable

`link_health` returned 18 rows, all with `n = 0`. That is what the corrected
brief says health looks like. But **without the list of finding kinds a reader
cannot tell a healthy capture from one where half the rubric silently failed to
run** — `n = 0` on five rows and `n = 0` on eighteen rows look equally clean, and
the only defence available to an assertion is "the table is not empty", which is
what mine does.

This is not hypothetical here. 04_llmc measured `rank_identity.device` NULL on
every rank of every capture while `link_health` reported all eighteen findings at
zero. A rubric that is silent about a null key column is a rubric with a gap in
it, and from outside there is no way to know which gaps exist.

**Ask:** publish the finding names, and let `link_health` be checkable for
completeness as well as for zeroes.

## Q4 — The CSV export decodes a version field and the Parquet export does not, and the derived layer is Parquet-only

`uct_binding` in the **CSV** export:

```
ticks,compiled_version,runtime_version,ops_size,accepted,forced
1536515444763391,1.15.0,1.20.0,400,0,0
```

The **same field**, same capture, in the Parquet export:

```
rank,...,compiled_version,runtime_version,ops_size,accepted,forced,pad0,pad1
7,...,69376,70656,400,0,0,0,0
```

69376 is `0x010F00` and 70656 is `0x011400`, so the encoding is
`(major << 16) | (minor << 8) | patch` — which I worked out by hand, from two
numbers whose decoded forms I happened to have side by side. An assertion
written against `compiled_version = '1.20.0'` matches **nothing** on the
representation where the derived tables live, and passes vacuously.

This is the same split 01_gromacs recorded for `op` and `tag`, and 04_llmc for
`op` and `datatype`. It is now four record types and three studies. The sharp
edge is structural: `pcomm link` accepts **only** Parquet, so every assertion
about the derived layer lives on the representation where enums are integers,
while the CSV export — the one that decodes them — has no derived tables at all.

**Ask:** one sentence somewhere saying which export decodes what. Better: decode
in both, or in neither.

## Q5 — `pcomm sql` cannot run an ordinary `.sql` file, and the two warts compound

Documented: a statement whose first token is a comment is rejected. Undocumented:
`pcomm sql` wraps the text in `COPY (<query>) TO '<tmp>.csv'`, so a **trailing
semicolon** is a `Parser Error` too — and every `asserts/*.sql` in
`tests/hpc/cases/` ends with one. Round 1 of this case recorded that.

Round 2 found the third member of the family. Because there is no documented way
to get a `params` table into a `pcomm sql` query, a runner has to splice one in
as a leading CTE — and then **any assertion that opens with its own `WITH`
becomes a syntax error**:

```
WITH params AS (...) WITH universe AS (
                     ^ Parser Error
```

That killed five of my nine assertions on the first job. The workaround (turn the
file's leading `WITH` into a comma) is three lines and had to be invented twice,
once here and once in 01_gromacs's runner, which sidesteps it by shelling out to
a `duckdb` binary that is not on MN5's PATH.

**Ask:** does `pcomm sql` accept more than one statement? If it did, an
assertion could materialise its own `params` and no runner would be needed at
all. Nothing says, and I wrote around the question rather than depending on the
answer.

## Q6 — There is still no documented way to run a params-based assertion against the derived layer

Round 1 found half of this: `suite/sql_check.sh` builds `params` from a closed
fifteen-name column list (`ranks, nodes, gpus, iters, msg_bytes, elems, ...`) and
rejects the **whole** meta.json for any other key. This case's oracle is a
processor grid, a ghost-slab share and a placement policy. None of those are in
that vocabulary.

Round 2 hits the other half: `sql_check.sh` loads a **CSV** export into sqlite3,
and `comm_edges`, `transport_mix`, `rank_identity` and `link_health` exist only
in the **Parquet** export. So the four tables this round exists to test cannot be
reached by the harness the suite ships, with or without params.

This case is now the **third** to write its own runner (01_gromacs and 04_llmc
are the others), and the three do not agree: one shells out to `duckdb`, one uses
`pcomm sql` with a CTE, one materialises params differently again. That is a
harness gap, not three coincidences.

## Q7 — Nothing in a capture records what the tracer was asked to do

This round's entire method is "run the same workload twice with one thing
changed and diff the captures". The two captures differ **only** in the mpirun
placement policy, and **nothing inside either of them says so**. Not the `-p`
selection, not the command line, not the launcher arguments, not the environment.
I know which is which because of the directory name I chose.

04_llmc recorded the same gap for an environment variable. For a round whose
deliverable is a comparison, provenance is not a nicety.

## Q8 — `transport_mix` is keyed by rank, so it cannot say who the peer was

The contract is
`transport_mix(rank, tl_name, dev_name, scope, op, ops, out_bytes, in_bytes, advertised_bytes, header_bytes)`.
There is no peer column. So it answers "how many bytes left this node" and never
"to whom", and the two halves of this study's headline have to be joined **by
the reader**: `comm_edges` gives the peer and the scope in rank space,
`transport_mix` gives the transport and the scope in wire space, and no query
relates a particular edge to the transport that carried it.

That looks like a deliberate design choice and a defensible one — the UCP-to-UCT
join is the join that does not exist. But it should be stated, because the
natural next question after "55.7 % of my bytes left the node" is "which of my
four peers were they going to", and the answer is that the tables cannot be
joined.

## Q9 — `tag_flow.status` has a third value nobody documented, and it is the only one this application produces

Measured: one status, on every rank, on both arms.

```
status,flows,ucp_sends,uct_ops
no_tagged_wire_op,527,13589,0
```

Case 21's assertions name `tag_matched` and `wire_only`. `no_tagged_wire_op` is
neither, and I found it by running the query. It appears to mean "this UCP send
reached no *tagged* wire operation", which for MN5 is the normal state — the
ConnectX hardware tag-matching doors are off by default, so `uct_send.tag` is
never written. But a user seeing 527 flows in a state whose name is not in any
document cannot tell "the fabric was not asked" from "the join is broken".

**Ask:** publish `tag_flow.status` and `tag_recv_resolved.status`. Both are
three-line additions to the brief and both are currently reverse-engineered from
one test file.

## Q10 — `uct_binding` is the best-designed record in the capture and nothing points at it

Credit where it is due, filed as a documentation gap rather than a defect.

When `ucx_deep`'s layout guard refuses, the symptom is: `uct_send` **absent from
the Parquet export entirely**, `transport_mix` present and **empty**, every
assertion naming `uct_send` reporting `[skip]`, and `link_health` reporting
eighteen findings at zero. Every one of those reads as "the fabric was quiet".

`uct_binding` says exactly what happened —
`compiled_version 1.15.0, runtime_version 1.20.0, ops_size 400, accepted 0,
forced 0` — and it is the *only* thing that does. But nothing in
`BRIEF-BLACKBOX.md` mentions the record, the derived-table list does not include
it, and I found it only because 01_gromacs's job script happens to `pcomm dump
<capture> 806` before anything else and I copied the habit.

**Ask:** put "read `uct_binding` first on any `ucx_deep` capture" in the brief,
and consider making a refused binding a `link_health` finding — it is precisely
the class of thing that rubric exists for, and it is currently invisible to it.

## Q11 — `rank_identity.devices` and `.label` still have no published meaning

04_llmc asked this and it is unanswered. Measured here: `devices = 1` on every
rank and `label = 'n0 · GPU 0'` … `'n3 · GPU 3'`. So `devices` is plausibly "how
many devices this rank used" — but at 04_llmc it was `0` alongside a null
`device`, which is consistent with that reading and also with three others, and
`label` is plausibly a display string that nothing should join on. I asserted
against neither, which is the right call and also means two of six columns in
the table went unchecked.

## Q12 — The one where the capture was right and I was wrong, recorded as a documentation ask

`comm_edges.sends` counts every UCP send on the edge. My oracle predicted a
2 : 2 : 1 : 1 split across the four halo peers from LAMMPS's swap table, and it
is wrong — Open MPI's recursive-doubling collective, behind LAMMPS's thermo
`Allreduce`, has partner set `{r^1, r^2, r^4, r^8}` for rank `r`, which overlaps
the halo peer set on three of its four members. Rank 0's edge to peer 4 carries
279 sends where the halo accounts for ~120, and its edge to peer **8** — not a
grid neighbour at all — carries 160 sends of 10,268 B.

The byte split is untouched (10 kB against 35.6 MB), which is why the byte
assertion passes at ±0.2 points while the count assertion cannot.

**The ask this generates is real:** `comm_edges` aggregates away the message
sizes, so there is no way to separate a halo population from a collective
population *inside the derived layer*. Doing it requires dropping back to
`ucx_tag_send` with a size floor and re-doing the peer join by hand — i.e.
un-deriving the derived table. A `comm_edges` that carried `min_bytes`/`max_bytes`
alongside `sends`, or a size histogram, would have made this a one-line
assertion instead of a wrong prediction.
