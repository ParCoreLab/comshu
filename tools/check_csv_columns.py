#!/usr/bin/env python3
"""Every record field must reach `pcomm export --csv`, or say why it does not.

WHY THIS EXISTS. `Mlx5DevxQp` gained a `cqn_snd` field — the join that makes a
completion attributable to a queue pair, which is the entire reason the field
was added — and it did not reach the CSV. Nothing failed. schema_check.py was
satisfied, because the struct and the .fbs agreed. schema_conformance passed,
because the bytes on the wire were right. The DuckDB path was fine, because
gen_raw_columns.py *generates* those columns from the struct and cannot drift.
Only the hand-written `--csv` writer was missing it, and the HPC suite reads
CSV, so the field existed everywhere except where every assertion looks.

That is not one slip. `Table<>` is three parallel hand-maintained lists — the
struct's fields, the `cols` string, the fprintf's conversions — and nothing
compared them. Two more were found the moment anything did:

  gpu_access.space     never exported at all. Present in the capture, in the
                       .fbs, in every DuckDB table, decoded to a name in
                       link_sql.hpp — and absent from --csv, so no HPC
                       assertion could ever have asked whether an access was to
                       global or shared memory.
  mlx5_devx_cq.obj     the interned devx object handle: the join to
                       mlx5_devx_cmd, and the identity every other DEVX table
                       exports. Missing from the record's first export, in the
                       same week the record was added, while demos/figures
                       advertised it as one of the record's three headline
                       fields.

TWO CHECKS, because the drift has two directions:

  1. COVERAGE — every leaf field of the record struct maps to a CSV column,
     either by name, by an explicit RENAMES entry, or by an explicit OMITTED
     entry that states a reason. A field that is none of those fails. This is
     the cqn_snd / space / obj case.

  2. ARITY — the number of columns in `cols` equals the number of FIELDS the
     fprintf format emits. This is the direction coverage cannot see: adding a
     column header without a value (or vice versa) shifts every later column,
     which produces a CSV that parses cleanly and means something else
     entirely. Silent corruption rather than silent absence.

     Fields, counted by the commas in the format literal — NOT conversions.
     The first version of this check counted `%` conversions and reported three
     failures that were all itself: a single column may render from several
     conversions when the value is composite, and does in three tables today
     (`%04x:%02x:%02x` for a PCI bus, `%016llx%016llx` for a UUID, `%u.%u.%u`
     for a UCX version). A comma in the format is a column separator and can be
     nothing else — a comma inside a value would break the CSV regardless — so
     commas + 1 is exactly the field count, and composite columns are none of
     this check's business.

Source of truth is the C++ record declarations, read with schema_check.py's
parser — the same one schema_check.py and gen_raw_columns.py already use, so
there is one place that knows how to read a PCOMM_RECORD and not three.

CLOSED BY CONSTRUCTION, like the tables in gen_raw_columns.py. An unmapped
field is an error, never a skip. That is the whole point: this file is only
worth having if the answer to "should I add an entry to OMITTED?" has to be
written down and read by someone, rather than defaulted to silence.

Text-level and dependency-free, for the same reason schema_check.py is: MN5's
login node has no flatc and nothing in the build may need one.

Usage: tools/check_csv_columns.py [repo_root]
"""

import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import schema_check as sc  # noqa: E402

CSV_WRITER = "pcomm-read/main.cpp"

