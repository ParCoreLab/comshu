# What the LAMMPS and LLM-scale-out round-2 studies found in the tool

**Status:** todo. Fourteen findings, of which two are fixed and twelve are not.
Finding 14 was produced by verifying finding 2 — the act of checking a fix
surfaced a defect in how fixes reach the machine at all.

**The runs.** Everything here comes from three MN5 jobs and nothing here is
speculation:

| job | study | what it was |
|---|---|---|
| 45329318 | `tests/hpc/apps/02_lammps` | 16 ranks / 4 nodes, `--map-by ppr:4:node` (block) |
| 45330017 | `tests/hpc/apps/02_lammps` | the same decomposition, `--map-by node` (cyclic) |
| 45330280 | `tests/hpc/apps/06_llm_scaleout` | llm.c, 16 GPUs / 4 nodes, `-p nccl,ib` |

Both studies were **black-box**: their author could not read `providers/`,
`pcomm-read/` or `tools/`. That is why several entries below are phrased as
"the capture cannot say" rather than "the code is wrong" — and why finding 2
turned out to be the tool behaving correctly and failing to say so.

---

## Fixed already

**1. `pcomm types` omitted type 528 while `read` and `export` showed it.**
Three surfaces disagreed about whether `gpu_device` exists; the one whose entire
job is "what can this build decode" was the one that lied. Fixed in `2bd22c8`.
Root cause is the fifth hand-maintained list in this repo sitting beside a
generated one — see that commit for the full list of five.

**2. `ucx_deep` reported `accepted = 0` on UCX 1.20.0.** NOT a defect: GUARD 1
refuses to patch `uct_iface_ops_t` on any version whose layout has not been
verified, because a trampoline in the wrong slot takes down the *application*.
The verified range was 1.14–1.19 and LAMMPS ran 1.20.0. Resolved by measuring
`src/uct/api/tl.h` across the tags — md5 `bef986ac` is identical for v1.14.0,
v1.18.0, v1.19.0, v1.19.1, **v1.20.0** and `origin/master` — and widening the
range with that evidence.

**Confirmed on hardware, job 45332111.** `accepted` went 0 -> 1 with
`runtime_version = 70656` (0x11400 = 1.20.0) on all 16 ranks, `transport_mix`
went from empty to 148 rows, and the study's dead prediction is answered: the
scope derived from `tl_name` and the scope derived from node membership agree.
The `comm_edges` bytes mirror across the two placements to within 0.003%
(250,401,000 / 318,713,225 block, 318,706,048 / 250,408,177 cyclic), for network
shares of 56.00% and 44.00% against a pre-committed 55.68% / 44.32%.

The *reporting* half of this one is still open; see finding 3.

---

## Open — reporting that misleads

**3. A refusal is invisible outside stderr.** `ucx_deep`'s guard prints a precise
message and sets `uct_binding.accepted = 0`, but no document mentions
`uct_binding`, and `link_health` does not look at it. The visible symptom is
that `transport_mix` — the table the black-box brief presents as *the* answer to
"did this leave the node" — is silently empty. A `link_health` row for
`accepted = 0` would turn a silent hole into a finding. Cheap, and it is the
reason a correct refusal read as a bug for two days.

**4. `link_health` files unavoidable findings on any NCCL ibverbs capture.**
Job 45330280: `wr_flight_unretired = 69,470`, 65.6% of posts, plus two siblings
— on a capture whose other 15 findings are zero. The third of those siblings
records that the QPs declared `sq_sig_all = 0`, i.e. the rubric proves the
traffic is unpairable *by design* and then counts it as a defect. Either the
selective-signalling case should suppress the other two, or the rubric needs a
severity distinct from "finding".

**5. CSV decodes enums, Parquet does not, and `link` is Parquet-only.** Four
record types affected, noticed independently by three studies. A reader who
wants the derived layer must use the export that shows `3` where the other
shows `nbi`.

