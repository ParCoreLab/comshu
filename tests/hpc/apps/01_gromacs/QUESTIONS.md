# QUESTIONS — what the capture and its documentation could not tell me

Round 2, 01_gromacs. Written under the black-box rule: `providers/`,
`libcomshu/` (beyond `schema/`), `comshu-read/`, `tools/`, `plans/` and
`BUGS-FOUND.md` were not read. Everything below is a place where I had a
question, went to the documentation a real user has, and did not find the
answer. Some I worked around; the workaround is recorded because its cost is the
measure of the gap.

**No capture exists yet** — MN5 has been unreachable since ~17:00 on 2026-08-29.
So this is the list of things I could not answer *while writing the study*, which
is the more interesting half: a user hits these before they ever get a result.

Ordered by what they cost.

---

## Q1 — Six enum fields are declared as `uint8` with their vocabulary in files the reader does not have

**Confirms the sibling report, and sharpens it.** The schema is not silent by
oversight; it is silent *against its own stated principle*. `comshu.fbs:45`
argues, at length and correctly, that enums must be declared rather than
described:

> These enums mirror `comshu/core/vocab.hpp` [...] They are declared here rather
> than described in a comment on purpose: a reordered C++ enumerator changes no
> size and no field offset, so the byte-level conformance test cannot see it, and
> prose cannot be checked at all.

Four enums get that treatment — `TimeKind`, `MemOp`, `MemSpace`,
`EdgePredicate`. Six fields do not, and three of them are pointed explicitly at
files the black-box rule forbids:

| field | what the schema says | declared? |
|---|---|---|
| `UctSend.op` | "see `UctOp` in `providers/ucx_deep/ucx_deep_records.hpp`" | no |
| `UcxAtomic.op` | "see `UcxAtomicOp` in `providers/ucx/ucx_records.hpp`" | no |
| `UcxRma.op` | "`op` discriminates (`RmaOp`)" — `RmaOp` is declared nowhere | no |
| `UcxTagSend.door`, `UcxTagRecv.door`, `UcxRma.door`, `UcxAtomic.door` | nothing at all | no |
| `UcxTagSend.status_class`, and four more | nothing at all | no |
| `UctIface.open_mode:uint64` | nothing at all | no |

`open_mode` is the one nobody has flagged and it is the worst of them: a 64-bit
field, presumably a bitmask of `UCT_IFACE_OPEN_MODE_*`, with no vocabulary
anywhere in the reader's world and no comment saying it is a bitmask.

**What it cost me.** The `op` vocabulary I have — `am_short`, `put_short`(4),
`put_bcopy`(5), `put_zcopy`(6), `get_short`(7), `get_bcopy`(8), `get_zcopy`(9),
`tag_eager_short`(10), `tag_eager_bcopy`(11), `tag_eager_zcopy`(12),
`tag_rndv_zcopy`(13) — I reconstructed from the *assertions of case 21*, which
happen to name both the strings and the integers. That is a lucky accident of one
test file, not documentation. Four of the fourteen doors (0-3) I still cannot
name. `door` and `status_class` I never recovered at all: the only `door` value I
know is `'nbi'`, from a case-22 assertion.

**And the vocabulary changes representation between exports.** Case 21's
`tag_and_addr_are_door_scoped.sql` documents the consequence in its own comment:
the CSV export decodes `op` to a name while the Parquet export leaves the integer,
so an assertion written for one **matches no row on the other and passes
vacuously**. This is independently confirmed, and it is not confined to enums —
`tag` comes out of CSV as a hex string and out of Parquet as an integer, so every
tag-arithmetic assertion in `asserts-r2/` had to carry an explicit decode canary
to keep a wrong-export run from reporting success. Three of my eleven assertion
files carry that canary. It is pure overhead created by an undocumented
representation split.

**Ask:** declare all six in `comshu.fbs`, exactly as the file's own comment
argues. And state in one place which export decodes and which does not.

