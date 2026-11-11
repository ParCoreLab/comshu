#!/usr/bin/env python3
"""Cross-check every C++ record declaration against libcomshu/schema/comshu.fbs.

tests/schema_conformance.cpp proves the ten records we *named* in it are
byte-identical to their schema structs. It cannot prove anything about a record
nobody named: add an eleventh COMSHU_RECORD, write no schema entry, add no test
line, and every check in the tree still passes while every non-C++ reader
silently misses the new stream. Absence is invisible — the same failure shape as
the case 07 label mismatch.

So this walks the *sources* rather than a list: every COMSHU_RECORD in the repo
must have a struct in comshu.fbs whose fields match one-for-one in name, order
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

import ast
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
    "RecordTypeId": "uint16",
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

# C++ headers holding the shared vocabulary (structs and enums records refer to)
# and the file framing. StreamHeader is checked exactly like a record: it is on
# disk in every capture, so it is part of the format whether or not it is a
# COMSHU_RECORD.
VOCAB_HEADERS = ["libcomshu/include/comshu/core/vocab.hpp",
                 "libcomshu/include/comshu/core/time.hpp",
                 "libcomshu/include/comshu/core/ids.hpp",
                 "libcomshu/include/comshu/rec/stream_header.hpp"]

# Where the schema hash constant lives, and what it hashes.
STREAM_HEADER_HPP = "libcomshu/include/comshu/rec/stream_header.hpp"

# This walk is over the *filesystem*, not `git ls-files`, and that is deliberate
# (see parse_records) — so it also descends into anything else living under the
# root. `.claude` holds agent worktrees, which are complete checkouts of this
# same repo: without skipping it, every record is "declared twice" and the check
# fails with 24 problems that are all one directory.
SKIP_DIRS = {"build", "build-all", "build-x", "third_party", ".tools", ".git",
             ".cache", "comshu-out", ".claude"}
# Records declared here are fixtures, not format. See parse_records.
TEST_DIRS = {"tests"}

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


# Matches COMSHU_RECORD(...) and COMSHU_RECORD_V(..., Version). The version group
# is absent for the plain form, which means 1 — see COMSHU_RECORD_V in
# core/record.hpp for what a version is and how ids relate to it.
RECORD_RE = re.compile(
    r"COMSHU_RECORD(_V)?\(\s*(\w+)\s*,\s*(?:\w+::)?RecordKind::(\w+)\s*,\s*(\d+)\s*,"
    r'\s*"([^"]+)"\s*(?:,\s*(\d+)\s*)?\)\s*\{(.*?)\s*\};',
    re.S)


def parse_records(root):
    """Every COMSHU_RECORD in the tree, wherever it is declared.

    .cpp as well as .hpp: a record declared in a translation unit is just as
    much on disk as one in a header, and scanning only headers would leave a
    way to add a record type that no check ever sees.

    Test directories are exempt, and that exemption is the point rather than an
    oversight — libcomshu/tests/record_versioning.cpp declares a demo family to
    prove the versioning mechanism, and inventing a real record (and a real
    type_id, and a schema entry) purely to have something to bump would put a
    permanent fiction in the on-disk format. Test records live at ids 900+,
    outside every range allocated in core/ids.hpp.
    """
    records = {}
    paths = sorted(list(root.rglob("*.hpp")) + list(root.rglob("*.cpp")))
    for path in paths:
        parts = set(path.relative_to(root).parts)
        if parts & SKIP_DIRS or parts & TEST_DIRS:
            continue
        text = path.read_text()
        for m in RECORD_RE.finditer(text):
            versioned, name, kind, type_id, wire, version, body = m.groups()
            if versioned and version is None:
                fail(f"{path}:{name}: COMSHU_RECORD_V without a version argument")
            if name in records:
                fail(f"record {name} declared twice ({records[name]['path']} and {path})")
            records[name] = {
                "kind": kind, "type_id": int(type_id), "wire": wire,
                "version": int(version) if version else 1,
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
FBS_TYPE_RE = re.compile(r"//\s*type\s+(\d+)\s*,\s*(\w+)(?:\s*,\s*v(\d+))?")


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
            pending = (int(hit.group(1)), hit.group(2),
                       int(hit.group(3)) if hit.group(3) else 1)
        s = re.match(r"\s*struct\s+(\w+)", line)
        if s:
            if pending:
                annotations[s.group(1)] = pending
            pending = None
    return structs, enums, annotations


# --- the schema hash --------------------------------------------------------

def schema_hash(fbs_path):
    """FNV-1a over the schema, comments stripped and whitespace collapsed.

    Every `.pcs` records this, so a capture states which schema wrote it — the
    thing that was missing when a file written before a schema change was
    byte-indistinguishable from one written after. Normalised so that editing
    prose does not churn the identity of the format, while any change to a
    declaration does.

    tools/read_pcs.py recomputes this the same way from Python; keep the two in
    step if the normalisation ever changes.
    """
    text = strip_comments(fbs_path.read_text())
    normalised = " ".join(text.split())
    h = 0xCBF29CE484222325
    for byte in normalised.encode():
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# --- parsing: the conformance test -----------------------------------------

def parse_conformance(path):
    text = path.read_text()
    return {
        "sizes": set(re.findall(r"COMSHU_SCHEMA_MATCH\(\s*\w+::(\w+)", text)),
        "bytes": set(re.findall(r'expect_identical\(\s*"(\w+)"', text)),
        "enums": set(re.findall(r"COMSHU_SCHEMA_ENUM\(\s*[\w:]*::(\w+::\w+)", text)),
    }


# --- the checks -------------------------------------------------------------

def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    fbs_path = root / "libcomshu/schema/comshu.fbs"
    conf_path = root / "libcomshu/tests/schema_conformance.cpp"

    records = parse_records(root)
    vocab_structs, vocab_enums = parse_vocab(root)
    fb_structs, fb_enums, fb_types = parse_fbs(fbs_path)
    conf = parse_conformance(conf_path)

    if not records:
        fail("no COMSHU_RECORD declarations found — the walk is broken, not the tree")

    def unqualify(ctype):
        """`core::RecordKind` and `RecordKind` are the same type to the schema."""
        for prefix in ("comshu::core::", "core::", "comshu::rec::", "rec::"):
            if ctype.startswith(prefix):
                return ctype[len(prefix):]
        return ctype

    def fbs_type_of(ctype, where):
        """The .fbs type a C++ field type must map to."""
        ctype = unqualify(ctype)
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
            fail(f"{where}: {len(cpp_fields)} C++ fields but comshu.fbs struct "
                 f"{fb_name} has {len(actual)}"
                 + (" (+ the temporal stamp)" if prefix_temporal else ""))
            return
        for i, ((cn, ct), (fn, ft)) in enumerate(zip(expected, actual)):
            if cn != fn:
                fail(f"{where}: field {i} is '{cn}' in C++ but '{fn}' in "
                     f"comshu.fbs struct {fb_name} — order and names must match")
            elif ct is not None and ct != ft:
                fail(f"{where}: field '{cn}' is {ct} in C++ but {ft} in "
                     f"comshu.fbs struct {fb_name}")

    # 1. Every record is in the schema, with matching shape, id and kind.
    for name, rec in sorted(records.items()):
        where = f"{rec['path']}:{name}"
        if name not in fb_structs:
            fail(f"{where} (type {rec['type_id']}, \"{rec['wire']}\") has no struct "
                 f"in libcomshu/schema/comshu.fbs — every record must be in the "
                 f"schema or no non-C++ reader can see it")
            continue
        # A .pcs file is an array of Stamped<R>, so the schema struct is the
        # temporal stamp followed by the payload.
        check_fields(rec["fields"], name, True, where)

        if name not in fb_types:
            fail(f"{where}: comshu.fbs struct {name} has no '// type <id>, <Kind>' "
                 f"annotation — that comment is the only place the schema states "
                 f"which stream the struct describes")
        else:
            fb_id, fb_kind, fb_version = fb_types[name]
            if fb_id != rec["type_id"]:
                fail(f"{where}: type_id {rec['type_id']} in C++ but {fb_id} in comshu.fbs")
            if fb_kind != rec["kind"]:
                fail(f"{where}: RecordKind::{rec['kind']} in C++ but {fb_kind} in comshu.fbs")
            if fb_version != rec["version"]:
                fail(f"{where}: version {rec['version']} in C++ but v{fb_version} in "
                     f"comshu.fbs — annotate the struct '// type {rec['type_id']}, "
                     f"{rec['kind']}, v{rec['version']}'")

        if name not in conf["sizes"]:
            fail(f"{where}: no COMSHU_SCHEMA_MATCH line in schema_conformance.cpp — "
                 f"the size assertion is not automatic, add it")
        if name not in conf["bytes"]:
            fail(f"{where}: no expect_identical(\"{name}\", ...) case in "
                 f"schema_conformance.cpp — size alone would accept reordered fields")

    checked_vocab = []
    # 2. Nothing in the schema is orphaned. Vocabulary structs are shared rather
    #    than being records of their own, so they are checked against C++ too.
    for name in sorted(fb_structs):
        if name in records:
            continue
        if name in vocab_structs:
            check_fields(vocab_structs[name], name, False,
                         f"vocabulary struct {name}")
            checked_vocab.append(name)
            continue
        fail(f"comshu.fbs declares struct {name} with no COMSHU_RECORD and no C++ "
             f"vocabulary struct behind it — dead schema, or a record that was "
             f"deleted without removing its entry")

    # 3. Enums used by record fields exist in the schema with the same values,
    #    and every enumerator is asserted in the conformance test. Reordering an
    #    enum changes no size and no offset, so nothing else can catch it.
    used = {unqualify(ctype) for rec in records.values() for _n, ctype in rec["fields"]}
    used |= {unqualify(ctype) for fields in vocab_structs.values()
             for _n, ctype in fields}
    used &= set(vocab_enums)
    for name in sorted(used):
        if name not in fb_enums:
            fail(f"C++ enum {name} is used by a record field but is not declared "
                 f"in comshu.fbs — declare it, do not describe it in a comment")
            continue
        if vocab_enums[name] != fb_enums[name]:
            fail(f"enum {name} differs:\n"
                 f"      C++ : {vocab_enums[name]}\n"
                 f"      .fbs: {fb_enums[name]}")
        for member, _value in vocab_enums[name]:
            if f"{name}::{member}" not in conf["enums"]:
                fail(f"enum {name}::{member} has no COMSHU_SCHEMA_ENUM line in "
                     f"schema_conformance.cpp")
    for name in sorted(set(fb_enums) - used):
        notes.append(f"schema enum {name} is declared but no record field uses it")

    # 4. The versioning convention (COMSHU_RECORD_V in core/record.hpp). None of
    #    this is enforceable by the compiler: two records can happily claim the
    #    same type_id in different headers, and the writer would then produce two
    #    different layouts in one filename. These are the rules that make "a new
    #    type_id per version" a mechanism rather than an intention.
    by_id = {}
    for name, rec in sorted(records.items()):
        by_id.setdefault(rec["type_id"], []).append(name)
    for type_id, names in sorted(by_id.items()):
        if len(names) > 1:
            fail(f"type_id {type_id} is claimed by {' and '.join(names)} — an id is "
                 f"the on-disk discriminator and must never mean two things")

    families = {}
    for name, rec in sorted(records.items()):
        families.setdefault(rec["wire"], []).append((rec["version"], name, rec))
    for wire, members in sorted(families.items()):
        members.sort()
        versions = [v for v, _n, _r in members]
        if len(set(versions)) != len(versions):
            dupes = sorted({v for v in versions if versions.count(v) > 1})
            fail(f'family "{wire}": version {dupes} declared more than once — '
                 f'(name, version) identifies a record layout')
        elif versions != list(range(1, len(versions) + 1)):
            fail(f'family "{wire}": versions {versions} must start at 1 and be '
                 f'contiguous — a gap means a version exists in captures that no '
                 f'code can name, or one was deleted, and old versions are never '
                 f'deleted because captures still hold them')
        kinds = {r["kind"] for _v, _n, r in members}
        if len(kinds) > 1:
            fail(f'family "{wire}": versions disagree on RecordKind ({", ".join(sorted(kinds))}) '
                 f'— a record does not change kind across versions')
        if len(members) > 1:
            notes.append(f'family "{wire}": v' +
                         ", v".join(f"{v} (type {r['type_id']}, {n})"
                                    for v, n, r in members))

    # 5. The schema hash every capture is stamped with actually identifies this
    #    schema. Left to a human to update, it would be wrong the first time
    #    someone edited comshu.fbs in a hurry — and a stale hash is worse than
    #    none, since it asserts sameness that is not true.
    want_hash = schema_hash(fbs_path)
    header_src = (root / STREAM_HEADER_HPP).read_text()
    m = re.search(r"kSchemaHash\s*=\s*0x([0-9A-Fa-f]+)ull", header_src)
    if not m:
        fail(f"{STREAM_HEADER_HPP}: no kSchemaHash constant found")
    elif int(m.group(1), 16) != want_hash:
        fail(f"{STREAM_HEADER_HPP}: kSchemaHash is 0x{int(m.group(1), 16):016X} but "
             f"comshu.fbs hashes to 0x{want_hash:016X} — the schema changed; set it to "
             f"0x{want_hash:016X}ull")
    else:
        notes.append(f"schema hash 0x{want_hash:016X} matches "
                     f"comshu/rec/stream_header.hpp")

    # 5b. The derived layer PUBLISHES the vocabularies, and this holds that
    #     publication equal to the schema.
    #
    #     `comshu export --csv` decodes enums; `--parquet` does not; and the
    #     derived layer is Parquet-only. So the export carrying comm_edges and
    #     link_health is exactly the one that shows `3` where the other shows
    #     `Prefetch`. link_sql.hpp's `vocabulary` table closes that — one row
    #     per (vocab, value, name), joinable from any query — and being a
    #     hand-written table inside a C++ string it is precisely the kind of
    #     list this repo has watched go stale five times.
    #
    #     So it is parsed back out and compared. A value renamed in comshu.fbs
    #     now fails the build rather than publishing a wrong name to every
    #     reader of every capture. Same bargain as tools/check_figures.py.
    link_sql = (root / "comshu-read/link_sql.hpp").read_text()
    m = re.search(r'\{"vocabulary",\s*"vocab,value,name",\s*R"sql\((.*?)\)sql"',
                  link_sql, re.S)
    if not m:
        fail("comshu-read/link_sql.hpp: no `vocabulary` derived table found — it "
             "moved or was renamed; update tools/schema_check.py")
    else:
        published = {}
        for vocab, value, name in re.findall(
                r"\('(\w+)',\s*(\d+),\s*'(\w+)'\)", m.group(1)):
            published.setdefault(vocab, []).append((name, int(value)))
        # Every vocabulary the table claims must exist, in C++ or in the schema,
        # and must match member-for-member and value-for-value.
        for vocab, rows in sorted(published.items()):
            want = vocab_enums.get(vocab) or fb_enums.get(vocab)
            if want is None:
                fail(f"link_sql.hpp vocabulary publishes {vocab!r}, which is "
                     f"neither a C++ enum class nor a comshu.fbs enum")
            elif rows != want:
                fail(f"link_sql.hpp vocabulary disagrees with the schema for "
                     f"{vocab}:\n      published: {rows}\n      declared : {want}")
        # ...and every enum a RECORD FIELD uses must be published, or a Parquet
        # reader meets a number this layer cannot name. That is the direction
        # that catches an enum ADDED later, which is the one nobody remembers.
        for name in sorted(used):
            if name not in published:
                fail(f"enum {name} is used by a record field but link_sql.hpp's "
                     f"`vocabulary` table does not publish it — a Parquet reader "
                     f"has no way to name its values")
        if not any(name in e for e in errors for name in ("vocabulary", "vocab")):
            notes.append(f"vocabulary: {sum(len(v) for v in published.values())} "
                         f"names across {len(published)} enums match the schema")

    # 6. The two readers name ibverbs enum values from two hand-written tables:
    #    the switch statements in comshu-read/main.cpp and the dicts in
    #    tools/read_pcs.py. The Python reader's entire claim is that it
    #    reproduces goldens computed from the C++ CSVs, so the tables must be
    #    equal — and they drifted once ("cmp_swp" where the CSV says
    #    "atomic_cmp_swp"), invisibly, because case 01 never posts an atomic.
    #    Held equal here rather than generated from comshu.fbs on purpose: the
    #    schema hash seals every capture, and a naming table is not worth a
    #    capture-format event.
    cpp_reader = (root / "comshu-read/main.cpp").read_text()
    py_reader_path = root / "tools/read_pcs.py"

    def cpp_name_table(fn_name):
        m = re.search(rf'std::string\s+{fn_name}\s*\(unsigned\s+\w+\)\s*\{{(.*?)\n\}}',
                      cpp_reader, re.S)
        if not m:
            fail(f"comshu-read/main.cpp: cannot find {fn_name}() — the reader "
                 f"moved; update tools/schema_check.py")
            return None
        return {int(n): s for n, s in
                re.findall(r'case\s+(\d+)\s*:\s*return\s+"([^"]+)"', m.group(1))}

    def py_name_table(var_name):
        for node in ast.walk(ast.parse(py_reader_path.read_text())):
            if (isinstance(node, ast.Assign) and len(node.targets) == 1
                    and isinstance(node.targets[0], ast.Name)
                    and node.targets[0].id == var_name):
                return ast.literal_eval(node.value)
        fail(f"tools/read_pcs.py: no {var_name} dict found — the reader moved; "
             f"update tools/schema_check.py")
        return None

    for cpp_fn, py_var in [("wr_opcode_name", "WR_OPCODE"),
                           ("wc_opcode_name", "WC_OPCODE")]:
        cpp_tab, py_tab = cpp_name_table(cpp_fn), py_name_table(py_var)
        if cpp_tab is None or py_tab is None:
            continue
        if cpp_tab != py_tab:
            for k in sorted(set(cpp_tab) | set(py_tab)):
                if cpp_tab.get(k) != py_tab.get(k):
                    fail(f"ibverbs name tables disagree on {py_var}[{k}]: "
                         f"the reader says {cpp_tab.get(k)!r}, read_pcs.py says "
                         f"{py_tab.get(k)!r} — the Python reader must name "
                         f"values exactly as the CSVs the goldens query")
        else:
            notes.append(f"{py_var}: {len(py_tab)} names match the reader's {cpp_fn}()")

    # --- report
    print("schema check: C++ records vs libcomshu/schema/comshu.fbs")
    for name in sorted(records):
        rec = records[name]
        mark = "ok " if not any(name in e for e in errors) else "XX "
        print(f"  [{mark}] {name:<12} type {rec['type_id']:<4} {rec['kind']:<9} "
              f"{len(rec['fields'])} fields  {rec['path']}")
    for name in sorted(checked_vocab):
        print(f"  [ok ] struct {name:<12} {len(vocab_structs[name])} fields  "
              f"(shared vocabulary / file framing)")
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
