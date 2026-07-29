# PLAN — record format: benchmark FlatBuffers against the raw-POD `.pcs`

- **Created:** 2026-07-29 18:30 +03
- **Status:** measured 2026-07-30 — see RESULTS. Decision pending team review.
- **Why now:** part of the team is pushing for FlatBuffers. Settle it with
  numbers, not architecture opinions. My prior is "no for the hot path", and a
  prior is exactly the thing a benchmark should be allowed to overturn.

## What the format is today

`<dir>/<type_id>.pcs`, one file per record type. Contents are a packed array of
`Stamped<R>` = `{core::Temporal temporal; R payload;}` appended with `fwrite`
(`rec/sink.hpp` FileSink). The reader does:

```cpp
out.resize(bytes / sizeof(Stamped<R>));            // pcomm-read/main.cpp:40
in.read(reinterpret_cast<char*>(out.data()), ...);
```

**No magic, no version, no record size, no schema.** The file is only
interpretable by a binary compiled with the same struct definitions and the same
padding.

## The real defect, independent of FlatBuffers

`bytes / sizeof(Stamped<R>)` **silently misparses** any capture written before a
record struct changes — no error, wrong numbers. We compare captures and goldens
across days, so this is live. It is also an undocumented dependence on compiler
padding and endianness.

Cheap fix, zero per-record cost: a per-file header — magic, format version,
`type_id`, `sizeof(Stamped<R>)`, ideally a layout hash — turning a silent
misparse into a loud error. **Do this regardless of how the benchmark lands.**

## What to measure (the actual question)

Not "is FlatBuffers good" but "what does it cost on the write path and what does
it buy on the read path".

1. **Write throughput, per record.** Current path = bounds check + POD store into
   the arena, then one `fwrite` of a contiguous block. FlatBuffers = build a
   table with a vtable + offset patching per record, then copy. Measure
   records/sec for `RawAccess` (288 B, 32 addresses — the hot one) and for a
   small event like `IbPostSend`.
2. **Hot-path relevance.** gpumem pushes ~3.2M records/rank in case 05 and 47 MB
   in case 07; case 05's cost is already ~435s. Per-record serialization work is
   exactly what this session proved is dangerous, so measure the *injected* cost
   inside `push()`, not just a synthetic loop.
3. **Size on disk.** vtable + offsets per record vs a packed 288-byte struct, at
   realistic record counts. Include compressed size, since that changes the
   argument.
4. **Read/analysis.** Load time for `pcomm-read --csv`, and whether the
   "homogeneous streams are trivially columnar-convertible" property
   (`sink.hpp`) survives.
5. **Schema evolution.** The thing FlatBuffers genuinely gives us. Weigh it
   against a header + version, which gets *detection* but not *tolerance*.

## Alternatives to include in the comparison

- **Header + raw POD (status quo + fix).** Cheapest, keeps zero-copy writes.
- **FlatBuffers.** Schema evolution, cross-language, optional fields — none of
  which our fixed-size dense records currently need.
- **Arrow IPC.** Probably the better fit for the *analysis* side: streams are
  already homogeneous and columnar-shaped and the analysis path is SQL/CSV.
  Could be an export target rather than the on-disk write format.
- **Cap'n Proto.** Cheaper writes than FlatBuffers, same self-description story.

Worth stating plainly in the writeup: write format and interchange format do not
have to be the same thing. "Raw POD on the hot path, Arrow/FlatBuffers on
export" is a legitimate answer and may be the one that satisfies both camps.

## Deliverable

A benchmark table + a short recommendation, with the raw numbers committed so
the team can check them. If FlatBuffers wins on the write path, my prior was
wrong and we adopt it.


---

# RESULTS (measured 2026-07-30)

Benchmark: `scratchpad/fbbench/{bench,ibbench}.cpp`, real record types
(`Stamped<GpuAccess>` 80 B, `Stamped<IbPostSend>` 88 B), FlatBuffers 25.x built
with the FlatBufferBuilder API directly — the same call sequence flatc-generated
code emits. Medians of 5-7 runs.

## Write path, per record

| path | GpuAccess | IbPostSend |
|---|---|---|
| raw POD (current) | 15.16 ns | 12.05 ns |
| raw POD + file header | 15.13 ns | — |
| FlatBuffers, one message per record | 50.21 ns (3.31x) | — |
| FlatBuffers, batched per 4096 flush | 34.52 ns (2.28x) | 37.13 ns (3.08x) |

## Size

| | uncompressed | gzip -6 |
|---|---|---|
| raw POD | 244.1 MB | **33.8 MB** |
| FlatBuffers batched | **158.7 MB** (0.65x) | 44.4 MB (1.31x) |

FlatBuffers is 35-41% smaller uncompressed because our records are sparse and it
omits default-valued fields and dedups vtables. Compressed the result reverses:
a packed struct full of zeros is exactly what gzip eats.

## Read path (parse + touch one field per record)

raw POD 1.67 ns/rec (zero-copy, it is already the struct) vs FlatBuffers batched
1.42 ns/rec (0.85x). FlatBuffers is *faster* here — its file is smaller, so
touching one field moves less memory. For whole-record access raw would win.

## The number that actually decides it

Perftest `ib_send_bw`, 64 B messages, real NDR, untraced vs traced through the
launcher (job 43976008):

```
untraced   6.844666 Mpps  =  146 ns/message
traced     1.297626 Mpps  =  771 ns/message   -> tracing costs 625 ns/message
```

**The record append is 12 ns. Tracing costs 625 ns.** Serialization is under 4%
of what tracing already spends; the rest is the hook path — interception,
trampolines, timestamping. Adopting FlatBuffers would add ~25 ns x 2 records =
50 ns/message, i.e. **~8% of existing tracing overhead, ~6% of traced
throughput**.

So my stated prior ("no for the hot path, the write cost is the problem") does
not survive contact with the denominator. I estimated a 10-19% tax from a 180 ns
post_send figure in old notes; the real traced cost is 771 ns. Two other
predictions were also wrong: FlatBuffers is *smaller*, not bigger, and reads
*faster*, not slower.

## Recommendation

**Do not decide this on write-path performance** — at 2-4% of tracing cost it is
close to noise, and the argument should be made on schema evolution, tooling and
interchange, which is where FlatBuffers genuinely wins. The honest summary of
the cost side is "about 6% throughput on the most hostile realistic workload".

Two things worth separating from the decision:

- **The file header should land regardless** (measured free: 15.13 vs 15.16 ns).
  Today `bytes / sizeof(Stamped<R>)` silently misparses any capture written
  before a struct changes.
- **Write format and interchange format need not be the same.** Raw POD on the
  hot path with an Arrow/FlatBuffers export remains a legitimate answer.

## Bigger finding, unrelated to the format

Tracing costs **5.3x throughput** on small-message ibverbs (6.84 -> 1.30 Mpps).
That is the optimization target worth having, and it is ~25x larger than the
entire serialization question. Not investigated yet.

## Two bugs surfaced by this run

1. **`post_send` is not captured for perftest.** The capture has 202,000
   `poll_cq` records (112.pcs) and **no 110.pcs at all**, while case 01's
   `ibv_rc_pingpong` records sends correctly. Either perftest reaches the send
   doorbell by a path we do not hook, or the object-ops install misses it. The
   625 ns above is therefore poll_cq interception alone.
2. **Both `srun` steps wrote to `rank-0`.** Known: `RankDetector` trusts
   `SLURM_PROCID`, which is per-step (things-to-circle-back-to.md). The
   throughput numbers are perftest's own and unaffected.
