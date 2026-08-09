# Canonical, timing-free fingerprint of a DEVX capture (see
# plans/PLAN-20260810-0102-sigmatrace-parity). Everything run-specific is
# projected away: no timestamps, no addresses, no qp_num, no handles. What
# survives is the per-QP doorbell sequence, compared as a BAG of sequences so
# the result is invariant to how queue pairs were named or ordered.
import csv, collections, hashlib, sys, os
d = sys.argv[1]
def rows(n):
    p = os.path.join(d, n + ".csv")
    return list(csv.DictReader(open(p))) if os.path.exists(p) else []

db = collections.defaultdict(list)
for r in rows("mlx5_doorbell"):
    db[(r["rank"], r["qp_num"])].append(int(r["prod_idx"]))
ds = collections.defaultdict(list)
for r in rows("mlx5_device_send"):
    ds[(r["rank"], r["qp_num"])].append((int(r["wqe_idx"]), r["opcode_name"], int(r["length"])))

# One canonical string per queue pair, then the sorted bag of them.
seqs = sorted("|".join(str(i) for i in sorted(v)) for v in db.values())
wqes = sorted("|".join(f"{i}:{o}:{l}" for i, o, l in sorted(v)) for v in ds.values())
umem = sorted((r["range_size"] if "range_size" in r else r.get("size", "?"), r.get("access", "?")) for r in rows("mlx5_umem"))

print("qps           ", len(db))
print("doorbells     ", sum(len(v) for v in db.values()))
print("device_sends  ", sum(len(v) for v in ds.values()))
print("opcode_hist   ", dict(sorted(collections.Counter(o for v in ds.values() for _, o, _ in v).items())))
print("total_bytes   ", sum(l for v in ds.values() for _, _, l in v))
print("umem_bag      ", hashlib.sha256(str(umem).encode()).hexdigest()[:16])
print("doorbell_bag  ", hashlib.sha256(str(seqs).encode()).hexdigest()[:16])
print("wqe_bag       ", hashlib.sha256(str(wqes).encode()).hexdigest()[:16])
