#!/usr/bin/env python3
"""Read a pcomm capture from Python, using only pcomm.fbs and flatc.

The stated reason for adopting FlatBuffers is that a capture can be read from
any language flatc supports. Nothing in the tree demonstrated that: the schema
was checked against the C++ records, the C++ records were checked against
themselves, and "other languages can read this" stayed an inference. This is
the evidence — no libpcomm, no C++, no pcomm-read, nothing hand-written about
the layout. It opens a real `.pcs` from the suite with flatc-generated Python
bindings and recomputes three of case 01's four goldens.

A `.pcs` is a packed array of one struct type, so reading it is: take the file
size, divide by the struct's SizeOf(), and Init() at each offset. There is no
framing to parse — which is also the thing gap 5 is about, since the file itself
says nowhere which struct it holds. Here the type comes from the filename
(`<type_id>.pcs`), exactly the convention pcomm-read relies on.

The fourth golden (topology) is deliberately not reproduced: comm_edges is
derived by pcomm-read from QP state transitions, not a stream on disk.

    tools/read_pcs.py [capture_dir]

Defaults to the committed case 01 capture in tests/data/.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "third_party/flatbuffers/python"))
sys.path.insert(0, str(ROOT / "libpcomm/schema/generated-python"))

from pcomm.fb.IbMemRegion import IbMemRegion    # noqa: E402
from pcomm.fb.IbQueuePair import IbQueuePair    # noqa: E402
from pcomm.fb.IbPostSend import IbPostSend      # noqa: E402
from pcomm.fb.IbPollCq import IbPollCq          # noqa: E402

# type_id -> generated struct, mirroring the PCOMM_RECORD ids. tools/
# schema_check.py keeps the schema honest about these; here they only pick the
# file to open.
STREAMS = {100: IbMemRegion, 102: IbQueuePair, 110: IbPostSend, 112: IbPollCq}

# ibverbs enums, decoded the way pcomm-read decodes them for the CSV the golden
# queries run against (pcomm-read/main.cpp: wr_opcode_name / wc_* ).
WR_OPCODE = {0: "rdma_write", 1: "rdma_write_imm", 2: "send", 3: "send_imm",
             4: "rdma_read", 5: "cmp_swp", 6: "fetch_add"}
WC_OPCODE = {0: "send", 1: "rdma_write", 2: "rdma_read", 3: "cmp_swp",
             4: "fetch_add", 128: "recv", 129: "recv_rdma_imm"}


def read_stream(path, cls):
    """Every record in one .pcs file. This is the whole reader."""
    buf = bytearray(path.read_bytes())
    stride = cls.SizeOf()
    if len(buf) % stride:
        raise SystemExit(f"{path}: {len(buf)} B is not a multiple of the "
                         f"{stride} B {cls.__name__} stride")
    out = []
    for pos in range(0, len(buf), stride):
        obj = cls()
        obj.Init(buf, pos)
        out.append(obj)
    return out


def load(capture):
    """{rank: {type_id: [records]}} for a capture directory."""
    ranks = {}
    for rank_dir in sorted(capture.glob("rank-*")):
        rank = int(rank_dir.name.split("-")[1])
        ranks[rank] = {tid: read_stream(rank_dir / f"{tid}.pcs", cls)
                       for tid, cls in STREAMS.items()
                       if (rank_dir / f"{tid}.pcs").exists()}
    return ranks


def golden(case_dir, name):
    rows = (case_dir / "checks" / f"{name}.golden").read_text().strip().splitlines()
    return [tuple(r.split(",")) for r in rows[1:]]      # drop the header row


def main():
    capture = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        ROOT / "tests/data/ib_pingpong-43996904"
    case_dir = ROOT / "tests/hpc/cases/01_ib_pingpong"
    if not capture.exists():
        raise SystemExit(f"no capture at {capture}")

    ranks = load(capture)
    print(f"python reader: {capture.name}, {len(ranks)} ranks, "
          f"schema libpcomm/schema/pcomm.fbs via flatc")

    failures = 0

    def check(name, got):
        nonlocal failures
        want = golden(case_dir, name)
        ok = got == want
        failures += not ok
        print(f"  [{'ok ' if ok else 'XX '}] {name}: {got}")
        if not ok:
            print(f"         golden: {want}")

    # regions — one MR per rank (the pingpong buffer).
    check("regions", [(str(r), str(len(s.get(100, [])))) for r, s in sorted(ranks.items())])

    # traffic — 20 sends of 4096 B per side, by opcode.
    traffic = []
    for rank, streams in sorted(ranks.items()):
        by_op = {}
        for s in streams.get(110, []):
            n, b = by_op.get(WR_OPCODE.get(s.Opcode(), s.Opcode()), (0, 0))
            by_op[WR_OPCODE.get(s.Opcode(), s.Opcode())] = (n + 1, b + s.Length())
        for op in sorted(by_op):
            traffic.append((str(rank), op, str(by_op[op][0]), str(by_op[op][1])))
    check("traffic", traffic)

    # completions — every work request completed with status success.
    completions = []
    for rank, streams in sorted(ranks.items()):
        counts = {}
        for c in streams.get(112, []):
            key = ("success" if c.Status() == 0 else f"err{c.Status()}",
                   WC_OPCODE.get(c.Opcode(), c.Opcode()))
            counts[key] = counts.get(key, 0) + 1
        for (status, op) in sorted(counts):
            completions.append((str(rank), status, op, str(counts[(status, op)])))
    check("completions", completions)

    # Not a golden, but worth stating: the QP stream is what topology is built
    # from, and reading it here is what makes the schema claim end-to-end.
    qps = {r: len(s.get(102, [])) for r, s in sorted(ranks.items())}
    print(f"  [--- ] queue_pairs per rank: {qps} (topology is derived by "
          f"pcomm-read, not a stream)")

    if failures:
        print(f"FAIL — {failures} golden(s) not reproduced from Python")
        return 1
    print("PASS — 3 of case 01's 4 goldens reproduced from .pcs with no C++")
    return 0


if __name__ == "__main__":
    sys.exit(main())
