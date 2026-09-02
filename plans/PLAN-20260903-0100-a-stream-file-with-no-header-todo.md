# A stream file with no header — a capture can silently lose whole streams

Found while verifying the NCCL profiler door, and **unrelated to it**. Filed
separately because it is a defect in the engine's stream writer, which is the
foundation everything else rests on.

## What happened

MN5 job 45351029, case 16 (`hubspoke_ib`): five cases went red that had been
green in job 45350841 one commit earlier. The failing assertions were
`entity_close_accounted`, `star_over_the_nic` and, on cases 21/22,
`small_rma_reached_the_small_doors` — none of them NCCL.

The workload was **byte-identical** between the two runs:

| stream | job 45350841 (green) | job 45351029 (red) |
|---|---|---|
| `ib_post_send` | r0=4677 r1=12348 r2=12348 r3=12348 | r0=4677 r1=12348 r2=12348 r3=12348 |
| `ib_mem_region` | r0=187 r1=187 r2=187 **r3=186** | r0=186 r1=187 r2=187 **r3=0** |
| `entity_close` | r0=607 r1=607 r2=607 **r3=606** | r0=606 r1=607 r2=607 **r3=0** |

Same traffic, same queue pairs, same rank count. Rank 3 lost two entire streams.

## The cause is on disk

The stream FILES exist on rank 3 — the directory listing is identical to rank
0's. Their contents are not:

```
rank-0/100.pcs = 17888 bytes      rank-3/100.pcs =  288 bytes
rank-0/522.pcs = 33968 bytes      rank-3/522.pcs =    0 bytes
                                  rank-3/510.pcs =    0 bytes
```

and `pcomm read` says exactly what is wrong with them:

```
rank-3/100.pcs   288 bytes  (no stream header — pre-format-1 capture, re-capture it)
rank-3/510.pcs     0 bytes  (no stream header — pre-format-1 capture, re-capture it)
```

**288 bytes of payload with no 32-byte header in front of it.** The file was
created and written to, and its header never landed. Two sibling files are
zero-length. Every other stream on that rank is intact, including
`110.pcs` with all 12,348 sends.

So this is not a truncated tail — it is a missing HEAD on a file that then
received data. Any hypothesis has to explain writing payload past a header that
was never written.

## It is not the NCCL work, and that was checked rather than assumed

The commit correlation is exact and misleading: green at `9a2b924`, red at
`55d1552`, which are two of this session's NCCL commits. Three independent
checks rule it out.

1. **The diff.** `git diff --name-only 9a2b924..55d1552` is four files:
   `pcomm-read/link_sql.hpp`, `providers/nccl/nccl_profiler_plugin.cpp`, a
   fixture and a plan. Nothing in `libpcomm/` — the engine that writes streams —
   and nothing in `providers/ibverbs/`, the provider case 16 runs.
2. **The process.** `Invocation`'s `images` column lists the mapped libpcomm
   objects, and on every rank of case 16 it is exactly
   `libibverbs_trace.so | libpcomm_engine.so`. `libnccl_trace.so` is not in the
   process, so no line of the profiler plugin executed.
3. **The layer.** `link_sql.hpp` is read-side. It cannot change which records a
   process writes.

The `Invocation` record answering "what actually ran" is what made check 2
possible, one day after it was added.

## What to do

1. **Reproduce deliberately.** A re-run of the five cases at the same commit is
   in flight (job 45351476). If they pass, the defect is intermittent and needs
   a stress reproduction rather than a suite run.
2. **Find the write path that can emit payload before a header.** Start at the
   per-type stream open in the engine: something opens the file, and the header
   write is either buffered and lost or skipped on a path that the data write
   does not take. Rank 3 is the busiest rank in `hubspoke_ib`, which points at
   a race under load or at an error path taken when a write is short.
3. **Make it loud at capture time, not read time.** Today the only signal is
   `pcomm read` reporting "no stream header" — a line a reader has to look at.
   A capture that lost a stream should say so in `link_health`, which is where
   this class of fact belongs and where the suite already looks.

## What it is worth

The suite's assertions caught the consequence on three independent cases, which
is the instruments working. But nothing named the CAUSE: five red cases pointed
at IB and UCX behaviour, and the actual fault was a zero-byte file. The gap
between "several assertions failed" and "one rank's streams have no header" is
the diagnostic distance this finding exists to close.
