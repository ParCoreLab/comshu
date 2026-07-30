#!/usr/bin/env python3
"""Cross-check every C++ record declaration against libpcomm/schema/pcomm.fbs.

tests/schema_conformance.cpp proves the ten records we *named* in it are
byte-identical to their schema structs. It cannot prove anything about a record
nobody named: add an eleventh PCOMM_RECORD, write no schema entry, add no test
line, and every check in the tree still passes while every non-C++ reader
silently misses the new stream. Absence is invisible — the same failure shape as
the case 07 label mismatch.

So this walks the *sources* rather than a list: every PCOMM_RECORD in the repo
must have a struct in pcomm.fbs whose fields match one-for-one in name, order
and type, must carry the type_id and kind the schema claims for it, and must be
covered by both halves of the conformance test. Every enum a record field uses
must be declared in the schema with the same values, and every schema struct
must trace back to a record. Adding a record with no schema entry fails here,
at build time.

This is a text-level check on purpose: MN5's login node is airgapped and has no
flatc, so nothing in the build may depend on running it. Python and the two
files are enough.

Usage: tools/schema_check.py [repo_root]
"""

import re
import sys
from pathlib import Path

# --- what a C++ record field type means on the wire -------------------------
# Anything not in here is rejected rather than guessed at: a field whose type we
# cannot map is exactly the case where a silent mismatch would hide.
SCALARS = {
    "Handle": "uint32",
    "DeviceId": "uint16",
    "DomainId": "uint16",
    "GpuId": "uint16",
    "Ticks": "uint64",
    "std::uint8_t": "uint8",
    "std::uint16_t": "uint16",
    "std::uint32_t": "uint32",
    "std::uint64_t": "uint64",
    "std::int8_t": "int8",
    "std::int16_t": "int16",
    "std::int32_t": "int32",
    "std::int64_t": "int64",
    "float": "float",
    "double": "double",
}

# C++ headers holding the shared vocabulary (structs and enums records refer to).
VOCAB_HEADERS = ["libpcomm/include/pcomm/core/vocab.hpp",
                 "libpcomm/include/pcomm/core/time.hpp"]

SKIP_DIRS = {"build", "build-x", "third_party", ".tools", ".git", ".cache", "pcomm-out"}

errors = []
notes = []


def fail(msg):
    errors.append(msg)


# --- parsing: C++ -----------------------------------------------------------

def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


FIELD_RE = re.compile(r"^\s*([A-Za-z_][\w:]*(?:\s*\*)?)\s+(\w+)\s*(\[\d+\])?\s*(\{[^}]*\})?\s*;\s*$")


def parse_fields(body):
    """Fields of a trivially-copyable aggregate, in declaration order.

    Explicit padding members are skipped: they exist so the C++ object
    representation has no uninitialised holes (see core/time.hpp), while flatc
    emits its own padding from the field alignments. Both sides end up with the
    same bytes — which is what the conformance test measures — but only one
    side spells the padding out.
    """
    fields = []
    for line in strip_comments(body).splitlines():
        line = line.strip()
        if not line or "(" in line or line.startswith("static") or line.startswith("using"):
            continue
        m = FIELD_RE.match(line)
        if not m:
            continue
        ctype, name = m.group(1), m.group(2)
        if name.startswith("_pad"):
            continue
        fields.append((name, ctype))
    return fields


RECORD_RE = re.compile(
    r"PCOMM_RECORD\(\s*(\w+)\s*,\s*(?:\w+::)?RecordKind::(\w+)\s*,\s*(\d+)\s*,"
    r'\s*"([^"]+)"\s*\)\s*\{(.*?)\n\};',
    re.S)


def parse_records(root):
    records = {}
    for path in sorted(root.rglob("*.hpp")):
        if set(path.relative_to(root).parts) & SKIP_DIRS:
            continue
        text = path.read_text()
        for m in RECORD_RE.finditer(text):
            name, kind, type_id, wire, body = m.groups()
            if name in records:
                fail(f"record {name} declared twice ({records[name]['path']} and {path})")
            records[name] = {
                "kind": kind, "type_id": int(type_id), "wire": wire,
                "fields": parse_fields(body),
                "path": path.relative_to(root),
            }
    return records


CPP_STRUCT_RE = re.compile(r"\bstruct\s+(\w+)\s*\{(.*?)\n\};", re.S)
CPP_ENUM_RE = re.compile(r"\benum\s+class\s+(\w+)\s*:\s*([\w:]+)\s*\{(.*?)\}\s*;", re.S)


