#!/usr/bin/env python3
"""Generate the raw record -> DuckDB column tables for `pcomm export --parquet`.

Why generated rather than written: a raw table is a mechanical projection of a
record struct — one column per field, the field's own type — and hand-writing
27 of those is 27 chances to describe a column differently from the struct it
came from. That is the drift `Table<>` already exists to prevent, one layer
down, and it would be worse here because a wrong *type* is silent where a wrong
name is loud.

Source of truth is the C++ record declarations, read with schema_check.py's
parser — the same one that already enforces the schema against them. Not the
`.fbs`: the accessors generated here are C++ expressions against the C++
structs, and `tests/schema_conformance` already proves the two layouts agree, so
parsing the C++ removes a translation step rather than adding one.

This is emphatically NOT the FlatBuffers reflection path rejected in
plans/PLAN-20260730-1900-flatbuffers-gaps-done.md (gap 7). That rejection stands
and for the same reasons: it would link the FlatBuffers runtime, which nothing
does today, and it cannot supply semantics — reflection knows a field is a
uint32, not that it is an interned handle. This script links nothing and claims
no semantics: it emits the *raw* columns, with the enum decoding and handle
resolution left to SQL above. `--csv` keeps the semantic layer, unchanged.

Build-time only, and only when PCOMM_DUCKDB=ON, so the header is never committed
and cannot go stale — the third-generated-artifact cost gap 7 warned about is
not paid here.

  tools/gen_raw_columns.py <repo-root> <output-header>
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import schema_check as sc  # noqa: E402

# C++ field type -> (DuckDB type, duckdb_append_* suffix, cast needed).
# Closed by construction: an unknown type is a hard error below rather than a
# skipped column, because a silently dropped column is a table that looks
# complete and is not.
SCALARS = {
    "std::uint8_t":  ("UTINYINT",  "uint8",  None),
    "std::uint16_t": ("USMALLINT", "uint16", None),
    "std::uint32_t": ("UINTEGER",  "uint32", None),
    "std::uint64_t": ("UBIGINT",   "uint64", None),
    "std::int32_t":  ("INTEGER",   "int32",  None),
    # Aliases from core/ids.hpp and core/vocab.hpp. Spelled out rather than
    # resolved transitively: the alias is the vocabulary the records are
    # written in, and pinning it here makes a change to it a change here.
    "Handle":        ("UINTEGER",  "uint32", None),
    "DeviceId":      ("USMALLINT", "uint16", None),
    "DomainId":      ("USMALLINT", "uint16", None),
    "Ticks":         ("UBIGINT",   "uint64", None),
}

# Enums land as their underlying uint8. Decoded to names in SQL, not here —
# that mapping is semantic and belongs with the derived tables.
ENUMS = {"TimeKind", "MemOp", "MemSpace", "EdgePredicate", "RecordKind"}


def flatten(fields, structs, prefix, expr):
    """(column_name, duckdb_type, append_suffix, accessor) for a field list.

    Nested structs (Temporal, TimePoint, MemRange) are flattened with '_', so
    Temporal.a.ticks becomes temporal_a_ticks. Flattened rather than kept as a
    DuckDB STRUCT: every consumer of these tables is a SQL query written by
    hand or by a model, and a flat column is what both can reach without
    knowing the nesting.
    """
    out = []
    for name, ctype in fields:
        col = f"{prefix}{name}"
        acc = f"{expr}.{name}"
        # A provider's records live outside `pcomm::core`, so they must spell the
        # shared vocabulary qualified: `core::DeviceId` where core's own records
        # write `DeviceId`. Same word, same type, same column — the namespace is
        # a property of where the struct is declared, not of the vocabulary.
        #
        # Stripping the prefix is NOT the transitive alias resolution the SCALARS
        # comment refuses. That refusal is about `DeviceId -> std::uint16_t`,
        # where following the alias would let a change to it pass unnoticed here.
        # This follows nothing: `core::DeviceId` and `DeviceId` are the same
        # entry, and adding a genuinely new alias still has to be spelled out.
        #
        # It refuses instead of guessing when it cannot map a type, which is what
        # caught this: providers/ucx gained a `core::DeviceId` field and the
        # generator exited 1 and wrote NOTHING, rather than dropping the column
        # from every DuckDB consumer while --csv kept exporting it. That is the
        # nccl_comm failure below, arriving from the other direction.
        ctype = ctype[6:] if ctype.startswith("core::") else ctype
        if ctype in SCALARS:
            ddb, suffix, _ = SCALARS[ctype]
            out.append((col, ddb, suffix, acc))
        elif ctype in ENUMS:
            out.append((col, "UTINYINT", "uint8", f"static_cast<std::uint8_t>({acc})"))
        elif ctype in structs:
            out.extend(flatten(structs[ctype], structs, col + "_", acc))
        else:
            sc.fail(f"gen_raw_columns: no DuckDB mapping for C++ type {ctype!r} "
                    f"(field {col}). Add it to SCALARS/ENUMS — do not drop it.")
    return out


# A record field whose name collides with an injected column.
#
# `rank` is injected into every raw table and means THE CAPTURE RANK. NcclComm
# also has a field called `rank`, meaning this process's rank INSIDE the
# communicator — a different number entirely, and 3 of 4 on a 4-rank job need not
# be 3. Emitted as-is that produced CREATE TABLE nccl_comm(... "rank" ... "rank"
# ...), which DuckDB refuses: "Column with name rank already exists". So the
# table did not mount, and nccl_comm was absent from every DuckDB consumer — the
# UI, the agent, `pcomm sql` — while --csv exported it fine, because the CSV
# writer had already hit this and renamed it by hand.
#
# The two paths disagreeing is the actual defect; this makes them agree, on the
# name --csv chose (pcomm-read/main.cpp, Table<NcclComm>::cols).
#
# CLOSED BY CONSTRUCTION. A collision not listed here is a hard error, not a
# silently duplicated column: the failure mode is a table that vanishes with one
# line on stderr, which is exactly what hid this one.
RENAMES = {
    ("nccl_comm", "rank"): "nccl_rank",
}

INJECTED = {"rank"}


def rename_collisions(rec, cols):
    out = []
    for c, t, suffix, acc in cols:
        if c in INJECTED:
            key = (rec["wire"], c)
            if key not in RENAMES:
                sc.fail(f"{rec['wire']}.{c} collides with the injected `{c}` column "
                        f"and has no entry in RENAMES — add one, and match the "
                        f"--csv column name in pcomm-read/main.cpp for it")
                continue  # the run dies below; do not index past the failure
            c = RENAMES[key]
        out.append((c, t, suffix, acc))
    return out


def namespace_of(path):
    """The namespace a record is declared in, read from its own file."""
    for line in path.read_text().splitlines():
        line = line.strip()
        if line.startswith("namespace ") and line.endswith("{"):
            ns = line[len("namespace "):-1].strip()
            if "=" not in ns and ns:          # skip `namespace x = y;` aliases
                return ns
    sc.fail(f"gen_raw_columns: no namespace found in {path}")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, out_path = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])

    records = sc.parse_records(root)
    structs, _enums = sc.parse_vocab(root)
    temporal = structs["Temporal"]

    # The header must stand on its own: it names every record type, so it has to
    # include every header that declares one. Derived from where the records
    # actually live rather than hard-coded, so a record declared somewhere new
    # is included automatically instead of failing to compile in whichever
    # consumer did not already include it by luck. (pcomm-read did; pcomm-ui did
    # not, which is how this was found.)
    includes = set()
    for rec in records.values():
        p = rec["path"].as_posix()
        marker = "libpcomm/include/"
        includes.add(p[p.index(marker) + len(marker):] if marker in p else pathlib.Path(p).name)

    lines = [
        "// GENERATED by tools/gen_raw_columns.py — do not edit, do not commit.",
        "// Raw record -> DuckDB column tables. See the generator for why.",
        "#pragma once",
        "#include <cstdint>",
        "#include <tuple>",
        '#include "pcomm/rec/thread_local_buffer.hpp"',
        *(f'#include "{h}"' for h in sorted(includes)),
        "",
        "namespace pcomm::raw {",
        "",
        "template<class R> struct RawTable;",
        "",
    ]
    type_names = []

    for name in sorted(records):
        rec = records[name]
        ns = namespace_of(root / rec["path"])
        qualified = f"{ns}::{name}"
        type_names.append(qualified)

        # rank is not in any record — it comes from the directory the stream was
        # found in — and every table needs it to be joinable across ranks.
        cols = [("rank", "INTEGER", "int32", "rank")]
        cols += flatten(temporal, structs, "temporal_", "s.temporal")
        cols += rename_collisions(rec, flatten(rec["fields"], structs, "", "s.payload"))

        ddl = ",".join(f'\\"{c}\\" {t}' for c, t, _, _ in cols)
        lines += [
            f"// type {rec['type_id']}, {rec['kind']}",
            f"template<> struct RawTable<{qualified}> {{",
            f'    static constexpr const char* table = "{rec["wire"]}";',
            f'    static constexpr const char* ddl = "CREATE TABLE \\"{rec["wire"]}\\"({ddl})";',
            f"    static void append(duckdb_appender ap, std::int32_t rank,",
            f"                       const pcomm::rec::Stamped<{qualified}>& s) {{",
        ]
        for _c, _t, suffix, acc in cols:
            lines.append(f"        duckdb_append_{suffix}(ap, {acc});")
        lines += ["    }", "};", ""]

    lines += [
        "// Every record type this build can export raw. Includes the intern",
        "// streams, which --csv consumes as a dictionary rather than exporting:",
        "// at this layer they are records like any other, and a capture whose",
        "// dictionary is unqueryable is a capture whose handles cannot be",
        "// resolved by anything downstream.",
        "using RawRecords = std::tuple<",
        ",\n".join(f"    {t}" for t in type_names),
        ">;",
        "",
        "}  // namespace pcomm::raw",
        "",
    ]

    # sc.fail only APPENDS to schema_check's error list — it does not exit. This
    # script never looked at that list, so every "hard error" above was in fact a
    # silently dropped column and a table that looked complete: exactly the shape
    # the comment at the top of this file says it is closed against. Checked now,
    # before anything is written, so a broken header is never produced at all.
    if sc.errors:
        for e in sc.errors:
            print(f"gen_raw_columns: {e}", file=sys.stderr)
        sys.exit(1)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines))
    print(f"gen_raw_columns: {len(type_names)} record types -> {out_path}")


if __name__ == "__main__":
    main()
