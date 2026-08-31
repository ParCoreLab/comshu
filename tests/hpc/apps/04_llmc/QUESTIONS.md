# 04_llmc — what the capture and its documentation could not tell me

Round 2, black-box: `providers/`, `libpcomm/` (bar `schema/`), `pcomm-read/`,
`tools/`, `plans/` and `BUGS-FOUND.md` were not read. Everything below is a
question I hit while running llm.c under `-p nccl,kernel` on du04 and could not
answer from `pcomm --help`, `pcomm list`, `pcomm.fbs`, the schema README, the
derived-table contracts in `BRIEF-BLACKBOX.md`, or the assertions in
`tests/hpc/cases/*/asserts/`. Ordered by how much they cost me.

Nine of the fifteen have a concrete measurement attached, so none of them is a
matter of taste.

## 1. Nothing in the capture says which algorithm or protocol carried a collective

The biggest one, and the reason `REPORT-r2.md` §5's knob claim came out the way
it did. NCCL's own tuner reports **three protocols inside a single fused group**
(`LL` at 768 B, `LL128` at 1.18 MB, `SIMPLE` at 38.6 MB) and that group is
exactly one kernel launch. So `gpu_kernel.name` — the only field in the capture
that mentions a protocol at all — names one compiled specialisation and is wrong
about the rest, and `NCCL_ALGO=Ring NCCL_PROTO=Simple` changes the device time
2.9x while changing **no** field in the capture. A user cannot tell from a
capture whether a slowdown was a protocol change, and the docs do not warn that
the kernel name is not the protocol. Is there a door for NCCL's per-collective
tuning decision, and if not, should `gpu_kernel.name` carry a caveat?

## 2. What do `rank_identity.devices` and `.label` mean?

The contract is a column list — `rank,node_ix,node_hash,device,devices,label` —
and nothing states what values are legal. I measured `devices = 0` and
`label = 'n0 · GPU ?'` on every rank of every capture and could not tell whether
`devices` is "how many devices this rank used", "how many the node has", or a
bitmask; nor whether `label` is a stable identifier or a display string. Because
I could not tell, I predicted nothing about either, and my assertion checks only
`device`. `device` itself is null (REPORT-r2.md §3) and I had to guess at why.

## 3. `link_health` is documented as empty and is a fixed 18-row rubric

`BRIEF-BLACKBOX.md`: *"`link_health` is expected to be empty. Every row in it is
a defect in the link — if it has rows, that is a finding."* Measured: 18 rows on
every capture, `n = 0` on all of them, one per named finding, each with a note.
A user following the brief files 18 defects against a clean capture. Case 21's
assertion tests `n > 0`, so only the brief is wrong — but the brief is what an
outside user gets. Which of the 18 findings are even *reachable* for a capture
with no ibverbs and no NVBit is also undocumented; I could only observe that all
18 are always written.

## 4. Empty tables exist in a CSV export, do not exist in a Parquet export, and
## the derived tables exist either way

Same capture, three behaviours:

| | empty raw table | derived table with no rows |
|---|---|---|
| `--csv` | present, header only (55 of 58 here) | not written at all |
| `--parquet` | **absent — `Catalog Error`** | **present, zero rows** |

`pcomm link` accepts only Parquet, so an assertion over the derived layer must
survive on the representation where `SELECT count(*) FROM nccl_p2p` is a parse
error rather than a zero. Round 1 recorded "the safety comes from the exporter";
this round is where that safety turns out to be format-specific. Is the Parquet
behaviour intended, and how is an assertion supposed to claim "this table is
empty" on it? (I resorted to a presence test over `duckdb_views()`.)

## 5. An export is a set of DuckDB *views*, which `duckdb_tables()` does not show

`SELECT count(*) FROM duckdb_tables()` is 0 on an export with 20 tables in it;
they are all in `duckdb_views()`. Nothing documents this, and it is the first
thing anyone writing a table-existence check will get wrong.

## 6. `pcomm sql` cannot run any assertion in `tests/hpc/cases/` as written

It wraps the statement in `COPY (<query>) TO '<tmp>.csv'`, so a trailing `;` is a
`Parser Error` — and every `asserts/*.sql` in the suite ends with one. Combined
with the documented "first token must not be a comment" wart, the two documented
artefacts of the project (the assertion files) cannot be fed to the documented
query interface without an editing step. Nothing says so; I found it by having
every assertion fail identically.

## 7. `gpu_kernel.name` is mangled, and the schema says demangled

`pcomm.fbs` type 530: `name:uint32; // interned demangled symbol`. Measured:
`_Z40ncclDevKernel_AllReduce_Sum_bf16_RING_LL24ncclDevKernelArgsStorageILm4096EE`.
Every `LIKE` pattern a user writes depends on which it is.