def parse_vocab(root):
    structs, enums = {}, {}
    for rel in VOCAB_HEADERS:
        text = (root / rel).read_text()
        for m in CPP_ENUM_RE.finditer(text):
            name, _underlying, body = m.groups()
            members, next_value = [], 0
            for entry in strip_comments(body).split(","):
                entry = entry.strip()
                if not entry:
                    continue
                if "=" in entry:
                    key, value = entry.split("=", 1)
                    next_value = int(value.strip(), 0)
                    entry = key.strip()
                members.append((entry, next_value))
                next_value += 1
            enums[name] = members
        for m in CPP_STRUCT_RE.finditer(text):
            structs[m.group(1)] = parse_fields(m.group(2))
    return structs, enums


# --- parsing: the schema ----------------------------------------------------

FBS_ENUM_RE = re.compile(r"\benum\s+(\w+)\s*:\s*(\w+)\s*\{(.*?)\}", re.S)
FBS_STRUCT_RE = re.compile(r"\bstruct\s+(\w+)\s*\{(.*?)\}", re.S)
FBS_FIELD_RE = re.compile(r"(\w+)\s*:\s*(\w+)\s*;")
# The type_id and kind live in the comment above each struct — the only place
# the schema records them, so they are checked rather than trusted.
FBS_TYPE_RE = re.compile(r"//\s*type\s+(\d+)\s*,\s*(\w+)")


def parse_fbs(path):
    text = path.read_text()
    body = strip_comments(text)

    enums = {}
    for m in FBS_ENUM_RE.finditer(body):
        name, _underlying, members = m.groups()
        parsed, next_value = [], 0
        for entry in members.split(","):
            entry = entry.strip()
            if not entry:
                continue
            if "=" in entry:
                key, value = entry.split("=", 1)
                next_value = int(value.strip(), 0)
                entry = key.strip()
            parsed.append((entry, next_value))
            next_value += 1
        enums[name] = parsed

    structs = {}
    for m in FBS_STRUCT_RE.finditer(body):
        structs[m.group(1)] = FBS_FIELD_RE.findall(m.group(2))

    # type_id / kind annotations, matched against the *uncommented* text.
    annotations = {}
    pending = None
    for line in text.splitlines():
        hit = FBS_TYPE_RE.search(line)
        if hit:
            pending = (int(hit.group(1)), hit.group(2))
        s = re.match(r"\s*struct\s+(\w+)", line)
        if s:
            if pending:
                annotations[s.group(1)] = pending
            pending = None
    return structs, enums, annotations


# --- parsing: the conformance test -----------------------------------------

def parse_conformance(path):
    text = path.read_text()
    return {
        "sizes": set(re.findall(r"PCOMM_SCHEMA_MATCH\(\s*\w+::(\w+)", text)),
        "bytes": set(re.findall(r'expect_identical\(\s*"(\w+)"', text)),
        "enums": set(re.findall(r"PCOMM_SCHEMA_ENUM\(\s*[\w:]*::(\w+::\w+)", text)),
    }


# --- the checks -------------------------------------------------------------