# --- a field the CSV calls something else -----------------------------------
# Keyed (wire_name, leaf_field) -> the column(s) that carry it. A field may map
# to several columns (a 128-bit UUID printed as one string is still two fields
# accounted for) or several fields to one column.
#
# These are renames the CSV made deliberately, for readability or to avoid a
# collision. Listing them is not bureaucracy: `range_base -> base` in one table
# and `range_base -> addr` in another is exactly the kind of inconsistency that
# is invisible until written in a column.
RENAMES = {
    # MemRange flattens to base/size; the CSV picked a different noun per table
    # depending on what the range IS. Kept as-is rather than unified, because
    # renaming a shipped column breaks queries for no gain.
    ("mem_object", "range_base"): ["base"],
    ("mem_object", "range_size"): ["size"],
    ("peer_mapping", "range_base"): ["base"],
    ("peer_mapping", "range_size"): ["size"],
    ("ib_mem_region", "range_base"): ["base"],
    ("ib_mem_region", "range_size"): ["size"],
    ("mlx5_umem", "range_base"): ["addr"],
    ("mlx5_umem", "range_size"): ["size"],
    ("gpu_access", "address"): ["addr"],
    ("gpu_access", "run_device"): ["run_dev"],
    ("gpu_access", "mem_device"): ["mem_dev"],
    ("ib_post_send_sg", "index"): ["idx"],
    ("ucx_tag_recv", "count"): ["capacity"],
    # cqc.cqe_sz is an encoding (0 = 64 B, 1 = 128 B); the column carries the
    # decoded byte count, and says so in its name.
    ("mlx5_devx_cq", "cqe_size"): ["cqe_bytes"],
    # A 128-bit UUID, printed as the one string anybody would compare.
    ("gpu_device", "uuid_hi"): ["uuid"],
    ("gpu_device", "uuid_lo"): ["uuid"],
    # `rank` in a NCCL communicator is this process's rank INSIDE it — not the
    # capture rank that every CSV injects as its first column. The collision is
    # real and gen_raw_columns.py documents the DuckDB half of it; --csv chose
    # the name first, and this is that same decision, stated here too.
    ("nccl_comm", "rank"): ["nccl_rank"],
    ("nccl_prof_comm", "rank"): ["nccl_rank"],
    # A direction is more legible than a boolean, and the column renders it as
    # one ("send"/"recv").
    ("nccl_proxy_op", "is_send"): ["dir"],
}

# --- a field that deliberately does not appear ------------------------------
# Every entry needs a reason, and "padding" is a real reason. Anything else
# should read as a decision someone would defend.
OMITTED = {
    ("capture_status", "pad"): "explicit struct padding, not data",
    ("capture_status", "pad2"): "explicit struct padding, not data",
    ("uct_iface", "pad0"): "explicit struct padding, not data",
    ("uct_iface", "pad1"): "explicit struct padding, not data",
    ("uct_send", "pad0"): "explicit struct padding, not data",
    ("uct_send", "pad1"): "explicit struct padding, not data",
    ("uct_binding", "pad0"): "explicit struct padding, not data",
    ("uct_binding", "pad1"): "explicit struct padding, not data",
    ("nccl_comm", "pad"): "explicit struct padding, not data",
    ("nccl_collective", "pad"): "explicit struct padding, not data",
    ("nccl_p2p", "pad"): "explicit struct padding, not data",
    ("nccl_window", "pad"): "explicit struct padding, not data",
    ("nccl_dev_comm", "pad"): "explicit struct padding, not data",
    ("nccl_prof_comm", "pad"): "explicit struct padding, not data",
    ("nccl_coll_exec", "pad"): "explicit struct padding, not data",
    ("mlx5_send_coverage", "pad0"): "explicit struct padding, not data",
    ("mlx5_send_coverage", "pad1"): "explicit struct padding, not data",
}

# --- a record with no Table<> at all ----------------------------------------
# The two intern streams are consumed AS the dictionary that resolves every
# Handle in every other table, not exported as tables of their own. main.cpp
# says so where known_types() is built. Anything else missing a Table<> is a
# record `pcomm export --csv` cannot emit, which is this file's failure one
# level up.
NO_TABLE = {
    "intern_key": "consumed as the intern dictionary, not exported as a table",
    "intern_string": "consumed as the intern dictionary, not exported as a table",
}

