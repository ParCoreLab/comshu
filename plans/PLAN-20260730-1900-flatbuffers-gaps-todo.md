# PLAN — close the gaps in the FlatBuffers adoption

- **Created:** 2026-07-30 19:00 +03
- **Status:** gaps 1-4 **done** 2026-07-30 (see below). 5-9 open; 5 is next, and
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
`tests/data/ib_pingpong-43996904` — pulled from MN5 job 43996904, `ok:true` —
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

### 5. Files do not record which schema wrote them
`.pcs` is still a bare array with no magic, version, `type_id` or stride. The
`.fbs` describes a *record*, not a *file*. A capture written before a schema
change is indistinguishable from one written after, so a future reader
misparses silently — the original defect that started this work, still open.

*Acceptance:* a small header (magic, format version, schema version, `type_id`,
stride). Measured free earlier: 15.13 vs 15.16 ns/rec. Note this changes the
file layout, so goldens and `pcomm-read` move together.

### 6. Schema evolution is a plan, not a mechanism
FB structs cannot gain fields; the intended answer is a new `type_id` per
version. Nothing implements or tests that, and the id ranges (100-199, 700-799)
have no versioning convention. First time a record needs a field, this gets
decided under pressure.

*Acceptance:* write the convention down, and prove it with one record carried
through a version bump that `pcomm-read` handles for both old and new captures.

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
the main justification from inference to evidence.~~ Done. 5 is the original
defect. 6 before the first field is needed, not during. 7 and 8 are judgement
calls; 9 is its own piece of work.

Note for 8 (structural binding): with 1-3 closed, `schema_check.py` already
fails the build on a reordered or renamed field, which was most of what 8 was
for. What remains is ergonomics, at the cost of `mutate_*` accessors across both
providers and pcomm-read. The case for it is weaker now, not stronger.

## Watch out for

- **The suite is the real oracle.** Cases 01-07 assert exact counts, so a layout
  slip shows as a golden diff rather than silence. Run the full suite for
  anything touching record layout — ~25-30 min, one job at a time on `acc_debug`.
- **MN5 has no flatc and no network.** Anything requiring codegen at build time
  will not work there; keep the generated header committed.
- **Do not trust a green conformance test as proof of a change you did not
  measure.** It passed on size alone until `17480f5`, and would have accepted a
  reordered field.