---

## Q2 — Nothing joins a UCX operation to the peer rank it touched

**Confirms the sibling report for one-sided, and extends it: it is true for
two-sided as well, and the documented route is broken on the commonest MPI.**

The documented path is `ucx_rma.endpoint` -> `ucx_ep_connection.remote_prefix_hash`
-> another rank's `ucx_worker_address.prefix_hash` -> that rank. Round 1 measured
it end to end on this exact stack and it does not work: Open MPI 4.1.x publishes
its worker address through `ucp_worker_query`, which is not a hooked door, so
`ucx_worker_address` records the wrong one of the two workers each rank creates
and 64 of 117 endpoints resolve, none of them carrying traffic. That is round 1's
F2 and I have no reason to think it has changed.

Three further things the sibling's version does not say:

1. **`transport_mix` cannot supply the peer either.** It is keyed by `rank`, not
   by edge, so it answers "how many bytes left this node" and never "to whom".
   That looks like a deliberate design choice and a defensible one — but it means
   that on the HPC-X stack, where `comm_edges` is structurally empty, **no table
   in comshu can name the far end of a transfer.**
2. **For two-sided traffic there is a route around it, and it is not comshu's.**
   Open MPI encodes the peer in the tag: `PML_UCX_MAKE_SEND_TAG` puts
   `comm->c_my_rank` in bits 20-39 (`pml_ucx_request.h:42`). So `ucx_tag_send.tag`
   identifies the sender and `ucx_tag_recv.tag` identifies the requested source,
   with no cross-rank join at all. My topology assertion is built on this. It is
   a fact about Open MPI, works for no other MPI, and is nowhere in comshu's
   documentation — a user would have to go and read the MPI's source, as I did.
3. **For one-sided traffic there is no such route**, because an RMA has no tag.
   So the sibling's gap (b) stands unmitigated, and it is the harder half.

**Ask:** hook `ucp_worker_query` and `ucp_worker_get_address_v2` as address
doors. Round 1 already named this as a one-hook fix. Failing that, document that
peer resolution does not work on Open MPI 4.1.x, because the tables are all
present and simply resolve to the wrong thing — which is the failure mode a user
cannot detect.

---

## Q3 — The unit of `count` is not recoverable from the capture, and for `UcxRma` it depends on an undocumented field

**Confirms the sibling report and makes it worse.**

`UcxTagSend.count` is documented — "the raw UCP datatype count" — and the
documentation is *correct*. It is also unusable, because **nothing in the record
says what the datatype was.** `ucp_tag_send_nbx` takes count and datatype
separately, `UcxTagSend` keeps the first and drops the second, so a reader
holding a row cannot convert it to bytes.

Whether that matters depends on the caller, and I had to read Open MPI to find
out:

```
pml_ucx.c:824        ucp_tag_send_nbx(ep, buf, mca_pml_ucx_get_data_size(op_data, count), tag, param)
pml_ucx_datatype.h:87  return count << op_data->size_shift;
pml_ucx_datatype.c:201 is_contig_pow2 -> size_shift = log2(size), datatype left contig(1)
                       else            -> size_shift = 0,          datatype = contig(size)
```

So for a contiguous datatype of power-of-two size — `MPI_BYTE`, `MPI_FLOAT`,
`MPI_DOUBLE` — Open MPI pre-multiplies and `count` **is bytes**. For a contiguous
type of non-power-of-two size — `MPI_Type_contiguous(3, MPI_FLOAT)`, 12 bytes,
which is the natural way to send an `rvec` array — it does not, and `count` is
**elements**. The two rows are byte-identical in the capture and differ by a
factor of twelve.

Round 1's headline volume result (25,662,688,388 B against 25,662,580,944
predicted, four parts per million) is correct *and* rests on this accident
without knowing it. Case 21's `meta.json` says it chose `MPI_UNSIGNED_CHAR`
throughout precisely so that count and bytes coincide — sound practice for a
microbenchmark, and exactly what a real application will not do for you.

