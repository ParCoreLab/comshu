# QUESTIONS — what the capture and its documentation could not tell me

Round 2, Quantum ESPRESSO, black-box. Written from the allowed reading list
only: `BRIEF-BLACKBOX.md`, `apps/README.md`, `libcomshu/schema/comshu.fbs` and its
`README.md`, `demos/entity-map.html` and `demos/figures/*.svg`,
`tests/hpc/cases/*/asserts/*.sql`, `tests/hpc/suite/README.md`, and QE's own
source. No file under `providers/`, `libcomshu/` (beyond the schema),
`comshu-read/`, `tools/`, `plans/` or `BUGS-FOUND.md` was opened.

This is a deliverable, not an apology. Every item below cost real design effort
in this study and each one will cost the next outside user the same.

Ranked by how much it distorted the work.

---

## 1. `transport_mix` has no published column contract, and it is the headline table

`BRIEF-BLACKBOX.md` gives a column list for every derived table except two:

```
comm_edges         src_rank,dst_rank,transport,unit,sends,bytes
endpoint           rank,qp_num,lid,peer_lid,peer_qp_num,peer_rank,transport,sends,bytes
...
transport_mix      (which transport actually carried it)      <- prose, no columns
tag_flow           rank,tag,status,ucp_sends,ucp_units,uct_ops,uct_payload,doors
```

`demos/figures/12-derived-tables.svg` repeats the same one-line prose.
`entity-map.html` says the derived tables "carry no schema" — which explains why
there is no *format*, but not why there is no *column list*, since seven of the
nine got one.

I recovered the columns — `rank, scope, tl_name, op, ops, out_bytes, in_bytes,
advertised_bytes` — by reading `21_ucx_deep`'s assertions and its `job.sbatch`.
That is recovering an interface from its tests. It works, and it means the
contract is whatever those two files happen to select today.

**Ask:** give `transport_mix` the same one-line column contract the other seven
have.

## 2. `scope`'s vocabulary is unpublished, and it is exactly the column my study needed

`transport_mix.scope` is visibly the column that answers *did this message leave
the node*, which is this entire study's question. Its permitted values appear
nowhere I may read. Case 21 selects it and never compares it to a literal.

So my headline assertion binds to `tl_name LIKE '%mlx5%' OR tl_name LIKE
'%verbs%'` instead — i.e. to **UCX's** transport-name vocabulary rather than to
comshu's. That is strictly worse: it is a third party's namespace, it will not
survive a transport family comshu has not met, and it silently degrades to "no
fabric traffic" rather than erroring if `tl_name` is ever exported as an
unresolved intern handle instead of a string. I designed the assertion so that
degradation fails *loudly* in the arm where fabric traffic is predicted, but I
should not have had to.

**Ask:** publish the `scope` value set. Two or three strings in a comment.

## 3. Provider enum vocabularies are not in the schema — **CONFIRMED**

A sibling reported this; I checked it independently and it holds, precisely.

`comshu.fbs` declares exactly five enums — `TimeKind`, `RecordKind`, `MemOp`,
`MemSpace`, `EdgePredicate` — under a heading that argues *why* enums must be
declared rather than described:

> "a reordered C++ enumerator changes no size and no field offset, so the
> byte-level conformance test cannot see it, and prose cannot be checked at all."

That argument applies verbatim to every provider enum, and none of them is
declared. `UcxTagSend.door`, `UcxTagSend.status_class`, `UcxRma.op`,
`UcxRma.door`, `UcxAtomic.op`, `UctSend.op`, `NcclCollective.op/datatype/redop`,
`NcclP2p.dir`, `GpuKernel.source` are all bare `uint8`, and each comment that
explains one points at a header outside the allowed list (`UctOp in
providers/ucx_deep/ucx_deep_records.hpp`, `UcxAtomicOp in
providers/ucx/ucx_records.hpp`, `KernelSource in core/vocab.hpp`).

What I could actually reconstruct, and from where:

| field | values I could recover | source |
|---|---|---|
| `UctSend.op` | 4–9 = `put_short/bcopy/zcopy`, `get_short/bcopy/zcopy`; 10–13 = `tag_eager_short/bcopy/zcopy`, `tag_rndv_zcopy`; `am_short`, `am_zcopy` exist | `21_ucx_deep/asserts/*.sql` |
| `UctSend.op` 0–3 | **could not recover at all** | — |
| `UcxTagSend.door` | `sync_nb` | `22_ucx_rma/asserts/the_static_only_doors_fired.sql` |
| `UcxRma.door` | `nbi` | same |
| `UcxAtomic.op` | `add`, `cswap`, `swap` | same |
| `status_class` | `error` | same |

