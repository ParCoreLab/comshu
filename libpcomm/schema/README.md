# `pcomm.fbs` — the on-disk record schema

A capture is a directory per rank holding `.pcs` streams — a 32-byte
`StreamHeader` followed by a packed array of one record type — plus
`intern-blob.bin`, the byte heap interned strings point into, behind its own
`BlobHeader`. `pcomm.fbs` declares both as FlatBuffers **structs** (fixed layout,
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
so `pcomm read` names the file and refuses it rather than guessing.

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
| `tools/schema_check.py` | a record with no schema entry at all, a reordered or renamed field, a wrong `type_id`/kind/version, an enum out of step, a stale `kSchemaHash`, a broken versioning rule, a record missing from the conformance test | **the build**, plus ctest `schema_check` |
| `tests/stream_format.cpp` | the file framing: what FileSink writes, that it round-trips, and that a wrong magic / version / type / stride is refused | ctest `stream_format` |
| `tools/check_generated.sh` | committed generated code that is no longer what flatc makes of `pcomm.fbs` | ctest `schema_generated_current` |
| `tests/intern_format.cpp` | the dictionary every Handle depends on: written through Tracer, read back through `InternDictionary`, and four ways of breaking it (missing blob, truncated blob, foreign blob, stale index) each reported | ctest `intern_format` |
| `tests/record_versioning.cpp` | the versioning mechanism: captures from both sides of a schema bump reading into one table, and a stream from a newer build being reported rather than dropped | ctest `record_versioning` |
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

## Structs cannot gain fields — so records are versioned

A record's layout is frozen the moment a capture holding it exists. FlatBuffers
structs cannot gain fields, and even if they could, the files already written
would not gain them. So "add a field to `IbPostSend`" is never an edit: it is a
new record type, **same family name, next version, fresh `type_id`**.

    PCOMM_RECORD  (IbPostSend,   Event, 110, "ib_post_send")        // v1
    PCOMM_RECORD_V(IbPostSendV2, Event, 114, "ib_post_send", 2)     // v2

Records sharing a name are one **family**; the name is the stable identity that
the CSV tables, the goldens and every query are written against, while the
`type_id` is the on-disk discriminator and never means two things. The id is
just the next free one in the provider's range — there is deliberately no
arithmetic rule tying v2's id to v1's, because the ids already allocated are
irregularly spaced, so any such rule would be false for the existing records and
would run out of room the first time it mattered.

`tools/schema_check.py` enforces the rest: versions start at 1 and are
contiguous, `(name, version)` and `type_id` are each unique, and every version
of a family has the same `RecordKind`. All four rules were verified by writing
the violation and watching it fail.

The reader's half is `pcomm/rec/version_map.hpp`. The reader loads every
version present and upgrades the old ones forward one step at a time, so a
capture written before the bump and one written after produce the same table.
An upgrade takes `Stamped<Old>` to `Stamped<New>` and carries the temporal stamp
untouched — a version bump must never restamp a record. Where the old version
simply did not record something, the upgrade has to be able to say *unknown*
rather than defaulting: `device = 0` is a real device, and a query over a mixed
capture would silently attribute old records to it.

`libpcomm/tests/record_versioning.cpp` proves it end to end on a demo family.
That family lives in the test, at ids 900+ outside every allocated range, and
`schema_check.py` exempts test directories — inventing a field no provider wants
purely to have something to bump would put a permanent fiction in the format.

Streams whose `type_id` this build does not know (a capture from a newer schema,
or a provider it was built without) are **reported**, with a record count, by
both `pcomm export --csv` and `unhandled_streams`. Nothing about them makes the
rest of the read fail, which is exactly why they have to be announced: an
ignored stream produces no rows and no error, and the export looks complete.

## When a relation earns an Edge

Four archetypes, and the one that keeps being re-litigated is `Edge`. The rule
was stated once, in a comment on `GpuAccess` — "the object link is inline (hot
1:1 edge), richer relations become Edge rows" — and nowhere else, so every new
relation has had to rediscover it. It is:

**Inline the relation as a field when all three hold.** It is 1:1 from this
record's side; it is known at the moment the record is written; and it is on a
path hot enough that a second record would cost more than the field does.

