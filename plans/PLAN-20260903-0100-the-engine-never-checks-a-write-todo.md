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

## What to fix

1. **Check every write.** `fwrite` returns the count actually written. A short
   write must be fatal to the stream and visible: the sink should latch an error
   flag, and the engine should report it. A tracer that loses data quietly is
   worse than one that refuses to start.
2. **Say it in the capture.** A stream that failed to write should leave
   evidence the reader can find — today the only signal is `pcomm read` printing
   "no stream header", a line someone has to think to look at. This belongs in
   `link_health`, where the suite already looks, alongside
   `nccl_profiler_door_shut` and `uct_ops_patch_refused`: all three are "the
   answer was withheld", and only one of them currently says so.
3. **Fail the run, not the analysis.** `mpirun exit=0` on a capture that lost
   three streams is the real indictment. The engine knows at `finalize` whether
   every stream closed clean.

## Operational note, needing a decision that is not mine

`~/pcomm-suite` is 63 GB of accumulated per-job results on a home directory that
is over an 80 GB quota. **The suite cannot produce a trustworthy board until
that is under control**, and every board run in the meantime is suspect for
reasons that have nothing to do with the code under test.

Old job directories are regenerable, but they are the user's data and deleting
63 GB is not a call this plan makes. Options worth weighing: prune runs older
than the last two, move `PCOMM_OUT_ROOT` to `gpfs_scratch` (592 GB of a 1 TB
group quota, far more headroom than home), or both. Moving the output root is
the durable fix — a suite that writes its results into a quota'd home directory
will hit this again.

## What it is worth

The suite's assertions caught the consequence on three independent cases, which
is the instruments working. But nothing named the cause: five red cases pointed
at IB and UCX behaviour while the actual fault was a filesystem quota and an
unchecked `fwrite`. Closing that diagnostic distance is the point of items 1-3.
