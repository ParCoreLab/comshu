# PLAN — close the gaps in the FlatBuffers adoption

- **Created:** 2026-07-30 19:00 +03
- **Status:** **all nine closed.** 1-5 on 2026-07-30; 6, 9 done and 7, 8 closed
  as won't-do on 2026-07-31, each with the reasoning recorded under its heading.
  What remains open is named at the end, and is not part of this plan.
- **Context commits:** `7034091` (schema + generated header), `76498db`
  (retracted a false padding claim), `449fb3b` (real padding fix), `17480f5`
  and `2e37d56` (byte-level conformance for all 10 records)
- **Prior work:** `plans/PLAN-20260729-1830-record-format-flatbuffers-*.md` has
  the benchmarks and the decision.

## What actually landed, stated precisely

`libpcomm/schema/pcomm.fbs` declares all 10 record types plus the
`Temporal`/`TimePoint`/`MemRange` vocabulary as FlatBuffers **structs**, and
`libpcomm/tests/schema_conformance.cpp` verifies every one is **byte-identical**
to the C++ record.

**We do not call FlatBuffers at runtime.** The recorder writes `Stamped<R>`
exactly as before; nothing links the FlatBuffers runtime. What was adopted is
the IDL. For FB structs that is the normal shape — a struct has no builder and
no vtable — but it means the C++/schema correspondence holds **by assertion,
not by construction**. Everything below follows from that.

## What closing 1-4 actually landed (2026-07-30)

`tools/schema_check.py` walks the tree for `PCOMM_RECORD` and cross-checks each
one against `pcomm.fbs`: fields by name/order/type, `type_id`, kind, and
presence in *both* halves of the conformance test. It runs as a **build step**
(`pcomm_schema_check`, a dependency of `pcomm_engine`), because the failure it
exists to catch is one made by forgetting. Verified by mutation, not by reading
it: an added record, a swapped field pair, a reordered enum, a changed
`type_id`, and a deleted conformance case each produce a specific failure.

The schema declares `TimeKind`/`MemOp`/`MemSpace`/`EdgePredicate` as real enums
and the conformance test asserts all 16 values (also verified by mutation —
reordering `MemOp` fires the static_assert). `tools/check_generated.sh`
regenerates both the C++ and the new Python bindings and diffs them.
`tools/read_pcs.py` reads the committed real capture
`tests/data/ib_pingpong-44025406` — pulled from MN5 job 44025406, `ok:true` —
through flatc-generated Python bindings and reproduces three of case 01's four
goldens (regions, traffic, completions) with no C++ in the loop. Topology is
not reproduced: `comm_edges` is derived by pcomm-read, not a stream on disk.

Two things found on the way, both of which had made existing checks decorative:

- **`PCOMM_FLATBUFFERS_INCLUDE` was undefined**, so the conformance test was
  silently not built at all. It had pointed at a scratch directory that no
  longer existed. The FlatBuffers headers are now vendored under
  `third_party/flatbuffers` (520 KB) and it builds everywhere, MN5 included.
- **`enable_testing()` came after `add_subdirectory(libpcomm)`** in the top-level
  `CMakeLists.txt`, so every `add_test` in libpcomm was dropped: `ctest -R
  schema` answered "No tests were found", which looks exactly like passing if
  you check the exit code of a full run. Moved above the subdirectories.

Both are the same shape as the gaps themselves — a check that is absent reports
nothing, and nothing is what success looks like.

## Gaps, most load-bearing first

### 1. A new record type can be added with no schema entry at all — **DONE**
The conformance test names each record explicitly. Add an 11th record and
nothing fails — no schema entry, no test, and every non-C++ reader silently
misses it. This is the same failure shape as the case 07 label mismatch: absence
is invisible.

*Acceptance:* adding a record without a schema entry fails the build. Options: a
registry the `PCOMM_RECORD` macro appends to, checked against the schema at
compile time; or a generator that emits the `.fbs` from the C++ definitions so
the two cannot diverge.