## 8. `gpu_kernel.stream` is not a `CUstream` and cannot be joined to `nccl_collective.stream`

`pcomm.fbs` says `stream:uint64; // CUstream`. Measured values are 7, 19, 20 —
CUPTI stream *ids*, not pointers — while `nccl_collective.stream` is an intern
handle over the real `CUstream` (values 3 and 77/149 on the same ranks). So
"which kernel executed this collective" is not answerable even with both
providers in one pass, and the two `stream` columns are different quantities with
the same name. I recovered the mapping only structurally, by counting: 3 f32
collectives and 3 f32 kernels on the gradient stream, 4 and 4 on the other.
Is a join intended, and if not, should the columns be named apart?

## 9. Intern handles are per rank, so `count(DISTINCT stream)` across ranks is meaningless

Rank 0 interned `main_stream` as handle 77 and ranks 1-3 as 149 — the same
`CUstream` role, three handles. A global `count(DISTINCT stream)` therefore reads
3 for a workload with two streams per rank, and I nearly wrote that as a
prediction failure. The schema README says handles restart per domain; it does
not say they restart per rank, and nothing in the CSV/Parquet column contract
warns that these columns are only comparable within a rank.

## 10. `gpu_kernel.source` is a number with no published meaning

Measured `source = 0` on all 5004 rows. The schema says the field is a
`KernelSource` and that submission timing "is only ever present as
`source = HostSubmit`", but the enum values live in `core/vocab.hpp`, which is
not readable in this round and is not in the schema. So I could not check
directly that these are execution windows; I had to infer it from
`link_health.kernel_rows_submission_only = 0`. `TimeKind`, `MemOp`, `MemSpace`
and `EdgePredicate` *are* declared in `pcomm.fbs`; `KernelSource` is not.

## 11. `pcomm list` advertises providers this build cannot load

A build with only `nccl` and `cupti` still lists all seven, under the heading
"the providers this build can select". `-p ib` then fails at run time with
`ib provider library not found (libibverbs_trace.so)` and exit 1 — loud, so not
dangerous, but the header is false. The same output also does not say *which*
providers must own the runtime, though the footer says the distinction exists;
I had to discover empirically that `nccl,kernel` co-run in one pass.

## 12. `PCOMM_PROVIDER_PATH` is not documented anywhere I could read

`pcomm run` with no arguments prints one line and no environment. The nccl
provider and the cupti backend build to different subtrees
(`providers/nccl/`, `gpu-backends/cupti/`), and the variable turns out to take a
colon-separated list. I found that by trying it. For an out-of-tree build this is
the first thing a user needs.

## 13. `timeline_span` is a derived table with no contract

It is not in `BRIEF-BLACKBOX.md`'s list, and `pcomm link` reports "10 derived"
without naming them. Its columns are `rank, stream, ep, bytes, kind, t0_ns,
t1_ns`; `kind = 2` on every row here, `bytes` is null, `stream` holds the string
`gpu_kernel`, and there is no `name`, so the one derived table that carried
anything for this workload cannot be filtered down to NCCL kernels. I did all
timing arithmetic on raw `gpu_kernel` ticks instead, converting with
`clock_anchor` (0.4444386 ns/tick, consistent to 8 digits across ranks), which
the docs also do not describe.

## 14. `PARAM_COLS` is a closed list, so a study cannot add an oracle

Round 1 recorded this; round 2 paid for it. The two quantities this round's
assertions needed — the NCCL device-kernel count and the ZeRO-1 groups per step
(23 = `groups_per_iter` + `NUM_PARAMETER_TENSORS`) — have no `PARAM_COLS` column,
so both had to be re-derived inside the SQL from other capture columns. That is
weaker than a parameter: an oracle that is computed from the capture is one join
away from being the capture checking itself.

## 15. Nothing in a capture records what the tracer was asked to do

`pcomm read` lists streams and counts; there is no record of the `-p` selection,
the command line, the provider versions, or the environment. Two of my four
captures differ *only* in an environment variable (`NCCL_PROTO`), and nothing
inside them says so — I know which is which because of the directory name I
chose. For a study whose whole method is "diff two captures", that is the one
piece of provenance the format does not carry.

---

## Things I expected to be problems and were not

- `-p nccl,kernel` composes in **one pass**, with no replay: `pcomm run` says
  `2 providers -> 1 pass` and llm.c's banner appears once. CUPTI costs ~2 % of
  step time and reports `dropped=0`.
- `pcomm link` is idempotent-ish and fast, and refuses a CSV export with a clear
  message naming the fix.
- The `-p nccl` control capture is row-for-row identical to the
  `-p nccl,kernel` one on every NCCL table, so the kernel provider does not
  perturb what the collective provider sees.