**Use an Edge when any one of them fails.** The relation is 1:N or N:M; or it is
*learned later*; or it is *time-varying*.

The middle case is the load-bearing one, and it is a property of the format
rather than a preference. A stream is a packed array with no index, which is
what makes the writer a memcpy — so **a field can never be filled in
afterwards.** Revising an already-written record would mean buffering every open
one and reordering the stream, which is a different writer. `EntityClose` exists
for exactly this reason rather than being a window back-filled into the open
record, and an Edge is the general form of the same move: a fact learned after
the record was written becomes its own record.

Case 05 is what that costs when the rule is got wrong. Its 1,179,948 accesses
carry `object = 0` because NCCL's channel buffers arrive through a door gpumem
does not record — and that zero is *permanent*. There is no door through which
anything could ever fill it in. When the blocker in
`plans/PLAN-20260817-0705` lifts, whatever finally names those buffers will be a
different component learning it at a different time, and an Edge is the only
shape in the format that can carry it.

**Today `attribution` (760) has no producer**, and that is not rot: `records.hpp`
seeded one record of each archetype from day one so the writer and reader
exercise all four paths. Each predicate was then solved another way —
`TouchedObject` by the inline `gpu_access.object`, `MapsToSourceLine` by the
`source_line` record, `ResidesOn` by the inline `mem_object.device`, `MapsToQp`
by a reader-side join. That is the rule working, not the archetype failing.

By the same rule two of those inline choices are already strained: a managed
allocation's residency is time-varying and `mem_object.device` can only record
where it started, and a pc under inlined device functions maps to a *chain* of
source locations where `source_line` holds one. Both are written up in
`plans/todos.txt`; neither is urgent, and both are places where the field is
quietly asserting a cardinality that is not true.

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

## The intern dictionary

Every high-cardinality value — a pc, a pointer, a file path, a kernel name — is
stored in records as a dense `core::Handle`, which is what keeps records
fixed-stride and the writer a memcpy. So the dictionary is not context: without
it a capture is a pile of integers.

It used to be `intern-<domain>.tsv`: bare text, no magic, no version, no schema,
read by a lookup that returned `""` for anything it could not find. A missing,
truncated or foreign dictionary produced empty cells and no error —
indistinguishable from a capture that genuinely had no strings, while silently
voiding every record in it.

It is now two record streams and a blob:

- **`InternKey`** (type 510) — `handle -> uint64` for the numeric domains (`Pc`,
  `Pointer`). Fixed width already, so it is simply a record and inherits the
  stream header, the schema entry, the byte-level conformance check and the
  versioning rules with no new machinery at all.
- **`InternString`** (type 512) — `handle -> (offset, length)` into
  `intern-blob.bin`. Strings are the one thing that cannot sit in a record: a
  record is a fixed-stride array element.
- **`intern-blob.bin`** — a `BlobHeader` (magic `89 P C B L O B 0A`, format
  version, `byte_count`, schema hash) then the bytes. `byte_count` is what makes
  truncation arithmetic rather than a guess, and the distinct magic is what makes
  "this blob belongs to another capture" detectable — a mismatched blob is the
  worst case in the format, since every string resolves to the wrong text at
  plausible offsets with nothing out of place to notice.

Lookups are keyed by **(domain, handle)**, not handle alone. Handles restart at
1 in every domain, so handle 1 in `String` and handle 1 in `KernelName` are
different things; the old one-file-per-domain layout hid that by only ever
loading one of the files.

`rec::InternDictionary` (`pcomm/rec/intern_reader.hpp`) is the reader, shared by
the reader and the test so neither exercises a copy of the other's logic.

## What is still not covered

`comm_edges` and the CSV export have no schema. Both are *outputs* of
the reader rather than parts of a capture — `comm_edges` is derived from QP
state transitions and exists only in the export — so they are a different
problem from the on-disk format, and the CSV header row is the closest thing
they have to one today. Record versioning is a mechanism now, but no *real* record has been through a
bump — the proof runs on a demo family in the test. The first production
evolution is where the convention meets a provider that has to emit both
versions during a transition, and that has not happened yet.