### 2. The committed generated header can drift from the `.fbs` — **DONE**
`schema/generated/pcomm_generated.h` is committed because MN5's login node is
airgapped and has no flatc. Nothing checks it still matches `pcomm.fbs`; edit
the schema, forget to regenerate, and the test validates against stale code
while claiming success.

*Acceptance:* CI (or a dev-build step) regenerates and diffs. Cheap and
mechanical — do this one first.

### 3. Enum values are duplicated as comments — **DONE**
`MemOp`, `MemSpace`, `TimeKind` and `EdgePredicate` appear in `pcomm.fbs` only
as prose (`// 0 Load 1 Store ...`). Reorder a C++ enum and the schema is wrong
with nothing to catch it — and unlike a layout change this does not alter sizes
or bytes, so the conformance test cannot see it either.

*Acceptance:* declare them as real `enum` in the schema and static_assert each
value against the C++ enumerator.

### 4. Portability is claimed but never demonstrated — **DONE**
The whole justification is "other languages can read captures". Nobody has done
it once. Until a non-C++ reader parses a real `.pcs`, this is a plausible
inference — the same category of claim as several that measurement overturned
this week.

*Acceptance:* a ~30-line Python reader using flatc-generated bindings that
reads a suite capture and reproduces one golden's numbers (case 01's MR/QP
counts are the smallest target). Commit it under `tools/` as executable proof.

### 5. Files do not record which schema wrote them — **DONE**
`.pcs` is still a bare array with no magic, version, `type_id` or stride. The
`.fbs` describes a *record*, not a *file*. A capture written before a schema
change is indistinguishable from one written after, so a future reader
misparses silently — the original defect that started this work, still open.

*Acceptance:* a small header (magic, format version, schema version, `type_id`,
stride). Measured free earlier: 15.13 vs 15.16 ns/rec. Note this changes the
file layout, so goldens and `pcomm-read` move together.

*Done:* `pcomm/rec/stream_header.hpp` — 32 B, keeping the records that follow
8-byte aligned, written once per file rather than per record, so the earlier
per-record measurement does not even apply. Magic is `89 P C O M M S \n` (PNG's
trick: the high bit and the trailing newline catch a text-mode copy). Carries
format_version, type_id, stride, kind and a schema hash. Declared in `pcomm.fbs`
and byte-checked by `schema_conformance` exactly like a record, so the framing
cannot drift either.

The schema hash is FNV-1a over the normalised `.fbs` — comments stripped,
whitespace collapsed — so prose does not churn it. It is *not* maintained by
hand: `schema_check.py` recomputes it and fails the build when the constant
drifts, since a stale hash asserts a sameness that is not true and is worse than
no hash at all. A capture whose hash differs but whose type_id and stride agree
is decoded with a warning naming both; refusing it would be wrong, because an
unrelated record changing moves the hash without making this stream unreadable.

Per the decision to switch fully rather than straddle: pre-format-1 captures are
**refused by name**, not read on a guess. `pcomm-read`'s stream inventory now
reads each file's own header and reports type, kind, stride, record count and
schema — the first thing in the tree that is told what a capture is by the
capture itself rather than by its filename.

`libpcomm/tests/stream_format.cpp` round-trips the format through FileSink and
checks all four rejection paths (bad magic, wrong version, wrong type, wrong
stride) plus the append-without-a-second-header case. It is header-only on
purpose — see the note below about `tests/`.

### 6. Schema evolution is a plan, not a mechanism — **DONE**
FB structs cannot gain fields; the intended answer is a new `type_id` per
version. Nothing implements or tests that, and the id ranges (100-199, 700-799)
have no versioning convention. First time a record needs a field, this gets
decided under pressure.

*Acceptance:* write the convention down, and prove it with one record carried
through a version bump that `pcomm-read` handles for both old and new captures.

