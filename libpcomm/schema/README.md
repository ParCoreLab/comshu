# `pcomm.fbs` — the on-disk record schema

A `.pcs` file is a packed array of one record type. `pcomm.fbs` declares those
layouts as FlatBuffers **structs** (fixed layout, inline, no vtables), so a
capture is a valid FlatBuffers struct array and can be read from any language
flatc supports.

Adopting it changed no layout and cost nothing: the structs are byte-identical
to what the recorder already wrote (`sizeof` matches for all 10 record types),
and writing them measured 9.48 ns/record against 10.13 for the hand-written POD.

`generated/pcomm_generated.h` is committed deliberately — MN5's login node is
airgapped and has no flatc, so the build must not depend on running it.
Regenerate after editing the schema:

    flatc --cpp -o generated/ pcomm.fbs

`libpcomm/tests/schema_conformance.cpp` static_asserts that every C++ record
still matches its struct here. It is the only thing preventing silent drift:
change a record without the schema and non-C++ readers misparse every capture,
with no error anywhere.

**Structs cannot gain fields.** Record types are already versioned by `type_id`
(ibverbs 100-199, internal 500-599, gpumem 700-799), so a schema change means a
new `type_id`, not an edit in place. That is the deliberate trade for making
writes free — tables would allow evolution at ~2.3x the write cost.
