#!/usr/bin/env python3
"""parity.py — pcomm vs SigmaTrace on case 11 (ibdevx), as a coverage matrix.

Both captures are projected onto the same (class, key) vocabulary per rank:
SigmaTrace's by sigma_fingerprint.cpp, one rank-N.tsv per rank; pcomm's here,
from `pcomm export --csv` output. Keys carry only what survives a rerun (sizes,
flags, LIDs, port state, GUIDs), so two runs of one tool can be compared as
bags, and so can the two tools.

Usage:
  parity.py --pcomm A.csv [--pcomm B.csv] --sigma DIR [--sigma DIR2]

The first --pcomm and the first --sigma are the pair compared. Every run given
is also checked against the other run of the same tool, first: a cross-tool
difference means nothing until each tool agrees with itself.

The output is a matrix, not a verdict — one row per class, with what both saw,
what only one saw, and a plain reading. A single PASS/FAIL would discard exactly
the information this exists to produce (PLAN-20260810-0102, step 4).
"""
import argparse
import csv
import os
import re
from collections import Counter, defaultdict

PORT_STATE = {"nop": 0, "down": 1, "init": 2, "armed": 3, "active": 4, "active_defer": 5}

# Compared by count only: their keys are either not stable across runs or not
# expressed in a shared vocabulary by the two tools (SigmaTrace records one
# post_send per call with a WR count, pcomm one row per WR).
COUNT_ONLY = {"qp_modify", "post_send", "post_recv", "completion", "doorbell"}

# Classes one tool records and the other has no event for at all. Listed so the
# matrix says "no counterpart" instead of reading a vocabulary gap as a miss.
PCOMM_ONLY = {"cq_create", "cq_destroy", "srq_create", "srq_destroy", "devx_qp_create",
              "devx_obj_destroy", "devx_cmd", "mr_on_gpu", "object_create", "object_free",
              "close_unmatched"}
SIGMA_ONLY = {"device_enum"}


def read(d, table):
    p = os.path.join(d, table + ".csv")
    if not os.path.exists(p):
        return []
    with open(p, newline="") as f:
        return list(csv.DictReader(f))


def pcomm_bags(d):
    bags = defaultdict(Counter)
    closes = {}  # (rank, handle) -> (class, key) that closing it counts as

    def add(r, cls, key):
        bags[int(r["rank"])][(cls, key)] += 1

    def opened(r, handle_col, cls, key):
        # setdefault: a DEVX object appears in mlx5_devx_cmd once per command on
        # it, and the first one is the create that names what it is.
        closes.setdefault((r["rank"], r[handle_col]), (cls, key))

    for r in read(d, "ib_device"):
        add(r, "device_open", "guid=0x%016x" % int(r["node_guid"], 16))
        opened(r, "context", "device_close", "-")
    for r in read(d, "ib_port"):
        add(r, "port", "port=%s lid=%s state=%d" % (r["port_num"], r["lid"],
                                                    PORT_STATE.get(r["state"], -1)))
    for r in read(d, "ib_queue_pair"):
        add(r, "qp_create", "-")
        opened(r, "qp", "qp_destroy", "-")
    for r in read(d, "ib_modify_qp"):
        add(r, "qp_modify", "-")
        # SigmaTrace emits QpConnectionInfo from a verbs modify to RTR too.
        # ib_modify_qp has no port column, so that half of the key is unknown.
        if r["state"] == "rtr":
            add(r, "qp_connect", "lid=%s port=?" % r["dest_lid"])
    for r in read(d, "mlx5_devx_qp_connect"):
        add(r, "qp_connect", "lid=%s port=%s" % (r["dest_lid"], r["port_num"]))
    for r in read(d, "ib_mem_region"):
        add(r, "mr_reg", "size=%s" % r["size"])
        opened(r, "mr", "mr_dereg", "size=%s" % r["size"])
    for r in read(d, "mlx5_umem"):
        add(r, "umem_reg", "size=%s" % r["size"])
        opened(r, "umem", "umem_dereg", "size=%s" % r["size"])
    for r in read(d, "mlx5_uar"):
        add(r, "uar_alloc", "flags=0x%x" % int(r["flags"], 16))
        opened(r, "uar", "uar_free", "-")

    for r in read(d, "ib_post_send"):
        add(r, "post_send", "-")
    for r in read(d, "ib_post_recv"):
        add(r, "post_recv", "-")
    for r in read(d, "ib_poll_cq"):
        add(r, "completion", "-")
    for r in read(d, "mlx5_device_send"):
        add(r, "doorbell", "-")

    # pcomm-only vocabulary
    for r in read(d, "ib_completion_queue"):
        add(r, "cq_create", "-")
        opened(r, "cq", "cq_destroy", "-")
    for r in read(d, "ib_shared_recv_queue"):
        add(r, "srq_create", "-")
        opened(r, "srq", "srq_destroy", "-")
    for r in read(d, "mlx5_devx_cmd"):
        add(r, "devx_cmd", "opcode=%s" % r["opcode"])
        opened(r, "obj", "devx_obj_destroy", "opcode=%s" % r["opcode"])
    for r in read(d, "mlx5_devx_qp"):
        add(r, "devx_qp_create", "gpu_driven=%s" % r["gpu_driven"])
    for r in read(d, "mem_owner"):
        if r["source"] == "ib_mem_region" and r["device"] != "65535":
            add(r, "mr_on_gpu", "size=%s" % r["size"])
    # The engine's object registry (MemObject, 520). Its handle is the third
    # column, as in every entity table; read positionally so a renamed column
    # cannot silently turn its closes into close_unmatched again.
    for r in read(d, "mem_object"):
        add(r, "object_create", "-")
        opened(r, list(r)[2], "object_free", "-")

    for r in read(d, "entity_close"):
        cls, key = closes.get((r["rank"], r["subject"]),
                              ("close_unmatched", "opener=%s" % r["opener"]))
        add(r, cls, key)
    return bags