*Done:* `PCOMM_RECORD_V` carries a version; records sharing a wire name are a
*family*, and the name — not the version — is the stable identity every query is
written against. The type_id is simply the next free id in the range: there is
deliberately no arithmetic rule (no "+1", no reserved digit), because the
existing ids are irregularly spaced, so any such rule would be a retrofit that
is false for the records already on disk. The link between versions lives in the
declaration, where it can be checked, rather than in the arithmetic of the
number, where it can only be assumed.

`schema_check.py` enforces four rules — contiguous versions from 1, unique
(name, version), unique type_id, constant kind across a family — each verified
by writing the violation and watching it fail. Doing that exposed a hole worth
more than the rules: the record regex required `};` to start a line, so a record
written on one line was invisible to *every* check. The first four mutations
"passed" for that reason. Fixed, and the scan now covers `.cpp` as well as
`.hpp`, with test directories exempt on purpose.

The reader's half is `pcomm/rec/version_map.hpp`: upgrades are single-step
(v1->v2->v3) rather than one function per (old, newest) pair, since the pair
count grows quadratically and each is somewhere to encode the same field twice
and get it wrong once. An upgrade carries the temporal stamp untouched, and must
be able to say *unknown* where the old version recorded nothing — `device = 0`
is a real device, and defaulting would silently attribute old records to it.

`stream_reader.hpp` was extracted from pcomm-read/main.cpp so the test exercises
the same load path the tool does, rather than a copy of it. Verified
output-neutral: `comm_edges` still matches case 01's topology golden exactly.

Streams with an unknown type_id are now reported by `pcomm-read --csv` with a
record count. That is the same failure one level up: the file is skipped, no row
appears, nothing errors, and the export looks complete. Checked both ways — the
real fixture is silent, and a capture with a type this build lacks names it.

### 7. Reflection is claimed but unused — **CLOSED: stop citing it** (2026-07-31)
Listed as a benefit; nothing uses it. Either build something real on it (a
generic `.pcs` dumper that needs no per-type C++ would be genuinely useful and
would delete code from `pcomm-read`) or stop citing it.

*Decision:* stop citing it. The claim is gone from `pcomm.fbs`; codegen stays,
because `read_pcs.py` demonstrates it on a real capture.

The C++ reflection path was costed before rejecting it, and it does not pay:

- **What it buys:** the `dump()` switch in pcomm-read, 98 lines. Nothing else.
  It cannot touch the 159-line CSV export, which is where the value is — that
  code resolves interned handles and maps `opcode == 2` to `"send"`, and those
  mappings are semantic. Reflection knows a field is a uint32; it does not know
  it is an interned handle, and the goldens are written against the names.
- **What it costs:** `GetAnyValueS` is declared in the header we vendor but
  *defined* in `src/reflection.cpp`, which we do not. So pcomm would link the
  FlatBuffers runtime for the first time — today nothing does, which is why the
  vendored headers are 520 KB and MN5 builds without incident. Plus a committed
  `.bfbs` as a third generated artifact that can go stale, needing its own
  entry in `check_generated.sh`.

Trading a zero-dependency build for 98 lines of a debug command is a bad trade.

*If a generic dumper is ever wanted*, build it in Python: `schema_check.py`
already parses `pcomm.fbs` into field names, types and order; `StreamHeader`
supplies type_id and stride from the file itself; FlatBuffers struct alignment
is mechanical; and the result is testable against the committed fixture, where
`schema_conformance` has already proven the layout matches the schema. No
linking, no `.bfbs`, and it would work on record types the C++ build has never
heard of — which the switch structurally cannot.

### 8. Structural binding instead of asserted
The strongest form: use the generated structs as the record payloads, so a
reordered field cannot compile. Byte equality is already proven for all 10, so
this is **purely ergonomics and enforcement — not correctness**, and the output
is unchanged either way. Cost is real: field access moves to `mutate_*`
accessors across both providers and `pcomm-read`.