The repository says "fourteen doors"; I can name ten. Four op codes have no
recoverable meaning from any readable file, and a query over them cannot be
written at all.

**Ask:** declare them in `comshu.fbs`, by the schema's own stated argument.

## 4. CSV decodes, Parquet does not — **CONFIRMED**, and documented in the wrong place

True, and the only statement of it I found is inside a comment in
`21_ucx_deep/asserts/tag_and_addr_are_door_scoped.sql`:

> "The CSV export DECODES — `op` is 'am_short' and `tag` is '0x1a2b' — while the
> Parquet export deliberately does not, so the same columns are a UTINYINT and a
> UBIGINT there. `sql_check.sh` prefers Parquet and falls back to CSV, so an
> assertion written for one silently matches no row on the other and passes
> vacuously."

That is the clearest statement of the hazard in the repository and it lives in a
test for one case. **Cost to this study, measured:** every assertion of mine that
names a door had to be written twice, as numbers *and* as names
(`tag_flow_closes_on_a_real_app-r2.sql`). And it reaches beyond enums: I do not
know whether the CSV renders a plain `uint64` like `UcxTagSend.count` in decimal
or in hex, so `fft_transpose_message_size-r2.sql` — the byte oracle this whole
study turns on — compares against **both** the decimal integer and the hex
spelling. Both branches are unit-tested against a synthetic database because I
cannot tell from documentation which one will be live.

**Ask:** state the per-export rendering rule once, in `BRIEF-BLACKBOX.md` beside
the export verbs, and say which columns are hex.

## 5. `UcxRma.count` has no documented unit — **CONFIRMED**, and it is worse than reported

The sibling is right, and the shape of it is worth stating exactly, because the
UCX family is inconsistent rather than silent:

| record | what the schema says about `count` |
|---|---|
| `UcxTagSend` (606) | "count is the raw UCP datatype count" |
| `UcxTagRecv` (608) | "count is the capacity offered, not bytes delivered" |
| `UcxTagProbe` (618) | `length` is "the one true byte count in the UCX family" |
| `UcxAmSend` (612) | *nothing* |
| **`UcxRma` (610)** | **nothing at all** — "A one-sided put or get; `op` discriminates" |

And `UcxRma` carries **no datatype field**, so the unit cannot be recovered from
the record either. `ucp_put_nb`-family calls take a byte length, so it is
probably bytes — but "probably" is what a schema is for.

There is a second-order cost the project has already paid without naming it:
`19_ucx_mpi` and `22_ucx_rma` both choose `MPI_UNSIGNED_CHAR` for their
workloads *specifically so that count and bytes coincide*, and both say so in
their `workload_note`. **A real application cannot do that.** QE sends
`MPI_DOUBLE_COMPLEX`. My byte oracle survives only because I derive the *element*
count from QE's grid and compare against `count` directly, never mixing the two —
and if `UcxTagSend.count` had turned out to be bytes rather than elements for
this datatype, round 1's measurement (which found `count` == the `ibv_reg_mr`
length in bytes) and the schema's own words would be in direct contradiction.
Round 1 flagged that and it is still unresolved.

**Ask:** one clause on `UcxRma.count` and `UcxAmSend.count`; and reconcile
`UcxTagSend.count`'s stated meaning with round 1's measurement.

## 6. One-sided op → peer rank: **PARTIALLY CONTRADICTED**

The sibling reports there is no documented join. I find a documented *chain* but
no documented *table*, and the distinction matters:

* `ucx_rma` names an `endpoint` (`comshu.fbs` 610).
* `ucx_ep_connection` (624) maps an endpoint to a `remote_prefix_hash`.
* `demos/figures/11-cross-rank-keys.svg` names `prefix_hash` as one of exactly
  five cross-rank keys: "the dialled worker, found in the other rank's capture",
  joining to `ucx_worker_address.prefix_hash` (622).

So the join is documented, end to end, on the figure whose whole subject is
cross-rank joins. What is **not** documented is anything that walks it:

