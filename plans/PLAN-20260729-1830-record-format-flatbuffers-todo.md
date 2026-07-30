# PLAN — record format: benchmark FlatBuffers against the raw-POD `.pcs`

- **Created:** 2026-07-29 18:30 +03
- **Status:** RECOMMEND ADOPT (FlatBuffers **structs**). Measured 2026-07-30.
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

## CORRECTION (job 43986473): the hook path is cheap, the RECORD path is not

The paragraph above attributes the 625 ns to "the hook path — interception,
trampolines, timestamping". That was an inference and it is wrong. Splitting it
with PCOMM_BENCH_NO_RECORD (hooks installed, nothing emitted):

```
untraced    6.711474 Mpps = 149.0 ns/msg
no-record   6.181506 Mpps = 161.8 ns/msg   -> hook path      12.8 ns  (2%)
traced      1.352219 Mpps = 739.5 ns/msg   -> record path   577.7 ns  (98%)
```

Interception is nearly free. Emitting the record is the entire cost.

**This also invalidates the absolute numbers in the microbenchmark above.** It
measured 12 ns per record append; in situ the same emission costs ~578 ns, a 48x
gap. A tight loop over a pre-built record with the arena, intern tables and sink
hot in L1 is not the workload — the real one emits one record between IB
operations, with everything cold and real work evicting the cache in between.

The *relative* comparison likely survives (both formats pay the same cache
penalty) but it should be re-measured in situ before anyone quotes "2.3x" as the
cost of FlatBuffers. Treat the ratios as provisional and the absolutes as wrong.

For the decision this makes FlatBuffers look *better*, not worse: +25 ns against
a 578 ns record path is ~4%, and against 739 ns of traced cost ~3%.

Untested hypothesis for where the 578 ns goes — flagged as a hypothesis because
this bug class has punished confident mechanisms all week: `h(cq)` interns a
handle per record and `pcomm_engine_intern_key` takes `std::lock_guard` on the
domain mutex every time. A per-record mutex plus hash lookup is the obvious
suspect, but it is unmeasured.

## Bigger finding, unrelated to the format

Tracing costs **~5x throughput** on small-message ibverbs (6.71 -> 1.35 Mpps),
and it is now localised: **98% of it is record emission, not interception**
(12.8 ns hook vs 577.7 ns record). That is the optimisation target worth having,
and it is ~25x larger than the entire serialization question.

## Two bugs surfaced by this run

1. **`post_send` is not captured for perftest.** The capture has 202,000
   `poll_cq` records (112.pcs) and **no 110.pcs at all**, while case 01's
   `ibv_rc_pingpong` records sends correctly. Either perftest reaches the send
   doorbell by a path we do not hook, or the object-ops install misses it. The
   625 ns above is therefore poll_cq interception alone.
2. **Both `srun` steps wrote to `rank-0`.** Known: `RankDetector` trusts
   `SLURM_PROCID`, which is per-step (things-to-circle-back-to.md). The
   throughput numbers are perftest's own and unaffected.


---

# CORRECTION 2: I benchmarked the wrong construct

The team's proposal (SigmaTrace `Events/Flatbuffers/Simple.fbs`) uses a
FlatBuffers **`struct`**, not a `table`. Everything above measures tables. A FB
struct is fixed-layout and inline — no vtable, no offsets — so it is genuinely
equivalent to a C struct, exactly as they said.

Re-measured with flatc-generated code from a real `GpuAccessF` schema:

| path | ns/rec | size | |
|---|---|---|---|
| raw POD (current) | 10.13 | 244.14 MB | 1.00x |
| **FlatBuffers struct** | **9.48** | **244.14 MB** | **0.94x** |

Free, and byte-for-byte the same size. `sizeof(GpuAccessF) == 80 ==
sizeof(Stamped<GpuAccess>)`, and every real field lands at the same offset
(temporal@0, pc@48, object@52, address@56, size@64, thread@76).

**So the performance objection is gone entirely.** The table numbers (2.3-3.3x,
35% smaller) describe a design nobody proposed.

## What FB structs do and do not buy

- **yes** — a portable schema (`.fbs`), codegen for other languages, reflection
- **yes** — a *defined* layout: FlatBuffers specifies alignment and
  little-endian encoding, so unlike a C struct it does not depend on compiler
  padding. This is a better fix for the silent-misparse hazard than the ad-hoc
  header I proposed.
- **no** — schema evolution. Structs are fixed; you cannot add a field. Tables
  give that at ~2.3x write cost. Since record types are already versioned by
  `type_id` (ibverbs 100-199, gpumem 700-799 ...), a new type id per schema
  version is a workable evolution path and costs nothing at runtime.

## Found while checking layout compatibility: we write uninitialised padding

The two layouts are NOT bit-identical, and the reason is ours, not FlatBuffers':
`memcmp` differs at bytes 1-5, 10-11, 26-29 — **every one of them a padding
byte**. FlatBuffers zeroes its padding. We do not.

`Temporal::instant()` returns `{TimeKind::Instant, t, {}, 0}`, an aggregate
whose padding is never initialised; assigning it copies the object
representation, garbage included. Verified against a real capture: **2000 of
2000 records have nonzero padding in `Temporal` bytes 1-7**, containing stack
residue (recognisable pointer fragments like `7f38f0b680`).

Consequences:
1. Trace files contain uninitialised process memory — minor information leak if
   captures are shared.
2. Raw `.pcs` bytes are non-deterministic across identical runs. This is
   *why* the determinism gate has to compare query output rather than files —
   previously treated as inherent, actually self-inflicted.
3. It hurts compression, so the gzip comparison above was biased against raw
   POD by its own garbage. 33.8 MB would shrink with zeroed padding.

Adopting FB structs fixes this as a side effect, since flatc-generated
constructors zero padding.

## Implementation sketch

1. Write `.fbs` for the existing record types, field-for-field (layout already
   matches, so this is transcription, not redesign).
2. Generate at build time with `flatc --cpp`; vendor flatc or require it.
3. Swap `PCOMM_RECORD` payloads for the generated structs, keeping `type_id`
   and the 100/700 ranges unchanged.
4. Validate: the suite is the oracle. Cases 01-07 assert exact counts, so a
   layout slip shows up as a golden diff rather than silence.
5. Keep the file header anyway — `.fbs` describes the record, not the file, and
   nothing yet records *which* schema version a file was written with.

Note step 4 gets easier once padding is zeroed: raw `.pcs` files become
byte-comparable across runs, which would let the determinism gate check files
directly instead of only query output.