*Do this last, if at all.* With 1-3 closed the drift risk is mostly handled, and
this is the most invasive item with the least remaining benefit.

*Decision (2026-07-31): won't do.* Two things changed since this was written.

**The benefit is already delivered.** The whole point was "a reordered field
cannot compile". `schema_check.py` already fails the *build* on a reordered or
renamed field, verified by mutation. Structural binding would turn a build-step
failure into a compile error — a better diagnostic, not a new guarantee.

**The cost is higher than this entry assumed.** "Use the generated structs as
the record payloads" is not possible as stated: every schema struct *begins*
with `temporal:Temporal`, so they model `Stamped<R>`, not `R`. That is exactly
what `PCOMM_SCHEMA_MATCH` asserts — `sizeof(Stamped<CppRecord>) ==
sizeof(FbStruct)`. Adopting them would mean restructuring how the recorder
stamps records, not swapping a type. On top of that, C++ records derive from
`RecordMeta` and the name base to carry kind/type_id/version — metadata a flatc
struct has nowhere to put — and field access across both providers and
pcomm-read moves to `mutate_*` accessors.

So it buys a better error message for a change that touches the hot-path record
shape. Revisit only if the record set grows large enough that schema_check's
text-level field matching starts missing things it cannot see.

### 9. Only records are schematised — **DONE** (2026-07-31)
Intern tables, `comm_edges` and the CSV export have no schema. A capture is not
self-describing end to end: the handles in a record are meaningless without the
intern tables, which no schema covers.

*Done, for the part that is actually a capture.* The intern dictionary was the
load-bearing half: a `Handle` means nothing without it, so its failure modes
void the whole capture rather than one record — and every one of them was
silent, because the lookup returned `""` for anything missing.

The split that made this smaller than it reads: two of the five domains (`Pc`,
`Pointer`) are `handle -> uint64`, already fixed width. Those are simply a
record — `InternKey`, type 510 — and inherit the stream header, schema entry,
byte-level conformance and versioning rules with **no new machinery**. Only
strings needed anything new, because a record is a fixed-stride array element:
`InternString` (512) holds `(offset, length)` into `intern-blob.bin`, which
carries a `BlobHeader` with its own magic and a `byte_count`.

The distinct blob magic matters more than it looks. A blob paired with the wrong
capture is the worst case the format has: every string resolves to the wrong
text, at plausible offsets, with nothing structurally out of place. `byte_count`
makes truncation arithmetic instead of a guess, and a bounds check per entry
catches an index and a blob from different runs.

Lookups are keyed by (domain, handle). Handles restart at 1 per domain, so
handle 1 in `String` and handle 1 in `KernelName` collide — the old
one-file-per-domain layout hid that by only ever loading `intern-string.tsv`,
which also meant `KernelName` and `TypeName` were never resolved at all.

`rec::InternDictionary` is shared by pcomm-read and the test, the same lesson as
`stream_reader.hpp` in gap 6. `tests/intern_format.cpp` writes through Tracer
and reads back through it, then breaks the capture four ways — missing blob,
truncated blob, foreign blob, stale index — and checks each is reported. One of
those four caught my own wrong expectation rather than a bug: a shrunken but
self-consistent blob is not "truncated", it is an index/blob mismatch, and the
code said so more precisely than the assertion did.

`read_pcs.py` resolves the dictionary too, so "another language can read a
capture" is now true end to end rather than only for the records — the committed
fixture's `mr` handles resolve to real addresses from Python.

**Coverage, stated honestly.** The suite is GREEN (all 7 cases, jobs
44025406-44027093) and four cases write `510.pcs`, so the numeric path is
exercised on real hardware. **No suite case interns a string**: there is no
`512.pcs` and no `intern-blob.bin` anywhere in the run, and checking the
pre-change captures confirms the old TSV sidecars were absent too — gpumem
simply does not intern in these workloads. So the blob path is covered by
`tests/intern_format.cpp` (which writes through the real Tracer and reads
through the real reader, including four corruption cases) and by nothing on the
cluster. A workload that produces source-line attribution would exercise it;
none of the seven does.