def sigma_bags(d):
    bags = defaultdict(Counter)
    for name in sorted(os.listdir(d)):
        m = re.fullmatch(r"rank-(\d+)\.tsv", name)
        if not m:
            continue
        with open(os.path.join(d, name)) as f:
            for line in f:
                cls, _, key = line.rstrip("\n").partition("\t")
                if cls:
                    bags[int(m.group(1))][(cls, key)] += 1
    return bags


def normalise(bags):
    out = defaultdict(Counter)
    for rank, bag in bags.items():
        for (cls, key), n in bag.items():
            out[rank][(cls, "-" if cls in COUNT_ONLY else key)] += n
    return out


def of_class(bag, cls):
    return Counter({k: n for (c, k), n in bag.items() if c == cls})


def classes(*bagsets):
    return sorted({c for bags in bagsets for bag in bags.values() for (c, _) in bag})


def compare(a, b, cls):
    """Summed over ranks: (count a, count b, both, a only, b only, per-rank detail)."""
    tot = [0, 0, 0, 0, 0]
    detail = []
    for rank in sorted(set(a) | set(b)):
        x, y = of_class(a.get(rank, Counter()), cls), of_class(b.get(rank, Counter()), cls)
        xo, yo = x - y, y - x
        tot = [t + v for t, v in zip(tot, (sum(x.values()), sum(y.values()),
                                           sum((x & y).values()), sum(xo.values()),
                                           sum(yo.values())))]
        if xo or yo:
            detail.append((rank, xo, yo))
    return tot, detail


def repeatability(name, runs):
    if len(runs) < 2:
        print("  %-10s only one run given — not checked" % name)
        return
    (pa, a), (pb, b) = runs[0], runs[1]
    diffs = [c for c in classes(a, b) if compare(a, b, c)[1]]
    if not diffs:
        print("  %-10s IDENTICAL across %s and %s" % (name, os.path.basename(pa), os.path.basename(pb)))
        return
    print("  %-10s DIFFERS in: %s" % (name, ", ".join(diffs)))
    for c in diffs:
        (na, nb, *_), _ = compare(a, b, c)
        print("             %-18s %d vs %d" % (c, na, nb))


def reading(cls, s, p, both, p_only, s_only):
    if s == 0 and p == 0:
        return "neither (empty in both)"
    if s_only == 0 and p_only == 0:
        return "agree"
    if s == 0:
        return "no SigmaTrace event for this" if cls in PCOMM_ONLY else "pcomm only"
    if p == 0:
        return "no pcomm record for this" if cls in SIGMA_ONLY else "SigmaTrace only"
    if s_only == 0:
        return "pcomm sees all of SigmaTrace's, and more"
    if p_only == 0:
        return "SigmaTrace sees all of pcomm's, and more"
    return "disagree"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pcomm", action="append", required=True)
    ap.add_argument("--sigma", action="append", required=True)
    args = ap.parse_args()

    pr = [(d, normalise(pcomm_bags(d))) for d in args.pcomm]
    sr = [(d, normalise(sigma_bags(d))) for d in args.sigma]

    print("== self-repeatability (a cross-tool difference means nothing without this)")
    repeatability("pcomm", pr)
    repeatability("SigmaTrace", sr)

    p, s = pr[0][1], sr[0][1]
    print("\n== coverage, %s vs %s (summed over ranks %s)"
          % (os.path.basename(args.sigma[0]), os.path.basename(args.pcomm[0]),
             ",".join(map(str, sorted(set(p) | set(s))))))
    hdr = ("class", "SigmaTrace", "pcomm", "both", "pcomm only", "Sigma only", "reading")
    print("%-18s %10s %6s %5s %10s %10s  %s" % hdr)
    details = []
    for cls in classes(p, s):
        (ns, np_, both, s_only, p_only), detail = compare(s, p, cls)
        print("%-18s %10d %6d %5d %10d %10d  %s"
              % (cls, ns, np_, both, p_only, s_only, reading(cls, ns, np_, both, p_only, s_only)))
        if detail and ns and np_:
            details.append((cls, detail))

    if details:
        print("\n== where both recorded a class and the keys differ")
        for cls, detail in details:
            for rank, s_only, p_only in detail:
                print("  %s rank %d" % (cls, rank))
                for k, n in sorted(s_only.items()):
                    print("      SigmaTrace only  %4dx  %s" % (n, k))
                for k, n in sorted(p_only.items()):
                    print("      pcomm only       %4dx  %s" % (n, k))


if __name__ == "__main__":
    main()
