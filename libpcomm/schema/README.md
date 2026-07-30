# `pcomm.fbs` — the on-disk record schema

A `.pcs` file is a 32-byte `StreamHeader` followed by a packed array of one
record type. `pcomm.fbs` declares both as FlatBuffers **structs** (fixed layout,
inline, no vtables), so a capture is a valid FlatBuffers struct array and can be
read from any language flatc supports.

The header is what makes a capture self-describing: magic, format version,
`type_id`, `stride`, `kind`, and a hash of the schema that wrote it. Before it
existed, which record type a file held came from its *filename* and which schema
wrote it came from nowhere at all — a capture written before a schema change was
byte-indistinguishable from one written after, so a reader that guessed wrong
misparsed in silence. It is written once when a stream file is created, never
per record, so it costs nothing on the hot path.

**Captures written before format 1 are not readable.** They have no header, and
the only thing that distinguishes them is the absence the check is looking for,
so `pcomm-read` names the file and refuses it rather than guessing.

Adopting FlatBuffers changed no record layout and cost nothing: the structs are
byte-identical to what the recorder already wrote (`sizeof` matches for all 10
record types), and writing them measured 9.48 ns/record against 10.13 for the
hand-written POD. The header added 32 bytes per *file* on top of that and
nothing per record.

**We do not call FlatBuffers at runtime.** The recorder writes `Stamped<R>` with
a memcpy exactly as before and nothing links the FlatBuffers runtime; what was
adopted is the IDL. So the C++/schema correspondence holds by check rather than
by construction, and the checks are the whole story:

| check | catches | runs as |
|---|---|---|
| `tests/schema_conformance.cpp` | a record whose layout, size or *enum values* drift from the schema — byte-level, all 10 records and the stream header | ctest `schema_conformance` |
| `tools/schema_check.py` | a record with no schema entry at all, a reordered or renamed field, a wrong `type_id`/kind, an enum out of step, a stale `kSchemaHash`, a record missing from the conformance test | **the build**, plus ctest `schema_check` |
| `tests/stream_format.cpp` | the file framing: what FileSink writes, that it round-trips, and that a wrong magic / version / type / stride is refused | ctest `stream_format` |
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

## The schema hash

`StreamHeader.schema_hash` is FNV-1a over `pcomm.fbs` with comments stripped and
whitespace collapsed, so prose edits do not churn it but any declaration change
does. The constant lives in `pcomm/rec/stream_header.hpp`;
`tools/schema_check.py` recomputes it and **fails the build** when it drifts,
because a hash that is merely *maintained* would be wrong the first time someone
edited the schema in a hurry — and a stale hash is worse than no hash, since it
asserts a sameness that is not true.

A capture whose hash differs but whose `type_id` and `stride` still agree is
decoded, with a warning naming both hashes. An unrelated record changing moves
the hash without making this stream unreadable; refusing it would be wrong, and
saying nothing is the thing the header exists to end.

## What is still not covered

Intern tables, `comm_edges` and the CSV export have no schema (gap 9), so a
capture is still not self-describing end to end: the handles in a record are
meaningless without the intern tables, and those are bare TSV sidecars with no
header of their own. Schema *evolution* is also still a convention rather than a
mechanism (gap 6) — `format_version` now exists to be bumped, but nothing
carries a record through a version bump or tests that `pcomm-read` serves both
sides of one.