def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    fbs_path = root / "libpcomm/schema/pcomm.fbs"
    conf_path = root / "libpcomm/tests/schema_conformance.cpp"

    records = parse_records(root)
    vocab_structs, vocab_enums = parse_vocab(root)
    fb_structs, fb_enums, fb_types = parse_fbs(fbs_path)
    conf = parse_conformance(conf_path)

    if not records:
        fail("no PCOMM_RECORD declarations found — the walk is broken, not the tree")

    def fbs_type_of(ctype, where):
        """The .fbs type a C++ field type must map to."""
        if ctype in SCALARS:
            return SCALARS[ctype]
        if ctype in vocab_enums or ctype in vocab_structs:
            return ctype          # by name: MemRange:MemRange, MemOp:MemOp
        fail(f"{where}: type '{ctype}' has no known schema mapping — add it to "
             f"SCALARS in tools/schema_check.py, or use a vocabulary type")
        return None

    def check_fields(cpp_fields, fb_name, prefix_temporal, where):
        expected = [("temporal", "Temporal")] if prefix_temporal else []
        for name, ctype in cpp_fields:
            expected.append((name, fbs_type_of(ctype, where)))
        actual = fb_structs[fb_name]
        if len(expected) != len(actual):
            fail(f"{where}: {len(cpp_fields)} C++ fields but pcomm.fbs struct "
                 f"{fb_name} has {len(actual)}"
                 + (" (+ the temporal stamp)" if prefix_temporal else ""))
            return
        for i, ((cn, ct), (fn, ft)) in enumerate(zip(expected, actual)):
            if cn != fn:
                fail(f"{where}: field {i} is '{cn}' in C++ but '{fn}' in "
                     f"pcomm.fbs struct {fb_name} — order and names must match")
            elif ct is not None and ct != ft:
                fail(f"{where}: field '{cn}' is {ct} in C++ but {ft} in "
                     f"pcomm.fbs struct {fb_name}")

    # 1. Every record is in the schema, with matching shape, id and kind.
    for name, rec in sorted(records.items()):
        where = f"{rec['path']}:{name}"
        if name not in fb_structs:
            fail(f"{where} (type {rec['type_id']}, \"{rec['wire']}\") has no struct "
                 f"in libpcomm/schema/pcomm.fbs — every record must be in the "
                 f"schema or no non-C++ reader can see it")
            continue
        # A .pcs file is an array of Stamped<R>, so the schema struct is the
        # temporal stamp followed by the payload.
        check_fields(rec["fields"], name, True, where)

        if name not in fb_types:
            fail(f"{where}: pcomm.fbs struct {name} has no '// type <id>, <Kind>' "
                 f"annotation — that comment is the only place the schema states "
                 f"which stream the struct describes")
        else:
            fb_id, fb_kind = fb_types[name]
            if fb_id != rec["type_id"]:
                fail(f"{where}: type_id {rec['type_id']} in C++ but {fb_id} in pcomm.fbs")
            if fb_kind != rec["kind"]:
                fail(f"{where}: RecordKind::{rec['kind']} in C++ but {fb_kind} in pcomm.fbs")

        if name not in conf["sizes"]:
            fail(f"{where}: no PCOMM_SCHEMA_MATCH line in schema_conformance.cpp — "
                 f"the size assertion is not automatic, add it")
        if name not in conf["bytes"]:
            fail(f"{where}: no expect_identical(\"{name}\", ...) case in "
                 f"schema_conformance.cpp — size alone would accept reordered fields")

    # 2. Nothing in the schema is orphaned. Vocabulary structs are shared rather
    #    than being records of their own, so they are checked against C++ too.
    for name in sorted(fb_structs):
        if name in records:
            continue
        if name in vocab_structs:
            check_fields(vocab_structs[name], name, False,
                         f"vocabulary struct {name}")
            continue
        fail(f"pcomm.fbs declares struct {name} with no PCOMM_RECORD and no C++ "
             f"vocabulary struct behind it — dead schema, or a record that was "
             f"deleted without removing its entry")

    # 3. Enums used by record fields exist in the schema with the same values,
    #    and every enumerator is asserted in the conformance test. Reordering an
    #    enum changes no size and no offset, so nothing else can catch it.
    used = {ctype for rec in records.values() for _n, ctype in rec["fields"]
            if ctype in vocab_enums}
    used |= {ctype for fields in vocab_structs.values() for _n, ctype in fields
             if ctype in vocab_enums}
    for name in sorted(used):
        if name not in fb_enums:
            fail(f"C++ enum {name} is used by a record field but is not declared "
                 f"in pcomm.fbs — declare it, do not describe it in a comment")
            continue
        if vocab_enums[name] != fb_enums[name]:
            fail(f"enum {name} differs:\n"
                 f"      C++ : {vocab_enums[name]}\n"
                 f"      .fbs: {fb_enums[name]}")
        for member, _value in vocab_enums[name]:
            if f"{name}::{member}" not in conf["enums"]:
                fail(f"enum {name}::{member} has no PCOMM_SCHEMA_ENUM line in "
                     f"schema_conformance.cpp")
    for name in sorted(set(fb_enums) - used):
        notes.append(f"schema enum {name} is declared but no record field uses it")

    # --- report
    print("schema check: C++ records vs libpcomm/schema/pcomm.fbs")
    for name in sorted(records):
        rec = records[name]
        mark = "ok " if not any(name in e for e in errors) else "XX "
        print(f"  [{mark}] {name:<12} type {rec['type_id']:<4} {rec['kind']:<9} "
              f"{len(rec['fields'])} fields  {rec['path']}")
    for name in sorted(used):
        print(f"  [ok ] enum {name} ({len(vocab_enums.get(name, []))} values)")
    for note in notes:
        print(f"  [note] {note}")
    for err in errors:
        print(f"  [XX ] {err}")
    if errors:
        print(f"FAIL — {len(errors)} problem(s)")
        return 1
    print(f"PASS — {len(records)} records, {len(used)} enums, "
          f"{len(fb_structs)} schema structs, all accounted for")
    return 0


if __name__ == "__main__":
    sys.exit(main())
