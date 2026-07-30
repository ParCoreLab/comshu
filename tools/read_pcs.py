#!/usr/bin/env python3
"""Read a pcomm capture from Python, using only pcomm.fbs and flatc.

The stated reason for adopting FlatBuffers is that a capture can be read from
any language flatc supports. Nothing in the tree demonstrated that: the schema
was checked against the C++ records, the C++ records were checked against
themselves, and "other languages can read this" stayed an inference. This is
the evidence — no libpcomm, no C++, no pcomm-read, nothing hand-written about
the layout. It opens a real `.pcs` from the suite with flatc-generated Python
bindings and recomputes three of case 01's four goldens.

A `.pcs` is a 32-byte StreamHeader followed by a packed array of one struct
type, so reading it is: parse the header, take its stride, and Init() at each
offset after it. The type_id and the stride come from the *file*, not from the
filename and not from anything this script knows — which is the point of the
header, and the reason this reader can check that what it opened is what it
thinks it opened instead of assuming.

The fourth golden (topology) is deliberately not reproduced: comm_edges is
derived by pcomm-read from QP state transitions, not a stream on disk.

    tools/read_pcs.py [capture_dir]

Defaults to the committed case 01 capture in tests/data/.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "third_party/flatbuffers/python"))
sys.path.insert(0, str(ROOT / "libpcomm/schema/generated-python"))

from pcomm.fb.StreamHeader import StreamHeader  # noqa: E402
from pcomm.fb.IbMemRegion import IbMemRegion    # noqa: E402
from pcomm.fb.IbQueuePair import IbQueuePair    # noqa: E402
from pcomm.fb.IbPostSend import IbPostSend      # noqa: E402
from pcomm.fb.IbPollCq import IbPollCq          # noqa: E402

STREAM_MAGIC = 0x0A534D4D4F435089
STREAM_FORMAT_VERSION = 1

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


def schema_hash(fbs_path):
    """FNV-1a over the schema, comments stripped and whitespace collapsed.

    Same normalisation as tools/schema_check.py — recomputed here rather than
    hardcoded, so this reader can tell whether the capture it opened was written
    by the schema sitting in the tree, from Python, with nothing shared but the
    .fbs file itself.
    """
    text = re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", fbs_path.read_text(), flags=re.S))
    h = 0xCBF29CE484222325
    for byte in " ".join(text.split()).encode():
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def read_stream(path, cls, expected_type):
    """Every record in one .pcs file. This is the whole reader.

    The header is validated, not skipped. Its stride is what the records are
    read at — so if this build's idea of the layout ever diverges from the
    file's, the file wins and says so, instead of being sliced at the wrong
    offset into records that look plausible.
    """
    buf = bytearray(path.read_bytes())
    header_size = StreamHeader.SizeOf()
    if len(buf) < header_size:
        raise SystemExit(f"{path}: too short to hold a stream header")

    h = StreamHeader()
    h.Init(buf, 0)
    if h.Magic() != STREAM_MAGIC:
        raise SystemExit(f"{path}: not a pcomm stream (magic {h.Magic():#018x}) — "
                         f"a pre-format-1 capture has no header and must be re-captured")
    if h.FormatVersion() != STREAM_FORMAT_VERSION:
        raise SystemExit(f"{path}: .pcs format version {h.FormatVersion()}, "
                         f"this reader speaks {STREAM_FORMAT_VERSION}")
    if h.TypeId() != expected_type:
        raise SystemExit(f"{path}: holds type {h.TypeId()}, filename says {expected_type}")
    if h.Stride() != cls.SizeOf():
        raise SystemExit(f"{path}: stride {h.Stride()} but the schema's "
                         f"{cls.__name__} is {cls.SizeOf()} B — capture and schema "
                         f"describe different layouts")

    payload = len(buf) - header_size
    if payload % h.Stride():
        raise SystemExit(f"{path}: {payload} B of records is not a multiple of the "
                         f"{h.Stride()} B stride — truncated capture")
    out = []
    for pos in range(header_size, len(buf), h.Stride()):
        obj = cls()
        obj.Init(buf, pos)
        out.append(obj)
    return out


def load(capture):
    """{rank: {type_id: [records]}} for a capture directory."""
    ranks = {}
    for rank_dir in sorted(capture.glob("rank-*")):
        rank = int(rank_dir.name.split("-")[1])
        ranks[rank] = {tid: read_stream(rank_dir / f"{tid}.pcs", cls, tid)
                       for tid, cls in STREAMS.items()
                       if (rank_dir / f"{tid}.pcs").exists()}
    return ranks


def golden(case_dir, name):
    rows = (case_dir / "checks" / f"{name}.golden").read_text().strip().splitlines()
    return [tuple(r.split(",")) for r in rows[1:]]      # drop the header row


def main():
    capture = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        ROOT / "tests/data/ib_pingpong-43997525"
    case_dir = ROOT / "tests/hpc/cases/01_ib_pingpong"
    if not capture.exists():
        raise SystemExit(f"no capture at {capture}")

    ranks = load(capture)
    print(f"python reader: {capture.name}, {len(ranks)} ranks, "
          f"schema libpcomm/schema/pcomm.fbs via flatc")

    # What the capture says wrote it, checked against the schema in the tree.
    # Not fatal: an unrelated record changing moves the hash without changing
    # these streams. Saying nothing, though, is what the header exists to end.
    want = schema_hash(ROOT / "libpcomm/schema/pcomm.fbs")
    seen = set()
    for rank_dir in sorted(capture.glob("rank-*")):
        for f in sorted(rank_dir.glob("*.pcs")):
            h = StreamHeader()
            h.Init(bytearray(f.read_bytes()[:StreamHeader.SizeOf()]), 0)
            seen.add(h.SchemaHash())
    for got in sorted(seen):
        print(f"  [{'ok ' if got == want else '!! '}] schema hash {got:#018x}"
              + ("" if got == want else f" (tree has {want:#018x})"))

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