* `endpoint` is contracted as "one row per local queue pair" — verbs, not UCX;
* nothing says whether `ucx_rma` contributes rows to `comm_edges`, whose
  contract is `src_rank,dst_rank,transport,unit,sends,bytes` with no statement of
  which record types feed it;
* `transport_mix` and `tag_flow` are both keyed by `rank`, never by peer.

And on the OpenMPI point-to-point path the key is **empty** regardless, for round
1's measured reason (`mca_pml_ucx.so` publishes through `ucp_worker_query`;
comshu hooks `ucp_worker_get_address`). So a user follows a documented chain to a
key no capture of an MPI application contains.

**Not testable on QE**: QE's parallel layer is two-sided and `ucx_rma` is
predicted empty. `every_rma_key_resolves-r2.sql` is included anyway, passing
vacuously, with this reasoning in its header so the next reader finds it.

## 7. What `-m simple` keeps is undiscoverable from outside

`comshu run -m simple` is the documented way to cut volume, and round 1 used it
to drop 1.9 M `ucx_worker_progress` rows — citing `core/vocab.hpp:79` for what
`simple` retains. That file is not readable from outside the project, and no
CLI help, schema comment, figure or README states whether `simple` keeps
`uct_send`.

`uct_send` is this round's headline table. Guessing wrong costs the allocation
and produces an empty table with an innocent-looking explanation. **So I run at
the default mode and pay the volume**, which is the wrong trade made for the
right reason.

**Ask:** `comshu run` with no arguments should list the modes and what each drops.

## 8. Whether `ucx_deep` will engage at all cannot be determined before the run

`ucx_deep` patches `uct_iface_ops_t` and guards on the ops-table size;
`comshu.fbs` type 806 says "`accepted = 0` is why `uct_send` is empty". Case 21
measured runtime 1.18.0 against a 1.19.0 build.

