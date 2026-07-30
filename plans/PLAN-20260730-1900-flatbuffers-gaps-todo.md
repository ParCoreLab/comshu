# PLAN — close the gaps in the FlatBuffers adoption

- **Created:** 2026-07-30 19:00 +03
- **Status:** gaps 1-6 **done** (1-5 on 2026-07-30, 6 on 2026-07-31). 7-9 open;
  the case for 8 is now weaker than when this was written.
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
`tests/data/ib_pingpong-43997525` — pulled from MN5 job 43997525, `ok:true` —
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

### 7. Reflection is claimed but unused
Listed as a benefit; nothing uses it. Either build something real on it (a
generic `.pcs` dumper that needs no per-type C++ would be genuinely useful and
would delete code from `pcomm-read`) or stop citing it.

### 8. Structural binding instead of asserted
The strongest form: use the generated structs as the record payloads, so a
reordered field cannot compile. Byte equality is already proven for all 10, so
this is **purely ergonomics and enforcement — not correctness**, and the output
is unchanged either way. Cost is real: field access moves to `mutate_*`
accessors across both providers and `pcomm-read`.

*Do this last, if at all.* With 1-3 closed the drift risk is mostly handled, and
this is the most invasive item with the least remaining benefit.

### 9. Only records are schematised
Intern tables, `comm_edges` and the CSV export have no schema. A capture is not
self-describing end to end: the handles in a record are meaningless without the
intern tables, which no schema covers.

## Suggested order

~~1, 2 and 3 are cheap and remove silent-drift classes. 4 is small and converts
the main justification from inference to evidence. 5 is the original defect.~~
Done. 6 before the first field is needed, not during — and it is now cheaper
than it was, since `format_version` exists to be bumped and the header gives a
reader something to branch on. 7 and 8 are judgement calls; 9 is its own piece
of work, and the intern-table sidecars are the obvious next thing to frame.

Note for 8 (structural binding): with 1-3 closed, `schema_check.py` already
fails the build on a reordered or renamed field, which was most of what 8 was
for. What remains is ergonomics, at the cost of `mutate_*` accessors across both
providers and pcomm-read. The case for it is weaker now, not stronger.

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