**`UcxRma.count` is worse: it has no unit comment at all, and its unit depends
on `door`.** `ucp_put_nbi(ep, buffer, length, remote_addr, rkey)` takes a byte
length; `ucp_put_nbx` takes a count plus a datatype. Same field, two meanings,
discriminated by `door` — whose vocabulary is unpublished (Q1). So the sibling's
"no documented unit" is really "the unit is a function of a field you cannot
decode".

**Ask:** record the UCP datatype (or the resolved byte length) beside `count`,
and state `UcxRma.count`'s unit per door.

---

## Q4 — `transport_mix`'s columns were undocumented until this round, and three of them still are

The column list was missing from the brief and was published on 2026-08-30, mid
study, after the 03_quantumespresso agent had reverse-engineered eight of ten
columns out of case 21's tests and then fallen back on matching `tl_name` against
`'%mlx5%'`. I was three assertion files down the same road. Recording it here
because the fix came from **outside the artefact set**: the only places in the
repo where those columns appear are the tool's own rule tests, which are not on
the reader's allowed list and which a user of a released tool would not have.

Three column meanings are still undefined anywhere I can read:

* **`in_bytes`** — inbound payload of a `get_*`? Also the receiving side of a
  rendezvous pull? My headline assertion sums `out_bytes + in_bytes` on the
  argument that each is real wire traffic in one direction and the sum counts
  each transfer once. **If `in_bytes` is instead a duplicate accounting of
  `out_bytes` at the far end, my fraction is computed over a doubled
  denominator.** I could not settle this and the assertion carries the risk.
* **`advertised_bytes`** — I inferred, from case 21's
  `rendezvous_advertises_rather_than_sends.sql`, that it must not be folded into
  `out_bytes` because that bills the same buffer twice. That is the right rule
  and it lives in an assertion comment, not a contract.
* **`ops`** — a count of `uct_send` rows, or of something coarser?

The underlying question is `UctSend.payload`, whose schema comment says it
"means sent, requested or advertised depending on the op family, and must not be
summed across them" — **and then names the op families in the forbidden file.**
`transport_mix`'s three byte columns are presumably that resolution made
explicit, which is a good design; it just is not written down.

---

## Q5 — There is no documented way to run a `params`-based assertion against the derived layer

Round 1 found half of this (its F5): `suite/sql_check.sh` builds `params` from a
hard-coded fifteen-name column list and rejects `npp`, `npme`, `natoms`. It
shipped `asserts.sh` to fix that.

The other half only appears once you query a *derived* table, which round 1 never
did:

* `comshu export --csv` emits one CSV per **record type** — the raw 600s and 800s.
* `transport_mix`, `tag_flow`, `tag_recv_resolved` and `link_health` are built by
  `comshu link` and exist only in the **Parquet** export.
* `asserts.sh` loads CSV into **sqlite3**.

So the four tables this entire round exists to test cannot be reached by the
runner this case already ships, and no documented runner reaches them *with* a
params table. I wrote `run-asserts-r2.sh` (DuckDB over the parquet export, same
`-- requires:` contract) to close it, which is a second private runner in one
case directory and a sign the harness wants the change upstream.

Related and unresolved: **does `comshu sql` accept more than one statement?** If
it did, an assertion could materialise `params` itself as a CTE or a view and
need no runner at all. Nothing says. My runner shells out to `duckdb` directly to
avoid depending on the answer, which in turn hard-codes the assumption that the
derived export is a directory of `<name>.parquet` files — also undocumented.

---

## Q6 — The shipped entity map contradicts the tables that now exist

`demos/entity-map.html` is on the allowed reading list and is the document that
explains what the records mean. Two of its statements are false as of this round:

* *"cupti (300-399), libfabric (400-499) and `ucx_deep` (800-899) are allocated
  in `ids.hpp` and **have no records**."* — `ucx_deep` has four, and they are in
  the same page's own index table, twenty lines above.
* *"**Nothing joins the two bands**: a `ucx_tag_send` and the `uct_send` rows it
  became are separate facts."* — this is the figure caption for the UCX section,
  and `tag_flow` is described in the brief as exactly that join, keyed on the
  tag.

A reader who trusts the entity map concludes that the question `tag_flow` answers
cannot be answered. The page's footer says it is generated by
`tools/gen_figures.py`, so the captions are hand-written prose inside a generator
and nothing checks them against the schema they claim to describe.

**This is the same shape of defect the brief itself records for `link_health`** —
a head comment saying the table should be empty, contradicted by the SQL beneath
it that emits a zero-valued rubric row per finding kind. Two independent
instances of documentation disagreeing with implementation, in the two documents
a black-box reader is told to rely on, is a pattern rather than a coincidence.

---

## Q7 — Three derived-table vocabularies are unpublished, and a "clean" result is therefore unrecognisable

Same shape as Q1, one layer up.

* **`tag_flow.status`** — I know `'tag_matched'` and `'wire_only'` from case 21's
  assertions. Is there a third for a UCP send that reached no wire operation? My
  prediction is that GROMACS produces *only* that third state, so I am asserting
  around a value whose name I do not know.
* **`tag_recv_resolved.status`** — completely unknown. I do not know what
  distinguishes a resolved row from an unresolved one, so I cannot write the
  assertion I actually want ("every receive that arrived by handle got a
  length"). I fell back on the pure invariant `delivered <= offered`, which is
  real but weak.
* **`link_health.finding`** — unknown. The brief now says a healthy capture has
  every row present with `n = 0`, which is exactly right and was corrected this
  round. But **without the list of finding kinds a reader cannot tell a healthy
  capture from one where half the rubric silently failed to run.** `n = 0` on
  five rows and `n = 0` on twenty rows look equally clean. The only finding name I
  know is `unnamed_uct_transport`, from case 21.

**Ask:** publish the three vocabularies, and have `link_health` be checkable for
*completeness* as well as for zeroes.

---

## Q8 — Two questions only a run can answer, recorded so the run is not wasted

Not documentation gaps — genuine unknowns that the job script is built to settle.
Listed so that whoever gets the slot knows these are the load-bearing ones.

1. **Does `uct_binding.accepted = 1` against UCX 1.15?** `ucx_deep` patches
   `uct_iface_ops_t`, an installed header with no ABI promise, guarded by a table
   size comparison. Case 21 measured UCX 1.18 against a 1.19 build. HPC-X 2.16
   ships **1.15**, three minor releases older than anything this layer has met,
   and it is the UCX the application's own MPI was built for. If the guard
   refuses, every assertion in `asserts-r2/` skips on an empty table and the round
   produces nothing. This is the first thing to read in the output.
2. **Does `ucx_rkey` fill up on a workload that issues no one-sided calls?** UCP
   rendezvous exchanges a packed remote key on every large transfer and every
   GROMACS message here is large. Whether that internal unpack passes through the
   public `ucp_ep_rkey_unpack` symbol a provider can interpose is not something
   any available document says. If it does, the capture holds a population of
   remote keys belonging to no recorded operation — not wrong, but
   unattributable, and worth naming.

---

## Not confirmed

The sibling report that **`rank_identity.device` is NULL and `label` reads
"GPU ?" without the gpumem provider** I can neither confirm nor contradict: my
capture does not exist yet, and `rank_identity` is not a table this study
queries. Flagging it as untested by me rather than letting silence read as
agreement. It is plausible on its face — `NodeIdentity` carries the node half of
the (node, device) pair and nothing in the `ucx`/`ucx_deep` ranges carries a
device ordinal at all, so a `-p ucx,ucx_deep` capture has no source for one.
