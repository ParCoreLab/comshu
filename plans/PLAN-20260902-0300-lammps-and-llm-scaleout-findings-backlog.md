# What the LAMMPS and LLM-scale-out round-2 studies found in the tool

**Status: 13 of 14 closed.** Finding 9 is open and needs a new provider door plus
a hardware run; everything else is fixed, tested and on `main`.

Finding 14 was produced by verifying finding 2 — the act of checking a fix
surfaced a defect in how fixes reach the machine at all. Finding 13 turned out to
be one and a half findings rather than three, which measuring is what
established.

**The runs.** Everything here comes from three MN5 jobs and nothing here is
speculation:

| job | study | what it was |
|---|---|---|
| 45329318 | `tests/hpc/apps/02_lammps` | 16 ranks / 4 nodes, `--map-by ppr:4:node` (block) |
| 45330017 | `tests/hpc/apps/02_lammps` | the same decomposition, `--map-by node` (cyclic) |
| 45330280 | `tests/hpc/apps/06_llm_scaleout` | llm.c, 16 GPUs / 4 nodes, `-p nccl,ib` |

Both studies were **black-box**: their author could not read `providers/`,
`pcomm-read/` or `tools/`. That is why several entries below were phrased as
"the capture cannot say" rather than "the code is wrong" — and why finding 2
turned out to be the tool behaving correctly and failing to say so.

---

## Closed

**1. `pcomm types` omitted type 528 while `read` and `export` showed it.**
Three surfaces disagreed about whether `gpu_device` exists; the one whose entire
job is "what can this build decode" was the one that lied. Fixed in `2bd22c8`.

**2. `ucx_deep` reported `accepted = 0` on UCX 1.20.0.** NOT a defect: GUARD 1
refuses to patch `uct_iface_ops_t` on any version whose layout has not been
verified. Resolved by measuring `src/uct/api/tl.h` across the tags — md5
`bef986ac` identical for v1.14.0 through v1.20.0 and `origin/master` — and
widening the range with that evidence. Confirmed on job 45332111: `accepted`
0 -> 1, `transport_mix` empty -> 148 rows, and the study's dead prediction
answered (scope from `tl_name` and scope from node membership agree; bytes mirror
across placements to 0.003%, giving 56.00% / 44.00% against a pre-committed
55.68% / 44.32%). Fixed in `169d685`.

**3. A refusal was invisible outside stderr.** `link_health` gained
`uct_ops_patch_refused` (defect, counted per RANK so a heterogeneous allocation
reads "3 of 16") and `uct_ops_patch_forced` (note). The visible symptom used to
be an empty `transport_mix`, indistinguishable from a workload that sent nothing;
that cost this study two days. `5b99572`.

**4. `link_health` filed unavoidable findings on any NCCL ibverbs capture.**
The rubric proved the traffic unpairable by design — the queue pairs declared
`sq_sig_all = 0` — and then counted the consequence as a defect: 69,470
unretired posts, 65.6% of every post, beside fifteen zero rows.

Fixed by giving the table a `severity` column of `defect` or `note`. A defect is
something the LINK could not do; a note is something the CAPTURE can account for.
Clean is now `WHERE n > 0 AND severity = 'defect'`. The unretired count splits on
the queue pair's own declaration (NULL stays on the defect side, so an old
capture is not absolved by its age), and `wr_flight_unsignalled_qps` became
`wr_flight_unexplained_qps` because the old name was precisely backwards once the
declared cases moved out. `5b99572`.

**5. CSV decoded enums, Parquet did not, and `link` is Parquet-only.** The
`vocabulary` derived table publishes every numeric vocabulary as
`(vocab, value, name)`, joinable from any query. `schema_check.py` holds it equal
to the enums in `pcomm.fbs` and `vocab.hpp` in both directions — a renamed value
fails the build, and so does an enum a record field uses that the table does not
publish. `fff4489`.

**6. `comm_edges.unit`** — the three values and the datatype-count caveat are
published in `agent/catalog.md` and `BRIEF-BLACKBOX.md`. `fff4489`.

**7. `link_health.finding` and `tag_flow.status` were unpublished.** Both closed
lists are now in the catalog and the brief, along with
`tag_recv_resolved.status`. `transport_mix`, `tag_flow` and `tag_recv_resolved`
were entirely absent from `agent/catalog.md`, which documented seven derived
tables while the layer had twelve — that was the root of findings 7 and 11
rather than a symptom. `5b99572`, `fff4489`.

**8. `rank_identity.node_ix` is not rank order** and `node_hash` is unsigned
64-bit. Both stated in the catalog and the brief. `fff4489`.

**10 + 14. Nothing recorded what the tracer was asked to do, nor which binary
answered.** Closed together by the `Invocation` record (type 532), one row per
rank, carrying argv, an allowlisted environment, the providers that declared
themselves, the build identity, and **every mapped `libpcomm` object with its
size and mtime on disk**.

The last of those is the answer to finding 14 and was demonstrated rather than
argued — the same probe run against the build tree and against a backdated
hand-made copy:

```
fresh  .../build-duck/providers/ibverbs/libibverbs_trace.so 35004896 1788340391
stale  .../providers-sysucx/libibverbs_trace.so             35004896 1788260520
```

`build` is stamped by a BUILD-STEP target rather than `configure_file`, because a
commit stamped at configure time keeps reporting whichever commit someone last
ran cmake on — the same stale-provenance failure one layer up. `$PCOMM_COMMIT`
overrides the git lookup for rsync'd trees with no `.git`, which is what du04 is.
Read through `run_provenance`. `1d7984e`.