TABLE_RE = re.compile(r"struct Table<([\w:]+)>\s*\{(.*?)\n\};", re.S)
NAME_RE = re.compile(r'name\s*=\s*"(\w+)"')
COLS_RE = re.compile(r'cols\s*=\s*((?:"[^"]*"\s*)+)')
# The fprintf's format argument: the first string literal run after `f,`.
FMT_RE = re.compile(r"fprintf\(\s*\n?\s*f,\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+)", re.S)


def leaves(fields, structs, prefix=""):
    """Leaf field names, nested structs flattened with '_' — the same shape
    gen_raw_columns.py produces, so a name that works there works here."""
    out = []
    for name, ctype in fields:
        ctype = ctype[6:] if ctype.startswith("core::") else ctype
        if ctype in structs:
            out.extend(leaves(structs[ctype], structs, prefix + name + "_"))
        else:
            out.append(prefix + name)
    return out


def main():
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    records = sc.parse_records(root)
    structs, _ = sc.parse_vocab(root)
    by_wire = {r["wire"]: r for r in records.values()}

    src = (root / CSV_WRITER).read_text()
    stripped = sc.strip_comments(src)
    tables = TABLE_RE.findall(stripped)
    if not tables:
        sc.fail(f"check_csv_columns: no Table<> specialisations found in {CSV_WRITER} — "
                "the parser has drifted from the file it reads")

    errors = []
    seen_wires = set()

    for cpp_type, body in tables:
        nm, cl = NAME_RE.search(body), COLS_RE.search(body)
        if not nm or not cl:
            errors.append(f"Table<{cpp_type}>: could not read its name/cols")
            continue
        wire = nm.group(1)
        seen_wires.add(wire)
        cols = "".join(re.findall(r'"([^"]*)"', cl.group(1))).split(",")

        # --- check 2: arity ---------------------------------------------
        fmt = FMT_RE.search(body)
        if not fmt:
            errors.append(f"{wire}: could not find the fprintf format string")
        else:
            literal = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', fmt.group(1)))
            n_fields = literal.count(",") + 1
            if n_fields != len(cols):
                errors.append(
                    f"{wire}: {len(cols)} columns in `cols` but the format string emits "
                    f"{n_fields} fields. Every column after the mismatch carries the WRONG "
                    f"field's value — a CSV that parses and lies.")

        # --- check 1: coverage ------------------------------------------
        rec = by_wire.get(wire)
        if rec is None:
            errors.append(f"{wire}: Table<> exports a wire name no PCOMM_RECORD declares")
            continue
        colset = set(cols)
        for field in leaves(rec["fields"], structs):
            key = (wire, field)
            if field in colset:
                continue
            if key in OMITTED:
                continue
            if key in RENAMES:
                absent = [c for c in RENAMES[key] if c not in colset]
                if absent:
                    errors.append(
                        f"{wire}.{field}: RENAMES points at {absent}, which `cols` does not have")
                continue
            errors.append(
                f"{wire}.{field}: in the record, absent from --csv. Add a column, or add a "
                f"RENAMES/OMITTED entry saying why not. This is the cqn_snd failure.")

    # A record with no Table<> at all is invisible to --csv entirely, which is
    # the same defect one level up.
    for wire in sorted(by_wire):
        if wire not in seen_wires and wire not in NO_TABLE:
            errors.append(f"{wire}: PCOMM_RECORD has no Table<> — "
                          f"`pcomm export --csv` cannot emit it at all")

    # A stale entry is a lie about the current code, so it fails too.
    known = {w for w, _ in list(RENAMES) + list(OMITTED)}
    for wire in sorted(known - seen_wires):
        errors.append(f"{wire}: RENAMES/OMITTED mentions a table that no longer exists")
    for wire in sorted(set(NO_TABLE) - set(by_wire)):
        errors.append(f"{wire}: NO_TABLE mentions a record that no longer exists")

    if errors:
        print(f"check_csv_columns: {len(errors)} problem(s)", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        sys.exit(1)
    print(f"check_csv_columns: {len(tables)} tables, every record field accounted for")


if __name__ == "__main__":
    main()
