# `pcomm.fbs` — the on-disk record schema

A `.pcs` file is a packed array of one record type. `pcomm.fbs` declares those
layouts as FlatBuffers **structs** (fixed layout, inline, no vtables), so a
capture is a valid FlatBuffers struct array and can be read from any language
flatc supports.

Adopting it changed no layout and cost nothing: the structs are byte-identical
to what the recorder already wrote (`sizeof` matches for all 10 record types),
and writing them measured 9.48 ns/record against 10.13 for the hand-written POD.

**We do not call FlatBuffers at runtime.** The recorder writes `Stamped<R>` with
a memcpy exactly as before and nothing links the FlatBuffers runtime; what was
adopted is the IDL. So the C++/schema correspondence holds by check rather than
by construction, and the checks are the whole story:

| check | catches | runs as |
|---|---|---|
| `tests/schema_conformance.cpp` | a record whose layout, size or *enum values* drift from the schema — byte-level, all 10 records | ctest `schema_conformance` |
| `tools/schema_check.py` | a record with no schema entry at all, a reordered or renamed field, a wrong `type_id`/kind, an enum out of step, a record missing from the conformance test | **the build**, plus ctest `schema_check` |
| `tools/check_generated.sh` | committed generated code that is no longer what flatc makes of `pcomm.fbs` | ctest `schema_generated_current` |
| `tools/read_pcs.py` | the portability claim itself — reads a real capture from Python and recomputes case 01's goldens | ctest `schema_python_reader` |

`schema_check.py` is attached to the build on purpose. `schema_conformance` can
only check the records it names, so the one failure it structurally cannot see
is an eleventh record that nobody added to it — and a check you have to remember
to run is no check against a mistake made by forgetting.

## Generated code is committed

`generated/pcomm_generated.h` (C++) and `generated-python/` (Python) are both
committed deliberately: MN5's login node is airgapped and has no flatc, so the
build must not depend on running it. `check_generated.sh` is what keeps them
from going stale. Regenerate both after editing the schema:

    cd libpcomm/schema
    flatc --cpp    -o generated/        pcomm.fbs
    flatc --python -o generated-python/ pcomm.fbs

The FlatBuffers headers are vendored under `third_party/flatbuffers` (520 KB,
header-only) so the conformance test builds everywhere including MN5, and the
pure-Python runtime alongside them so `read_pcs.py` needs no pip install. Both
come from **v25.12.19**, the flatc that generated the committed output; the
generated header static_asserts that version against the headers, so a
mismatched toolchain fails at build time rather than silently. `tools/get_flatc.sh`
builds that exact version into `.tools/flatc` (needs network — not on MN5).

## Enums are declared, not described

`TimeKind`, `MemOp`, `MemSpace` and `EdgePredicate` are real `enum`s here. They
used to be prose comments, which is the one drift the byte-level test cannot
see: reorder a C++ enumerator and every record keeps its size, its offsets and
(for any given test vector) its bytes, while every reader that decodes the field
by name now decodes it wrong. `schema_conformance` asserts each value
individually. **The values are on disk — do not renumber either side.**

## Structs cannot gain fields

Record types are versioned by `type_id` (ibverbs 100-199, internal 500-599,
gpumem 700-799), so a schema change means a new `type_id`, not an edit in place.
That is the deliberate trade for making writes free — tables would allow
evolution at ~2.3x the write cost. The convention for *how* a version bump is
numbered, and how `pcomm-read` serves both old and new captures, is still
neither written down nor tested: gap 6 in
`plans/PLAN-20260730-1900-flatbuffers-gaps-todo.md`.

## What is still not covered

The file itself records nothing about which schema wrote it — no magic, no
format version, no `type_id`, no stride. `read_pcs.py` takes the record type
from the *filename*, the same convention pcomm-read relies on, which is exactly
the original defect (gap 5). Intern tables, `comm_edges` and the CSV export have
no schema at all (gap 9), so a capture is not self-describing end to end: the
handles in a record are meaningless without the intern tables.