---

## Open — vocabularies that exist only in the source

**6. `comm_edges.unit`** is `messages` on the verbs lane and `datatype-units` on
the UCX lane, both alongside a column called `bytes`, and that vocabulary is
published nowhere. On a non-power-of-two datatype a reader is off by the type
size and nothing says so.

**7. `link_health.finding` and `tag_flow.status` values are unpublished.**
LAMMPS produced `no_tagged_wire_op`, which appears in no document the studies
were allowed to read.

**8. `rank_identity.node_ix` is not rank order** — ranks 0–3 resolved to `n1` —
and `node_hash` is unsigned 64-bit, which a naive reader will render signed.
Neither is wrong; neither is stated.

---

## Open — what the capture structurally cannot say

**9. No record carries NCCL's algorithm or protocol.** The study recovered "it
was a ring" by matching an IB payload total to `4·2·(15/16)·S` at 99.36%. That
is inference standing in for measurement, and it is the single most useful fact
about a collective run.

**10. Nothing records what the tracer was asked to do.** Jobs 45329318 and
45330017 differ *only* in `--map-by`, produce measurably different traffic, and
nothing inside either capture says which is which. An `Invocation` artifact —
argv, provider list, the `PCOMM_*` environment — would make a capture
self-describing. This is the cheapest high-value item in this file.

**14. Nothing records which BUILD produced a capture, and it cost a run.**
Job 45331793 was submitted to verify the range widening and reported
`accepted = 0` on 1.20.0 — apparently refuting the change. It did not. That job
loads `build-apps06/providers-sysucx/`, a tree of `.so` files hand-copied out of
`build-ucx120`; the source change had synced and `build/` had rebuilt, but the
copied artefact was two hours stale. There are now SIX UCX build directories on
MN5 (`build-ucx`, `build-ucx06`, `build-ucx06sys`, `build-ucx120`, `build-ucx2`,
`build-ucxtest`) and one job pointing at copies from one of them.

`uct_binding` already carries `compiled_version` and `runtime_version`, so a
capture can say which UCX HEADERS it was built against — but not which pcomm
commit, which is the axis that actually bit. This is the same class of gap as
finding 10: provenance of the binary rather than of the invocation, and the two
want fixing together. I nearly reported the stale result as evidence against my
own change.

**11. `transport_mix` covers UCX only**, while the brief presents its `scope` as
the general answer to "did this leave the node". On the NCCL capture it saw
579,914 B against 1.81 GB of IB — 0.032%, all of it the MPI bootstrap.
`comm_edges.scope` (added 2026-09-01) is the general answer; the brief should
say which table answers which question.

**12. `comm_edges` aggregates message sizes away**, so a halo population cannot
be separated from a collective one inside the derived layer. This is what made
LAMMPS's 2:2:1:1 message-split prediction unfixable without leaving the derived
layer entirely.

---

## Open — ergonomics

**13. `pcomm sql` has three accumulated warts**: it rejects a leading comment,
rejects a trailing semicolon, and `params` as a leading CTE breaks any assertion
carrying its own `WITH`.

The consequence is measurable and nobody was counting it: **four of four
application studies wrote their own private assertion runner** rather than use
`tests/hpc/suite/sql_check.sh`. That is the harness being rejected by every
user it has had. Fixing the three warts is small; deciding whether `sql_check.sh`
should absorb the four runners is the real question.

---

## Not a finding, recorded so it is not re-derived

LAMMPS's `per_peer_message_split_is_two_two_one_one` is marked xfail with
evidence: the oracle was incomplete, not the capture. Open MPI's
recursive-doubling collective partners are `{r^1, r^2, r^4, r^8}`, which overlap
the halo peers on three of four, so rank 0→8 carries 160 sends of 10,268 B that
are not halo traffic. Bytes are untouched (0.03%), which is why the byte-share
assertion passes at ±0.2 points while the message-count one cannot.
