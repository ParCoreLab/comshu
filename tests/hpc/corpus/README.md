# tests/hpc/corpus — the capture corpus

Step 0 of `plans/PLAN-20260807-1718-ui-backbone-todo.md`. The UI's derived
tables, its partitioning, and every "is this fast enough" question are design
decisions that need **real captures at real scale** to answer. Designing them
against `tests/data/ib_pingpong-44368973` — 72 KB, two ranks — is how the
`qp_num` collision class of bug gets rediscovered at demo time.

## The corpus lives outside the repo, and outside `~/pockt`

Default `~/waste/pcomm/corpus`, override with `PCOMM_CORPUS`. **Only `index.tsv`
and `types.tsv` are committed.**

Two separate reasons, and the second is the one that picks the directory:

- Captures are hundreds of megabytes each, so they do not belong in git.
- `~/pockt` is mirrored between machines by syncthing. Anything bulky and
  regenerable placed under it gets replicated to every host for no benefit —
  the corpus is ~450 MB today and grows with every scale run. `~/waste/` is the
  local-only scratch root that syncthing does not carry, so that is where it
  goes.

The index is the part that has to be reviewable and diffable, and it is the part
every design decision actually reads. It is small, so it is committed.

## A capture directory carries no provenance at all

This is the fact that shapes the tooling. A capture is:

```
<case>-<jobid>/rank-N/<type_id>.pcs
              rank-N/intern-blob.bin
```

and nothing else. No meta.json, no job record, no note of which commit built the
tracer. Everything we know about a capture comes from the *name of the directory
it was written into* and from the suite case that produced it — both of which
are lost the moment it is copied somewhere.

So `harvest.sh` writes a `provenance.tsv` beside each fetched capture, at fetch
time, because it cannot be reconstructed afterwards. For captures harvested
retroactively out of `~/pcomm-suite` the tracer commit is genuinely unknown and
is recorded as `unknown`, rather than guessed from the job id's date.

## Schema drift is a warning, not a wall

Captures in `~/pcomm-suite` were written by schema `14b1d68c503ae656`; HEAD is
`160d1b71ce3bed36`. Verified 2026-08-09: every stream still decodes, because
`stream_reader.hpp` refuses only on a *stride* disagreement and warns on a hash
one. `nccl_internode-44400747` exports its full CSV set under HEAD's reader with
nothing but per-file notes.

That is a property of these captures, not a guarantee. If a record's layout
moves, its stream is refused per-stream while the rest of the capture still
reads — so a stale corpus degrades in pieces rather than all at once, and
`index.sh` records the schema hash per capture so the degradation is visible
rather than inferred.

## Usage

```bash
tests/hpc/corpus/harvest.sh                     # newest job's 8 suite cases
tests/hpc/corpus/harvest.sh --job 44400747      # a specific job
tests/hpc/corpus/harvest.sh --cases "nccl_internode nvshmem_multinode"
tests/hpc/corpus/index.sh                       # rebuild index.tsv from the corpus
```

`index.sh` needs a built `pcomm`; it finds one in `build-corpus/`, `build/`,
or `$PCOMM_READ`.

## What the corpus still lacks

Tracked here rather than in the plan, because it is a status, not a design:

- **Scale above 32 ranks.** `scale_nccl.sbatch` covers 32 (8 nodes × 4), which
  is `acc_debug`'s ceiling — the QoS is capped at `node=8`. 64 ranks needs
  `acc_ehpc`, a different queue with a different wait. Nothing has measured
  whether `comm_edges` stays exact above 32.
- **Volume.** The largest capture is ~168 MB / 2.1M records. Nothing here is
  uncomfortable enough to size the Parquet or query layers against — which is
  itself the finding: the design is not volume-constrained at the scales we can
  currently produce.
- **IBGDA/DEVX.** Cases 09-12 run on du04, not MN5, so no DEVX-only capture is
  in this corpus. This is the one that matters most for the comm matrix: under
  IBGDA the `ib_*` streams are empty and `comm_edges` is blank.