A latent bug turned up while writing that test, in code from this same change.
`InternTable::next_handle()` is shared between strings and keys, but `strings()
` returned only text — so both the old TSV dump and my first version of the
record writer reconstructed handles by counting. That is correct only while a
domain holds strings *or* keys and never both; one key interned into a string
domain would shift every later string onto the wrong handle, producing a
perfectly well-formed dictionary in which those records resolve to the wrong
text. Handles now travel with the strings, and the test interns a key into a
string domain on purpose.

*Not done, deliberately:* `comm_edges` and the CSV export. Both are **outputs**
of pcomm-read, not parts of a capture — `comm_edges` is derived from QP state
transitions and exists nowhere on disk. Schematising an export is a different
problem from framing a capture, and lumping them together is what made this gap
look bigger than the part that mattered.

## Suggested order

All nine are closed. 6 landed before the first field needed it rather than
during, which was the point. 7 and 8 were judgement calls and both went the same
way — the benefit was already delivered by cheaper means, and the cost was
higher than the entry assumed. 9 turned out to be two problems wearing one
heading: the intern dictionary (a capture, and load-bearing) and the CSV export
(an output, and a different problem).

## Still open, elsewhere

- **No real record has been through a version bump.** The mechanism is proven on
  a demo family in a test. The first production evolution is where it meets a
  provider that has to emit both versions during a transition.
- **`pcomm_engine_intern_key` takes a process-wide mutex per interned value**,
  so the ibverbs firehose takes a lock per post_send. `recorder.hpp` flags this
  as skeleton — the lock-free per-thread design with a merge in the reader
  replaces it. Unmeasured, and a far bigger capture-time question than anything
  in this plan.
- **`intern_table.hpp`'s header comment describes the intended design as the
  current one** ("per-thread, per-domain — no locks on the hot path"), which is
  not what the code does. The same shape of claim this plan spent nine items
  removing from the record format.
- **`comm_edges` and the CSV export have no schema** — outputs rather than
  captures, see gap 9.

## Found while doing 5: `tests/` has not compiled since the engine ABI change

`tests/core_smoke.cpp` still uses `Recorder::buffer<R>()`, which stopped
existing in `4dd6a69` when the arena moved behind the C ABI; `fork_smoke` and
`thread_flush_smoke` link `pcomm::pcomm` (INTERFACE, header-only) but not
`pcomm::engine`, which now owns `Tracer::instance`, `ThreadBufferRegistry::
instance` and `pcomm_engine_arena*`. All three fail to build. Nothing noticed
because the MN5 build passes `-DPCOMM_BUILD_TESTS=OFF` and, until this week,
`ctest` locally reported "No tests were found" rather than a failure.

**Fixed** in the gap-5 commit, because otherwise nothing here could be verified
locally. `core_smoke` now exercises `ThreadLocalBuffer` directly for the parts
where a locally-held `ISink` still means something, and goes through the Tracer
for the parts that reach disk; `thread_flush_smoke` does the same. All 9 tests
build and pass — the first green `ctest` since 2026-07-15.

The cause was a missing gate, not a missed failure, so `tools/build_all.sh` and
a `pre-commit` hook landed with it: every buildable configuration, tests ON,
loud about what it skipped.

## Watch out for

- **The suite is the real oracle.** Cases 01-07 assert exact counts, so a layout
  slip shows as a golden diff rather than silence. Run the full suite for
  anything touching record layout — ~25-30 min, one job at a time on `acc_debug`.
- **MN5 has no flatc and no network.** Anything requiring codegen at build time
  will not work there; keep the generated header committed.
- **Do not trust a green conformance test as proof of a change you did not
  measure.** It passed on size alone until `17480f5`, and would have accepted a
  reordered field.
