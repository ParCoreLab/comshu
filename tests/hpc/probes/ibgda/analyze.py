#!/usr/bin/env python3
"""Answer Stage 2's question from a probe log + a pcomm capture.

    analyze.py <probe.txt> <capture.csv-dir>

Three questions, in the order that makes a NO conclusive:

  1. Did IBGDA engage at all?  Without this every other answer is vacuous — a
     silent fallback to the IBRC proxy transport produces "no doorbell traffic"
     that looks exactly like "NVBit cannot see doorbell traffic". This is the
     same trap case 08 sat in for weeks: an empty table matching an empty
     expectation.
  2. Which NIC-registered buffers live in GPU memory?  Those are the candidate
     send queues.
  3. Do any instrumented accesses land in them?
"""

import collections
import csv
import os
import sys

CU_MEMORYTYPE_DEVICE = 2
MLX5_CMD_OP_CREATE_QP = 0x500


def read_probe(path):
    umems, uars, devx = [], [], collections.Counter()
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "umem" and len(parts) >= 6:
                addr, size, mem_type, ordinal, umem_id = parts[1:6]
                umems.append((int(addr, 16), int(size), int(mem_type), int(ordinal), umem_id))
            elif parts[0] == "uar" and len(parts) >= 3:
                uars.append((int(parts[1], 16), parts[2]))
            elif parts[0] == "devx" and len(parts) >= 2:
                devx[int(parts[1], 16)] += 1
    return umems, uars, devx


def main():
    probe_path, csv_dir = sys.argv[1], sys.argv[2]
    umems, uars, devx = read_probe(probe_path)

    print("--- 1. did IBGDA engage? ---")
    n_qp = devx.get(MLX5_CMD_OP_CREATE_QP, 0)
    print(f"  mlx5dv_devx_obj_create calls : {sum(devx.values())}")
    print(f"    of which CREATE_QP (0x500) : {n_qp}")
    print(f"  mlx5dv_devx_alloc_uar calls  : {len(uars)}")
    for op, n in sorted(devx.items()):
        print(f"    opcode 0x{op:04x} : {n}")
    if n_qp == 0:
        print("  VERDICT: IBGDA did NOT engage — no DEVX queue pairs were created.")
        print("  Everything below is vacuous. Check NVSHMEM_DEBUG=INFO output for a")
        print("  fallback message before drawing ANY conclusion about option B.")
        return 2

    print("\n--- 2. NIC-registered buffers in GPU memory ---")
    gpu_umems = [u for u in umems if u[2] == CU_MEMORYTYPE_DEVICE]
    print(f"  umem registrations total     : {len(umems)}")
    print(f"    resident in GPU memory     : {len(gpu_umems)}")
    for addr, size, _, ordinal, umem_id in sorted(gpu_umems)[:20]:
        print(f"      0x{addr:012x} + {size:<10} dev={ordinal} umem_id={umem_id}")
    if not gpu_umems:
        print("  VERDICT: no NIC buffer is in GPU memory. Either the NIC handler fell")
        print("  back to the CPU, or NVSHMEM placed the queues in host memory")
        print("  (NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE). Option B cannot be judged")
        print("  from this run.")
        return 2

    print("\n--- 3. do instrumented accesses land in them? ---")
    access_csv = os.path.join(csv_dir, "gpu_access.csv")
    if not os.path.exists(access_csv):
        print(f"  no {access_csv} — capture produced no GPU accesses at all")
        return 2

    ranges = [(a, a + s, umem_id, ordinal) for a, s, _, ordinal, umem_id in gpu_umems]
    hits = collections.Counter()
    total = 0
    with open(access_csv) as f:
        for row in csv.DictReader(f):
            total += 1
            try:
                addr = int(row["addr"], 16)
            except (KeyError, ValueError):
                continue
            for lo, hi, umem_id, ordinal in ranges:
                if lo <= addr < hi:
                    hits[(umem_id, ordinal, row.get("op", "?"))] += 1
                    break

    print(f"  gpu_access rows              : {total}")
    print(f"  landing inside a NIC buffer  : {sum(hits.values())}")
    for (umem_id, ordinal, op), n in hits.most_common(20):
        print(f"      umem_id={umem_id} dev={ordinal} op={op}: {n}")

    print("\n=== VERDICT ===")
    if hits:
        print("  YES — NVBit sees stores into the buffers the NIC was handed.")
        print("  Option B is live. Next: confirm these are WQE writes (not just the")
        print("  payload buffer) by decoding create_qp for sq_base, and measure what")
        print("  nvbit_add_call_arg_reg_val costs for the doorbell value.")
        return 0
    print("  NO — IBGDA engaged, its queues are in GPU memory, and not one")
    print("  instrumented access touched them.")
    print("  CONFOUND before concluding: this run sets PCOMM_KERNEL_NAME=put_to_next,")
    print("  so only that kernel is instrumented. If NVSHMEM's IBGDA path is not")
    print("  inlined into it, the stores exist but were never instrumented. Re-run")
    print("  without the filter to separate 'NVBit cannot see them' from 'NVBit was")
    print("  not looking' — but expect case 07's hang, which is why the filter is")
    print("  there. Only after that is option B properly dead and option A the answer.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