QE on MN5 loads **HPC-X 2.16 / UCX 1.15** — older than anything this provider
has met — and the UCX provider must sit in front of *that* libucp, so the newer
runtime is not an option (round 1's NOTES). Nothing tells me offline whether
1.15's ops table is the size the guard wants. `comshu list` is documented as
saying "which providers exist, what each captures" — not what each *requires*.

I designed around it: `uct_binding_accepted-r2.sql` runs first, `uct_send` and
`transport_mix` are deliberately **not** in `expect_tables`, and P5 in
`PREDICTION.md` is a fallback claim resting only on the 600-range. But a user
should be able to ask "will this provider engage against UCX 1.15" without
spending a queue slot.

**Ask:** have `comshu list` print each provider's supported runtime range.

## 9. `UcxTagSend.device` cannot distinguish a host send from a device-0 send

`comshu.fbs` 606 gives `UcxTagSend` a `device:uint16` and says nothing about what
it means or what value denotes "not on a device". The schema README says
elsewhere, in a different context, that **"`device = 0` is a real device"**.

For a CUDA-aware application this is the single most interesting bit in the
record — QE is built with `-D__GPU_MPI` (`install/m4/x_ac_qe_cuda.m4:92-93`) and
hands MPI *device* pointers, which round 1 proved the expensive way when
`rc_verbs` aborted with `md supports: host`. Whether a given transpose left GPU
memory or a host bounce buffer is precisely the question, and a `uint16` with no
sentinel and no documentation cannot answer it. Under round 1's `--gres=gpu:1`
every rank saw device 0, so the column was constant and uninformative; I ask for
`--gres=gpu:4` partly to make it carry information at all.

**Ask:** document `device`'s meaning and its "none" value.

## 10. `rank_identity.device` — the sibling claim I could not settle offline

Reported: `device` is NULL and `label` reads "GPU ?" on captures without the
gpumem/NVBit provider, because the device is derived only from NVBit records
though other records in the same capture carry it.

I can neither confirm nor contradict this without a capture. `BRIEF-BLACKBOX.md`
gives `rank_identity` the contract `rank,node_ix,node_hash,device,devices,label`
and says nothing about which records populate `device` or when it is absent.

**I predicted the failure in writing** (`PREDICTION.md` P7) and wrote
`rank_identity_names_a_device-r2.sql` as a **live** assertion rather than
pre-marking it `expect_fail`. Marking an assertion xfail on another agent's word
is how a suite ratifies a claim it never checked, and `suite/README.md` is
explicit that `expect_fail_asserts` is for a defect *with evidence*.

## 11. `link_health` — the prose and the queries disagree

`BRIEF-BLACKBOX.md`: "`link_health` is **expected to be empty**. Every row in it
is a defect in the link."

But `21_ucx_deep`'s assertion filters `WHERE finding = '...' AND n > 0`, and its
job script prints `WHERE n > 0` — both of which only make sense if a row with
`n = 0` is a normal entry. Two readings, opposite consequences: under the prose,
`SELECT * FROM link_health` is the assertion; under the queries, `WHERE n > 0`
is, and the prose version would fail every clean capture.

Confirmed by the coordinator mid-study; recorded here because a reader of the
brief alone would get it wrong, exactly as I did.

**Ask:** fix the sentence to "expected to have no row with `n > 0`".

## 12. `tag_flow.status` vocabulary is unpublished

The contract gives the column; nothing gives its values. I know `tag_matched`
and `wire_only` from one case assertion. There is evidently at least a third
(the intent-only case — a UCP send that joined nothing), and without its name I
**cannot write the assertion I actually wanted**: *every UCP tagged send is
accounted for in exactly one flow state*. I can only write "at least one flow
matched", which is much weaker.

This is the sharpest example of the general problem: an unpublished vocabulary
does not merely make assertions awkward, it makes the strong form of the claim
unwriteable.

## 13. There is no verb for the headline claim type

`apps/README.md` names claim type 3 — run twice with one knob changed, diff the
captures — as "the headline: it is the thing a profiler is for". The CLI has no
way to express it. `comshu sql` takes one export.

Round 1 wrote its own `check.sh` to attach two SQLite databases; I wrote
`check-r2.sh` for the same reason and solved it differently (each assertion
branches on `params.npool < params.nodes`, so the same file makes the opposite
claim in each arm and stays a single-capture statement). Both are workarounds
for a missing verb, and every future app study will write a third.

**Ask:** `comshu sql <exportA> <exportB>` with a `capture` column, or a documented
recipe for the DuckDB attach.

## 14. `mem_object` has no door discriminator — confirmed against a second application

`apps/README.md` states this as a known gap; QE confirms it is decisive. The
managed-memory question (`plans/todos.txt` item 1) **cannot be answered from a
capture at all** — a managed allocation and a device allocation are
byte-identical rows — so the evidence has to come from the binary. For QE it
does, three independent ways (`meta-r2.json`'s `managed_memory_note`), and the
answer is **no managed memory**.

The consequence is worth stating plainly: `apps/README.md` nominated
QuantumESPRESSO and LAMMPS/Kokkos as the two workloads that would make item 2
live. LAMMPS was checked and does not; QE does not either, and QE 7.5's own
build systems contain no flag that could make it. **Item 2 has no candidate
workload left in this corpus.** It stays latent for want of a workload, not for
want of a capture — which is a different and more actionable statement than
"unresolved".

## 15. `entity-map.html` contradicts itself about `ucx_deep`

Under "Where the model is still open":

> "Three ranges are reserved and empty: cupti (300–399), libfabric (400–499) and
> ucx_deep (800–899) are allocated in `ids.hpp` and have no records."

The **same page** lists four `ucx_deep` records (800 `uct_iface`, 802 `uct_ep`,
804 `uct_send`, 806 `uct_binding`) in its index, and figure 09 draws them.

A new user reading the prose first concludes the transport layer does not exist —
which is the opposite of the truth and of this round's entire premise. The page
says it is generated by `tools/gen_figures.py`, so the stale sentence is in a
generator nobody re-read after the 800s landed.

---

## Two things I want to record as *not* problems

* **The `-- requires:` three-state convention is right and I used it unchanged.**
  Missing table, empty table, and absent params column are all "cannot
  evaluate", and none is a failure. On this stack `ib_post_send` is legitimately
  empty and `uct_send` may be; `[skip]` with a reason is the correct report and
  it is what let me keep assertions that are unanswerable today and live the day
  a provider changes.
* **`uct_binding` existing at all is the single best design decision I met in
  this schema.** "`accepted = 0` is why `uct_send` is empty, and saying so is the
  whole reason this record exists" is the difference between a negative result
  and a mystery. Every provider that can decline to engage should have one.
