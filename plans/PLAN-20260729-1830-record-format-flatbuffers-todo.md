# PLAN — record format: benchmark FlatBuffers against the raw-POD `.pcs`

- **Created:** 2026-07-29 18:30 +03
- **Status:** todo (scheduled for 2026-07-30)
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
