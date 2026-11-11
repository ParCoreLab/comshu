# QUESTIONS — what the capture and its documentation could not tell me

Written from the black-box position the round-2 brief defines: the CLI, the
record schema (`libcomshu/schema/comshu.fbs` and its README), the derived-table
contracts in `BRIEF-BLACKBOX.md`, other cases' `asserts/*.sql`, and the
application's own source. No `providers/`, no `libcomshu/` beyond the schema, no
`comshu-read/`, no `plans/`.

Every item below cost me something concrete — an assertion I could not write,
one I had to write around, or a claim I had to leave to a printed diagnostic.
They are ordered by what they cost, not by how easy they look to fix.

---

## 1. The enum vocabularies are not published, and the two exports disagree

`UcxRma.op` is `uint8` with the comment *"`op` discriminates (RmaOp)"`.
`UcxRma.door`, `UcxAtomic.op`, `UcxAtomic.door`, `status_class`, `UctSend.op`
are the same: a `uint8` and a pointer to a type that lives in
`providers/ucx/ucx_records.hpp` and `providers/ucx_deep/ucx_deep_records.hpp` —
files I may not read, and which a real outside user does not have either.

The schema README goes out of its way to say that `TimeKind`, `MemOp`,
`MemSpace` and `EdgePredicate` are *real `enum`s here* precisely because a
comment is the one drift the byte-level test cannot see. `RmaOp`,
`UcxAtomicOp`, the door enums and the status classes are exactly the same kind
of value, live on disk in the same way, and are **prose**.

It is made worse by a second fact, which I learned from case 21's assertion
comments and not from any document: **the CSV export decodes these columns to
strings and the Parquet export does not**, and `sql_check.sh` prefers Parquet
and falls back to CSV. So an assertion written against `op = 'put'` silently
matches nothing on one engine and passes vacuously.

*What it cost.* Not one assertion in this study names a door, an op, or a
status class on a raw table. The count oracle filters on **message size**; the
success check tests the **raw signed status** against UCX's documented
`UCS_OK = 0` convention; the atomic checks key on **`op_size`**. The claim I
most wanted to make — *"MPI_Put reached `ucp_put_nbi` and not `ucp_put_nbx`"*,
which is the entire point of the `_nbi` doors having been hooked from a static
survey and never observed — I could not write at all. `job.sbatch` prints the
door histogram instead, and a human has to read it.

*What would fix it.* Declare them as `enum`s in `comshu.fbs` like the other four,
or have `comshu types` print each column's value vocabulary alongside the
type-id → table mapping it already prints.

## 2. Nothing says whether one-sided traffic can be attributed across ranks

This is the largest gap and it is structural, not cosmetic.

A `ucx_rma` row carries `rkey` and `remote_addr`. `ucx_rkey` (626) resolves the
key to `(rkey, endpoint)` — and stops. There is no peer rank, no remote length,
no remote base address, and the schema says the omission of a packed-buffer
hash is *deliberate*: the unpack door takes no length, so a fixed prefix would
be an over-read.

On the other side, `ucx_rkey_pack` (628) carries `(memh, context, buffer_hash,
buffer_len)` — a hash of the whole exported buffer, which the owner can compute
because `ucp_rkey_pack` returns the size.

So the exporting rank has a hash and the importing rank has none. **There is no
documented join from a Put to the rank whose memory it wrote.** For a tagged
send the path exists — `ucx_ep_connection.remote_prefix_hash` to another rank's
`ucx_worker_address.prefix_hash` — and `comm_edges` is built on it. For a
one-sided operation I could find no equivalent, and this is a workload where
*every byte* of application traffic is one-sided.

*What it cost.* I cannot assert that rank 0's 1,000 puts landed in rank 1's
window, which is the single most obvious true statement about this benchmark.
The study asserts referential integrity *within* a rank and stops at the node
boundary.

*Related, and unanswerable from the documentation:* is `ucx_rkey.endpoint` the
local endpoint the key was unpacked on (so that `ucx_ep_connection` would
resolve the peer)? The record does not say, and `ucp_ep_rkey_unpack`'s signature
is not in any file I may read. If it is, the join exists and is simply not
documented — which is its own finding, because I could not discover it.

## 3. What does `UcxRma.count` count?

`UcxTagSend.count` is documented emphatically — *"the raw UCP datatype count"*,
and case 19's `meta.json` builds a whole argument on the point. `UcxRma.count`
has no such note. `ucp_put_nbi` takes a **byte length**; `ucp_put_nbx` takes a
**count plus a datatype**. The same column therefore means different things
depending on which door produced the row, and nothing in the record, the schema
or the derived contracts says which convention applies to a given row.

*What it cost.* I chose a benchmark whose only datatype is `MPI_CHAR`
(`osu_put_bw.c:72` refuses anything else) so that the two readings coincide and
my oracle is true either way. That was a real constraint on the choice of
workload, and a study that needed a wider datatype could not work around it.

## 4. `transport_mix` has no published column list

`BRIEF-BLACKBOX.md` gives column contracts for eight derived tables and for this
one gives a parenthesis: *"(which transport actually carried it)"*. I recovered
`rank`, `tl_name`, `op`, `ops`, `out_bytes`, `in_bytes`, `advertised_bytes` and
`scope` by reading case 21's assertions and its job script — i.e. by reverse-
engineering another test rather than by reading documentation.

*What it cost.* Two assertions in this study query a table whose schema I am
inferring. If a column is named differently the query errors rather than
failing, which reports as a broken test rather than as a broken claim.

## 5. Nothing in a capture records which MPI components ran

`--mca osc ucx` is load-bearing: with `osc/rdma` or `osc/pt2pt` the same
`MPI_Put` becomes an active message, no key is ever unpacked, and `ucx_rma`,
`ucx_rkey` and `ucx_rkey_pack` are all empty. **That is byte-identical to a
tracer that saw nothing.** The capture contains `ucx_context.api_major/minor`
and `uct_binding.runtime_version` — the UCX it bound to — but nothing about the
MPI above it: not the OSC component, not the PML, not the OpenMPI version.

*What it cost.* `job.sbatch` prints `ompi_info` before the runs, because
otherwise the most likely cause of an empty capture is unknowable after the
fact. A `mpi_environment`-style artifact — component names as interned strings,
recorded once per rank — would turn a whole class of empty capture from a
mystery into a one-line explanation.

## 6. Does `comm_edges` include one-sided traffic, and on which side?

The contract is `src_rank, dst_rank, transport, unit, sends, bytes`. There is no
op column and no note about RMA. For this workload the question has a sharp
answer that the documentation does not give: is a Put a `send` from the origin?
Is a Get a `send` from the *target*, which never called anything? Or is
one-sided traffic simply absent from the table?

*What it cost.* I could not write the assertion a reader would most expect —
"the rank matrix is 1 edge, origin to target, 3,174,400 B" — because I cannot
tell whether an empty `comm_edges` would be a defect or the documented
behaviour. `job.sbatch` prints the table for every arm so the answer is at
least recorded.

## 7. `ucx_mem_map.resolved` — when is `length` the length?

The record says `resolved` "says whether the address/length are the effective
ones from `ucp_mem_query` or as requested". It does not say which value the flag
takes for which case, nor which one `MPI_Win_allocate` produces on this stack.

*What it cost.* I wanted an assertion tying the mapped region to the window the
benchmark asked for: `osu_put_bw` allocates `size * window_size` bytes per
window, so the largest window is 8192 × 8 = 65,536 B and a mapped length below
that would mean the record is describing a different allocation. I dropped it,
because a `resolved = 0` row is documented to hold a *requested* length, UCX
rounds registrations up to pages, and I cannot tell from the outside whether
`length < 65536` on such a row is a defect or the record working as designed.
The assertion I could have written is exactly the one that would have caught a
provider reading the length from the wrong argument slot.

*Aside, and to this study's credit rather than its cost:* `EntityClose` **is**
fully documented in the schema (type 522: `subject` is "the interned handle the
open record carries", `opener` is "type_id of the record that should have opened
it"), which is why `rkey_lifetimes_balance` could be written from the
documentation alone. It is the counter-example that shows what the records in
item 1 are missing.

## 8. `object_access` and one-sided operations

The contract is `owner_rank, object, obj_offset, size, source, actor_rank,
actor_device, exact` — an object heat-map keyed by owner. A Put carries exactly
that information (`remote_addr`, `count`, and an owner if the rkey could be
resolved across ranks). Case 21's own comment says `remote_addr` "is the
object-heatmap key the whole HPC-X class has never had". Whether `comshu link`
actually feeds one-sided operations into `object_access`, and what `source`
would say if it did, is not stated anywhere in the contracts.

## 9. How many rkeys *should* one window produce?

I asserted a floor (one key per window) rather than an equality, because
nothing documents the relationship between an MPI window and the number of
`ucp_ep_rkey_unpack` calls. I believe OpenMPI unpacks two per window per peer
(data + state) and does it lazily on first access, but that belief comes from
knowing OpenMPI, not from anything in the capture. A reader with a capture in
front of them cannot tell "MPI unpacked 10 keys for 5 windows" from "comshu
recorded 10 of the 15 unpacks that happened".

## 10. Smaller things, each of which cost a few minutes

- **`comshu read` row counts vs `expect_tables`.** `expect_tables` fails a case
  when a table is empty, but a table can also be *absent* because the provider
  was not selected. From the outside these look the same in a summary.
- **`-- requires:` is documented for `params.<col>` and tables, but not for
  derived tables that exist-but-are-empty** — `transport_mix` with zero rows is
  a different situation from no `transport_mix`, and only one of them is a skip.
- **The first-token comment wart** (`comshu sql` and `POST /query` reject a
  statement whose first token is a comment) is documented in the apps README,
  which is where a reader will find it only after hitting it. Every `.sql` in
  this study puts its prose after the `-- requires:` line and before the
  `SELECT`, which works, but the rule is a property of the reader rather than of
  SQL and there is nothing in `comshu sql`'s own usage text that says so.
- **No documented way to ask a capture "which providers produced you"** other
  than inferring it from which streams exist.
