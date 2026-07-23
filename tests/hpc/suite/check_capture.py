#!/usr/bin/env python3
"""check_capture.py — assert a pcomm capture matches a case's expectations.

This is what turns a demo into a regression test: given a trace directory and an
`expect.json`, it checks that the providers actually captured what the workload
should have produced, and exits non-zero (with a readable diff) when they didn't.

The contract is deliberately provider-agnostic and format-agnostic: it asserts on
the *record streams* the tracer writes — `<rank-dir>/<type_id>.pcs` — by presence,
byte volume, and how many ranks carry them. That catches the regressions that
matter (a provider stopped capturing, cross-thread flush dropped proxy-thread
records, replay passes collided, a rank streamed nothing) without needing a
decoder for every record type. It understands the replay layout (streams nested
under out/<pass>/rank-*) the same way pcomm-read does.

    check_capture.py <trace-dir> <expect.json> [--json]

expect.json:
    {
      "case": "nccl_internode",
      "providers": ["ib"],
      "assert": {
        "min_rank_dirs": 8,
        "streams": [
          {"type": 102, "name": "ib_queue_pair", "min_ranks": 8},
          {"type": 110, "name": "ib_post_send",  "min_ranks": 8, "min_total_bytes": 100000}
        ]
      }
    }
"""
import json
import sys
from pathlib import Path


def find_rank_dirs(root: Path):
    """Every rank-* dir under root, at any depth (flat run or nested replay)."""
    out = []
    for p in sorted(root.rglob("rank-*")):
        if p.is_dir():
            out.append(p)
    return out


def collect_streams(rank_dirs):
    """type_id -> {"ranks": set(logical rank ints), "bytes": total, "files": n}."""
    streams = {}
    for rd in rank_dirs:
        try:
            rank = int(rd.name.split("-", 1)[1])
        except (IndexError, ValueError):
            rank = -1
        for pcs in rd.glob("*.pcs"):
            try:
                tid = int(pcs.stem)
            except ValueError:
                continue
            s = streams.setdefault(tid, {"ranks": set(), "bytes": 0, "files": 0})
            s["ranks"].add(rank)
            s["bytes"] += pcs.stat().st_size
            s["files"] += 1
    return streams


def check(trace_dir: Path, spec: dict):
    """Returns (results:list[(ok,msg)], ok:bool)."""
    results = []
    a = spec.get("assert", {})
    rank_dirs = find_rank_dirs(trace_dir)
    streams = collect_streams(rank_dirs)

    # logical rank coverage (a replayed rank 0 across two passes still counts once)
    logical_ranks = set()
    for rd in rank_dirs:
        try:
            logical_ranks.add(int(rd.name.split("-", 1)[1]))
        except (IndexError, ValueError):
            pass

    def record(ok, msg):
        results.append((ok, msg))

    min_dirs = a.get("min_rank_dirs")
    if min_dirs is not None:
        record(len(logical_ranks) >= min_dirs,
               f"rank coverage: {len(logical_ranks)} ranks (need >= {min_dirs})")

    for st in a.get("streams", []):
        tid = st["type"]
        label = f'{tid} ({st.get("name", "?")})'
        got = streams.get(tid)
        if got is None:
            record(False, f"stream {label}: MISSING (no {tid}.pcs anywhere)")
            continue
        ok = True
        detail = [f'{got["bytes"]}B', f'{len(got["ranks"])} ranks']
        if "min_ranks" in st and len(got["ranks"]) < st["min_ranks"]:
            ok = False
            detail.append(f'need >= {st["min_ranks"]} ranks')
        if "min_total_bytes" in st and got["bytes"] < st["min_total_bytes"]:
            ok = False
            detail.append(f'need >= {st["min_total_bytes"]}B')
        if "min_files" in st and got["files"] < st["min_files"]:
            ok = False
            detail.append(f'need >= {st["min_files"]} files (got {got["files"]})')
        record(ok, f"stream {label}: " + ", ".join(detail))

    # nothing captured at all is always a failure, even with an empty spec
    if not streams:
        record(False, "no record streams captured under " + str(trace_dir))

    overall = all(ok for ok, _ in results) and bool(results)
    return results, overall


def main():
    args = [x for x in sys.argv[1:] if not x.startswith("--")]
    as_json = "--json" in sys.argv[1:]
    if len(args) != 2:
        print(__doc__)
        return 2
    trace_dir, expect = Path(args[0]), Path(args[1])
    spec = json.loads(expect.read_text())
    case = spec.get("case", expect.stem)

    if not trace_dir.is_dir():
        msg = f"trace dir not found: {trace_dir} (did the job run?)"
        if as_json:
            print(json.dumps({"case": case, "ok": False, "error": msg}))
        else:
            print(f"[{case}] FAIL — {msg}")
        return 1

    results, ok = check(trace_dir, spec)
    summary = {"case": case, "ok": ok,
               "checks": [{"ok": o, "msg": m} for o, m in results]}
    # Drop a machine-readable verdict beside the capture so a suite runner can
    # aggregate async SLURM jobs after the fact.
    try:
        (trace_dir / "result.json").write_text(json.dumps(summary, indent=2))
    except OSError:
        pass
    if as_json:
        print(json.dumps(summary))
    else:
        print(f"=== {case} : {'PASS' if ok else 'FAIL'} ===")
        for o, m in results:
            print(f"  [{'ok ' if o else 'XX '}] {m}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
