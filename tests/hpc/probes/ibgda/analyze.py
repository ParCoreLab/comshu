#!/usr/bin/env python3
"""Answer Stage 2's question from a probe log + a pcomm capture.

    analyze.py <probe-log-dir> <capture.csv-dir>

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


def read_probe(log_dir):
    """Read every per-rank probe.txt.<rank> under log_dir.

    Malformed lines are counted, not skipped silently: a shredded log is what
    job 44156479 produced, and a parser that quietly drops fragments would have
    turned that into a confident wrong answer.
    """
    umems, uars, devx = [], [], collections.Counter()
    bad, ranks = 0, []
    names = sorted(n for n in os.listdir(log_dir) if n.startswith("probe.txt."))
    for name in names:
        ranks.append(name.rsplit(".", 1)[-1])
        with open(os.path.join(log_dir, name)) as f:
            for line in f:
                parts = line.split()
                if not parts:
                    continue
                try:
                    if parts[0] == "umem" and len(parts) >= 6:
                        addr, size, mem_type, ordinal, umem_id = parts[1:6]
                        umems.append(
                            (int(addr, 16), int(size), int(mem_type), int(ordinal), umem_id)
                        )
                    elif parts[0] == "uar" and len(parts) >= 3:
                        uars.append((int(parts[1], 16), parts[2]))
                    elif parts[0] == "devx" and len(parts) >= 2:
                        devx[int(parts[1], 16)] += 1
                    else:
                        bad += 1
                except ValueError:
                    bad += 1
    print(f"  probe logs read              : {len(names)} ({', '.join(ranks) or 'none'})")
    if bad:
        print(f"  MALFORMED LINES              : {bad}  <- log corruption, treat with care")
    return umems, uars, devx


def main():
    log_dir, csv_dir = sys.argv[1], sys.argv[2]
    print("--- 0. inputs ---")
    umems, uars, devx = read_probe(log_dir)
    print()

    # DEVX QP creations do NOT prove IBGDA engaged, and reading them that way is
    # the mistake this guard exists to prevent. Job 44156479 logged 159 DEVX
    # CREATE_QPs on a run where IBGDA failed to initialise outright ("neither
    # nv_peer_mem, or nvidia_peermem detected. Skipping transport") — HPC-X/UCX
    # drives DEVX for ordinary MPI, so the count was never NVSHMEM's to claim.
    #
    # The discriminator is the one SigmaTrace uses (Mlx5dv.cpp:82-87): a QP whose
    # work-queue buffer lives in GPU memory. A CPU-side DEVX QP cannot produce
    # GPU doorbells, so no GPU-resident umem means no IBGDA, whatever the QP
    # count says.
    print("--- 1. did IBGDA engage? ---")
    n_qp = devx.get(MLX5_CMD_OP_CREATE_QP, 0)
    gpu_umems = [u for u in umems if u[2] == CU_MEMORYTYPE_DEVICE]
    print(f"  mlx5dv_devx_obj_create calls : {sum(devx.values())}")
    print(f"    of which CREATE_QP (0x500) : {n_qp}   <- NOT evidence on its own")
    print(f"  mlx5dv_devx_alloc_uar calls  : {len(uars)}")
    print(f"  umems resident in GPU memory : {len(gpu_umems)}   <- this is the evidence")
    for op, n in sorted(devx.items()):
        print(f"    opcode 0x{op:04x} : {n}")
    if not gpu_umems:
        print("  VERDICT: IBGDA did NOT engage — no NIC buffer is in GPU memory.")
        print("  Everything below is vacuous. Read the NVSHMEM_DEBUG=INFO output for")
        print("  the transport-init line before concluding anything about option B;")
        print("  a DEVX QP count above is MPI's, not NVSHMEM's.")
        unknown = [u for u in umems if u[2] == -1]
        if len(unknown) == len(umems) and umems:
            print(f"  CAUTION: all {len(umems)} umems classified as -1. Check the cuerr=")
            print("  column — if it is 3 (NOT_INITIALIZED) the probe could not ask CUDA")
            print("  rather than CUDA answering 'host memory', and this verdict is void.")
        return 2

    print("\n--- 2. NIC-registered buffers in GPU memory ---")
    print(f"  umem registrations total     : {len(umems)}")
    print(f"    resident in GPU memory     : {len(gpu_umems)}")
    for addr, size, _, ordinal, umem_id in sorted(gpu_umems)[:20]:
        print(f"      0x{addr:012x} + {size:<10} dev={ordinal} umem_id={umem_id}")

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
