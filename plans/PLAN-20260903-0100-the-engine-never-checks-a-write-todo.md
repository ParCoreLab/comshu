# The engine never checks that a write succeeded

Found while verifying the NCCL profiler door, and **unrelated to it**. Filed
separately because it is a defect in the stream writer, which is the foundation
everything else rests on.

## The one-line version

`libpcomm/include/pcomm/rec/sink.hpp` ignores the return value of every
`fwrite`:

```cpp
67:  std::fwrite(view.data, 1, view.bytes, f);   // records
98:  std::fwrite(&h, sizeof h, 1, f);            // the STREAM HEADER
```

So when the filesystem refuses a write — full, over quota, or any other errno —
the engine carries on, the process exits 0, and the capture is silently missing
whole streams. Nothing at capture time says a byte was lost.

## How it was found

MN5 job 45351029: five cases went red that had been green one commit earlier
(`entity_close_accounted`, `star_over_the_nic`,
`small_rma_reached_the_small_doors`) — none of them NCCL. The workload was
byte-identical between the two runs: same 4677/12348/12348/12348 sends, same
queue pairs, same ranks. Rank 3 had lost `ib_mem_region` and `entity_close`
entirely.

The files existed. Their contents did not:

```
rank-0/100.pcs = 17888 bytes      rank-3/100.pcs =  288 bytes   (no stream header)
rank-0/522.pcs = 33968 bytes      rank-3/522.pcs =    0 bytes
                                  rank-3/510.pcs =    0 bytes
```

288 bytes of payload with no 32-byte header in front of it, while `110.pcs` on
the same rank held all 12,348 sends. Not a truncated tail — a missing HEAD on a
file that then received data, which is precisely what an ignored short write on
line 98 followed by a partial write on line 67 produces.

Re-running the five cases at the same commit reproduced it and made it WORSE:
job 45351476 had **43 headerless streams**, with `ib_mem_region` and
`entity_close` empty on every rank. Corruption escalating run over run is a
storage signal, not a code one, and the quota confirmed it:

```
gpfs_home  USR   83.99 GB used   80.00 GB quota   84.00 GB hard limit   grace 6.7 days
~/pcomm-suite                                     63 GB
```

Over quota and about 10 MB from the hard limit. Each run had slightly less room
than the last, which is exactly the observed progression: green, then one rank
losing two streams, then 43.

## It is not the NCCL work, and that was checked rather than assumed

The commit correlation was exact and misleading: green at `9a2b924`, red at
`55d1552`, two of this session's NCCL commits. Three independent checks ruled it
out before the quota was ever looked at.

1. **The diff.** `git diff --name-only 9a2b924..55d1552` is four files:
   `pcomm-read/link_sql.hpp`, `providers/nccl/nccl_profiler_plugin.cpp`, a
   fixture and a plan. Nothing in `libpcomm/`, nothing in `providers/ibverbs/`.
2. **The process.** `Invocation`'s `images` column lists the mapped libpcomm
   objects, and on every rank of case 16 it is exactly
   `libibverbs_trace.so | libpcomm_engine.so`. `libnccl_trace.so` was never in
   the process, so no line of the profiler plugin executed.
3. **The layer.** `link_sql.hpp` is read-side and cannot change what a process
   writes.

The `Invocation` record answering "what actually ran" made check 2 possible, one
day after it landed.

## Fixed (2026-09-03)

1. **Every write is checked.** `fwrite` (records), `fwrite` (header), `fflush`
   and `fclose` all have their results inspected, and the sink latches per-stream
   loss counts that `losses()` reports. `fflush` matters as much as the others:
   with buffered stdio a quota failure usually surfaces there, not at the write.

   Two latent bugs fell out of the rewrite. A failed `fopen` used to cache a
   **null `FILE*`** in the stream map, so every later record of that type was a
   silent no-op — and the destructor then called `fclose(nullptr)`, undefined
   behaviour on a path that only runs once something has already gone wrong. And
   a header write that fails now **closes the file** instead of writing records
   into an unframed one: a zero-length file is honestly empty, a headerless file
   with records in it is a trap.

2. **The run says so.** `Tracer::finalize` prints `CAPTURE INCOMPLETE`, the
   directory, and per-stream record and byte counts. `PCOMM_STRICT_WRITES=1`
   turns it fatal (exit 70) — off by default, because a capture that lost one
   stream is still worth analysing and killing a 4-node job at teardown would
   destroy the rest of the evidence.

3. **The suite refuses to grade it.** `all_in_one.sbatch` fails any case whose
   log contains `CAPTURE INCOMPLETE`, with exit 75 (EX_TEMPFAIL — the run, not
   the code). Caught from the log rather than through `PCOMM_STRICT_WRITES`,
   because `_Exit(70)` under mpirun aborts the job and takes the explanatory log
   line with it.

   This is the part that matters most. In job 45351029 every assertion in those
   cases was reading a capture that had lost whole streams, so the three reds
   AND the greens beside them were equally meaningless. A board must not grade a
   capture it cannot trust.

4. **Tested where it can be.** `tests/sink_write_errors.cpp` cannot fill a
   filesystem, so it takes the write paths away instead: an unwritable
   directory, an uncreatable one, and the happy path that proves the loss
   counters stay quiet when nothing is wrong. A sink that cried loss on healthy
   runs would be switched off within a week. Mutation-checked — removing the
   loss accounting for a stream that never opened fails two assertions.

## Still open

**`link_health` does not carry this yet.** A reader who only ever sees the
derived tables still cannot tell a capture that lost a stream from one whose
workload was quiet. The reader has the evidence — a file with no header is
detectable — but plumbing it from `pcomm export` into a rubric row is a separate
change. Until then the signal lives on stderr and in the suite board.

## Operational, done and not done

`~/pcomm-suite` held 63 GB of accumulated per-job results on a home directory
over an 80 GB quota. Deleted on the user's instruction, which returns home to
roughly 21 GB of 80 and unblocks the suite.

**The durable fix is still open.** A suite that writes its results into a
quota'd home directory will hit this again — it is 1,512 entries after a few
weeks. `PCOMM_OUT_ROOT` should point at `gpfs_scratch`, which had 592 GB used of
a 1 TB group quota, and the suite should prune runs older than the last N. Not
done here because it changes where every board on this machine lands, which is
worth deciding deliberately rather than as a footnote to a bug fix.

## What it is worth

The suite's assertions caught the consequence on three independent cases, which
is the instruments working. But nothing named the cause: five red cases pointed
at IB and UCX behaviour while the actual fault was a filesystem quota and an
unchecked `fwrite`. Closing that diagnostic distance is the point of items 1-3.
