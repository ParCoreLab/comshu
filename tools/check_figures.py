#!/usr/bin/env python3
"""Check that demos/figures draws every record, and draws it correctly.

tools/gen_figures.py hardcodes the record set twice — once as boxes in the
family diagrams, once as rows in the index table on demos/entity-map.html — and
reads neither the schema nor the C++ declarations. That is deliberate for the
LAYOUT, which is an argument about which relationships matter and cannot be
derived from a struct. It is not defensible for the record SET.

Without this check, adding a PCOMM_RECORD leaves the figure silently missing it,
the index silently missing it, and every other check in the tree green. That is
the exact failure schema_check.py exists to prevent, one layer out: absence is
invisible, and a check you have to remember to run is no check against a mistake
made by forgetting.

Three lists, checked against each other:

    the PCOMM_RECORD declarations   <- the source of truth
    gen_figures.DRAWN               <- what the diagrams drew
    gen_figures.FAMILIES            <- what the index table lists

Parsed with schema_check.py's own parser rather than a second one, for the
reason gen_raw_columns.py gives: it is already the thing that enforces the
schema against those declarations, so borrowing it removes a translation step
instead of adding one.
"""
import pathlib
import sys
import types

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import schema_check as sc      # noqa: E402

# gen_figures is EXECUTED FROM SOURCE rather than imported, and that is not
# fussiness. Python validates a cached .pyc on (source mtime, source size) at
# one-second granularity, so an edit that keeps the byte length and lands in the
# same second as the previous compile reuses the stale bytecode — observed while
# mutation-testing this very file, where flipping a kind tag "ENT" -> "ART" is
# exactly such an edit, and the check confidently reported the state it had
# already forgotten. A check that a stale cache can fool reports PASS on what it
# happens to remember, which is worse than no check.
def _load_gen_figures():
    path = pathlib.Path(__file__).resolve().parent / "gen_figures.py"
    mod = types.ModuleType("gen_figures")
    mod.__file__ = str(path)
    exec(compile(path.read_text(), str(path), "exec"), mod.__dict__)
    return mod


gf = _load_gen_figures()

KIND_TAG = {"Entity": "ENT", "Event": "EVT", "Artifact": "ART", "Edge": "EDG"}

problems = []


def bad(msg):
    problems.append(msg)


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    declared = {r["wire"]: r for r in sc.parse_records(root).values()}

    indexed = {}
    for _key, _title, _rng, recs in gf.FAMILIES:
        for name, tid, kind, _desc in recs:
            if name in indexed:
                bad(f"{name}: listed twice in the index table")
            indexed[name] = (tid, kind)

    drawn = gf.DRAWN

    for name in sorted(set(declared) - set(indexed)):
        r = declared[name]
        bad(f"{name} (type {r['type_id']}, {r['kind']}, {r['path']}) is declared "
            f"but missing from the index table")
    for name in sorted(set(indexed) - set(declared)):
        bad(f"{name} is in the index table but no PCOMM_RECORD declares it")
    for name in sorted(set(declared) - set(drawn)):
        bad(f"{name} is declared but no family figure draws it")
    for name in sorted(set(drawn) - set(declared)):
        bad(f"{name} is drawn in a figure but no PCOMM_RECORD declares it")

    for name in sorted(set(declared) & set(indexed)):
        want = declared[name]
        tid, kind = indexed[name]
        if tid != want["type_id"]:
            bad(f"{name}: index table says type {tid}, declaration says {want['type_id']}")
        if kind != want["kind"]:
            bad(f"{name}: index table says {kind}, declaration says {want['kind']}")

    for name in sorted(set(declared) & set(drawn)):
        want = declared[name]
        tid, tag = drawn[name]
        if tid != want["type_id"]:
            bad(f"{name}: figure box says type {tid}, declaration says {want['type_id']}")
        if tag != KIND_TAG[want["kind"]]:
            bad(f"{name}: figure box is tagged {tag}, declaration says "
                f"{want['kind']} ({KIND_TAG[want['kind']]})")

    if problems:
        print("FAIL — demos/figures does not match the record declarations:\n")
        for p in problems:
            print(f"  {p}")
        print("\nEdit tools/gen_figures.py (the FAMILIES table and the family "
              "figure it belongs in), then re-run it to regenerate.")
        return 1

    kinds = {}
    for r in declared.values():
        kinds[r["kind"]] = kinds.get(r["kind"], 0) + 1
    summary = ", ".join(f"{n} {k.lower()}" for k, n in sorted(kinds.items()))
    print(f"PASS — {len(declared)} records drawn and indexed, all accounted for ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