**11. `transport_mix` covers UCX only.** The catalog now says which table answers
which question: `comm_edges.scope` is the general "did this leave the node",
`transport_mix` answers "which transport" and is the one to check when it comes
back empty. `fff4489`.

**12. `comm_edges` aggregated message sizes away.** `edge_sizes` is comm_edges
one group-by deeper, with `size_class` as the lower bound of a power-of-two
bucket. comm_edges was deliberately NOT rewritten to feed it — it is held
byte-identical to a second C++ implementation — so the price is one rule written
twice, and `tests/comm_edges_rule.sh` pays it: rolled up over `size_class`,
edge_sizes must reproduce comm_edges exactly, asserted in both directions and
mutation-tested. `cfcd48d`.

**13. `pcomm sql` had three accumulated warts — and one and a half of them were
real.** Measured: a leading comment works and always did; a query carrying its
own `WITH` works (the `params` splice is `sql_check.sh`'s job); a trailing
semicolon was genuinely broken. Measuring also turned up a fourth nobody had
reported and worse than the semicolon: a trailing `-- comment` ate the wrapper's
own closing paren, so `SELECT 1 -- why` failed with "syntax error at end of
input". That is how every `.sql` file under `tests/hpc/*/asserts` is annotated.

Fixed by binding the query as a TEMP VIEW body instead of parenthesising it, so
DuckDB does the lexing — textual normalisation was tried and abandoned, because
going far enough to handle `SELECT 1; -- why` means deciding whether a `--` is a
comment or two characters inside a string literal. `tests/sql_cli_rule.sh` pins
eleven query shapes. `09d3cfc`.

---

## Open

**9. No record carries NCCL's algorithm or protocol.** The study recovered "it
was a ring" by matching an IB payload total to `4·2·(15/16)·S` at 99.36%. That is
inference standing in for measurement, and it is the single most useful fact
about a collective run.

**Why it is still open, and what it would take.** The nccl provider interposes
the PUBLIC HOST API (`ncclAllReduce` and friends, via the weak/strong PROFAPI
construction — see `providers/nccl/nccl_hooks.hpp`). Algorithm and protocol are
chosen by NCCL *after* that call returns, per collective, from the message size
and the topology. They are structurally invisible at this door, so this is not a
missing field on an existing record — it is a new door.

The candidate is NCCL's **profiler plugin API** (`libnccl-profiler.so`), whose
event descriptors do carry algorithm and protocol at the kernel level. MN5 has
2.26.5 and 2.29.3, both new enough. That means a new provider, a new record, and
a multi-node NCCL job to verify against — a session of its own, not a tail-end
change.

**Partially mitigated in the meantime.** `NCCL_ALGO` and `NCCL_PROTO` are in the
`Invocation` env allowlist, so a capture that PINS them now says so. That records
what was *requested*, never what NCCL *chose* when they are unset, and the
distinction must not be blurred in any report.

---

## Also fixed while working these

**`ctest -R figures_current` was RED on `main`** and had been since GpuDevice
landed. `tools/check_figures.py` did exactly what it was written to do — "adding
a PCOMM_RECORD leaves the figure silently missing it" — and then nobody ran it,
because the previous session's verification loop was the HPC suite while that
check lives in the local ctest set. Three record counts in `gen_figures.py` said
53 while the tree held 60. `50d95ce`.

**A JOIN I REPORTED AS SILENTLY WRONG, AND WAS NOT.** While building
`edge_sizes` I observed one queue pair's two size classes resolving to different
peers — the zero-length class falling to `dst_rank = -1` while the 64-byte class
on the same `(rank, qp_num)` resolved correctly — and wrote it up as a probable
DuckDB planner issue, in a code comment and in this file.

**It was a stale binary holding my own mutation.** A mutation test had appended
`AND s.size_class > 0` to that exact LEFT JOIN, which produces precisely that
symptom and nothing else. The source was restored; two rebuild-skipping test
cycles ran afterwards, so the binary kept answering for the mutated tree. From a
clean build both spellings agree, and re-applying the mutation reproduces the
"anomaly" byte for byte. There is no planner bug and `comm_edges` was never at
risk.

Recorded because it is finding 14's failure mode arriving one commit after
finding 14 was closed, in the work that closed it. Two process changes came out
of it: the local runner no longer has a skip-the-build flag, and `Invocation`
now records the mtime of every mapped object, which is what would have caught
this from the capture side.

---

## Not a finding, recorded so it is not re-derived

LAMMPS's `per_peer_message_split_is_two_two_one_one` is marked xfail with
evidence: the oracle was incomplete, not the capture. Open MPI's
recursive-doubling collective partners are `{r^1, r^2, r^4, r^8}`, which overlap
the halo peers on three of four, so rank 0→8 carries 160 sends of 10,268 B that
are not halo traffic. Bytes are untouched (0.03%), which is why the byte-share
assertion passes at ±0.2 points while the message-count one cannot.

**`edge_sizes` is what would let that oracle be finished** — the message-count
split it needed is now one query — but the oracle itself still has to account for
the collective partners, so the xfail stands until someone writes that.

---

## The scope question finding 13 raised, still unanswered

**Four of four application studies wrote their own private assertion runner**
rather than use `tests/hpc/suite/sql_check.sh`. The three warts are fixed and the
closed `workload_params` list that rejected GROMACS's and Quantum ESPRESSO's
oracles was opened earlier. Whether `sql_check.sh` should now absorb the four
runners is a decision about scope, not a defect, and it is not mine to take
unilaterally.
