# pcomm — live demo

Workload: `asym_patterns hub` — NVSHMEM hub-and-spoke across **all 16 H100s** on
four nodes. Fifteen spokes push a full 16 KB slot to rank 0; rank 0 answers each
with a slot/8. Talking points live in [`talk-slides.md`](talk-slides.md).

`NVSHMEM_DISABLE_P2P=1` is the load-bearing flag: NVSHMEM would otherwise route
the three spokes that share rank 0's node over NVLink, where the NIC never sees
them, and `-p ib` would return a star missing three arms with no error to
explain it. With it, all 30 edges cross the HCA and the matrix is complete.

Say it plainly if asked: those intra-node edges go through the NIC **in
loopback**. Real device traffic, but not how the app would route left alone —
a picture of the pattern, not a basis for a bandwidth claim. Case 09 pins the
same flag for the same reason.

Every fence below is real output, from job `44748943` on 2026-08-18.

## setup — before the talk

koc296797@alogin2.bsc.es%% ls -l ~/asym_patterns-li-sm90
```out
-rwxr-xr-x 1 koc296797 koc 7583976 Aug 17 10:34 /home/koc/koc296797/asym_patterns-li-sm90
```

koc296797@alogin2.bsc.es%% command -v pcomm
```out
/home/koc/koc296797/.local/bin/pcomm
```

koc296797@alogin2.bsc.es%% salloc --account=etur02 --qos=acc_debug --nodes=4 --ntasks-per-node=4 --gres=gpu:4 --cpus-per-task=20 --time=00:40:00
```out
salloc: Pending job allocation 44748943
salloc: job 44748943 queued and waiting for resources
salloc: job 44748943 has been allocated resources
salloc: Granted job allocation 44748943
salloc: Waiting for resource configuration
salloc: Nodes as02r2b[22,24-25,27] are ready for job
srun: error: ioctl(TIOCGWINSZ): Inappropriate ioctl for device
srun: error: Not using a pseudo-terminal, disregarding --pty option
```

koc296797@alogin2.bsc.es%% hostname
```out
as02r2b22
```

`mpirun` is not on a bare login PATH — it comes from the NVHPC module, which is
what `pcomm_load_runtime` loads. Run this INSIDE the allocation, before the
capture cell, or that cell dies with `mpirun: command not found`:

koc296797@alogin2.bsc.es%% source ~/proj/pcomm/tests/hpc/suite/common.sh && pcomm_load_runtime && export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH" && command -v mpirun
```out
/apps/ACC/NVIDIA-HPC-SDK/25.7/Linux_x86_64/25.7/comm_libs/mpi/bin/mpirun
```

---

koc296797@alogin2.bsc.es%% pcomm list
```out
pcomm providers — pick with -p <name>[,<name>...]

  name     scope        capture
  ───────  ───────────  ─────────────────────────────────────────────
  ib       inter-node   InfiniBand wire traffic — QPs, sends, completions (host-side verbs)
                        aliases: ib, net, ibverbs

  gpumem   intra-node   GPU-initiated load/store — peer access over NVLink (NVBit, à la Snoopie)
                        aliases: gpumem, p2p, nvlink, snoopie, ldst

  ibgda    inter-node   GPU-initiated InfiniBand (IBGDA) — DEVX queue pairs, doorbell pages
                        aliases: ibgda, devx, mlx5, mlx5dv

  kernel   device       GPU kernel execution windows — when each kernel actually ran
                        aliases: kernel, kernels, cupti

  ucx      inter-node   UCX application intent — contexts, workers, endpoints, tagged sends
                        aliases: ucx, ucp

  nccl     collective   NCCL collective layer — communicators, collectives, peers, groups
                        aliases: nccl, collectives, coll

Providers that must own the runtime run in their own pass: pcomm re-runs
the workload once each and merges the passes under one -o directory.
```

---

koc296797@alogin2.bsc.es%% ls -l ~/asym_patterns-li-sm90
```out
-rwxr-xr-x 1 koc296797 koc 7583976 Aug 17 10:34 /home/koc/koc296797/asym_patterns-li-sm90
```

---

koc296797@alogin2.bsc.es%% rm -rf ~/demo-out && mpirun -np 16 --map-by ppr:4:node -x PATH -x LD_LIBRARY_PATH -x NVSHMEM_BOOTSTRAP=MPI -x NVSHMEM_DISABLE_P2P=1 pcomm run -p ib -o ~/demo-out -- ~/asym_patterns-li-sm90 hub 4096 3
```out
pcomm: 1 provider → 1 pass
  · ib           → /home/koc/koc296797/demo-out
        … (once per rank)
asym_patterns[hub] PE 0/16: 15 edges, 7680 elems/iter, 3 iters
asym_patterns[hub] PE 1/16: 1 edges, 4096 elems/iter, 3 iters
        … (PE 2..15, 1 edge each)
```

---

koc296797@alogin2.bsc.es%% ls ~/demo-out; ls ~/demo-out/rank-0
```out
rank-0   rank-10  rank-12  rank-14  rank-2  rank-4  rank-6  rank-8
rank-1   rank-11  rank-13  rank-15  rank-3  rank-5  rank-7  rank-9

100.pcs  104.pcs  108.pcs  112.pcs  116.pcs  120.pcs  124.pcs  128.pcs
102.pcs  106.pcs  110.pcs  114.pcs  118.pcs  122.pcs  126.pcs  510.pcs
512.pcs  520.pcs  522.pcs  524.pcs  590.pcs  intern-blob.bin
```

koc296797@alogin2.bsc.es%% pcomm read ~/demo-out | head -24
```out
streams in /home/koc/koc296797/demo-out:
  file                                 type kind       stride   records           schema
  rank-7/100.pcs                        100 entity         96       203 6a1cf6fbb0b71348
  rank-7/510.pcs                        510 artifact       64       774 6a1cf6fbb0b71348
  rank-7/116.pcs                        116 event          56        75 6a1cf6fbb0b71348
  rank-7/118.pcs                        118 entity         88        27 6a1cf6fbb0b71348
  rank-7/110.pcs                        110 event          96     12408 6a1cf6fbb0b71348
  rank-7/520.pcs                        520 entity         80       143 6a1cf6fbb0b71348
  rank-7/522.pcs                        522 event          56       648 6a1cf6fbb0b71348
  rank-7/126.pcs                        126 entity         64       132 6a1cf6fbb0b71348
  rank-7/128.pcs                        128 event          88     12408 6a1cf6fbb0b71348
  rank-7/102.pcs                        102 entity         64       132 6a1cf6fbb0b71348
  rank-7/104.pcs                        104 entity         72       194 6a1cf6fbb0b71348
  rank-7/512.pcs                        512 artifact       72         2 6a1cf6fbb0b71348
  rank-7/120.pcs                        120 entity         88        79 6a1cf6fbb0b71348
  rank-7/112.pcs                        112 event          80     12528 6a1cf6fbb0b71348
  rank-7/114.pcs                        114 event          96     18424 6a1cf6fbb0b71348
  rank-7/524.pcs                        524 entity         56         1 6a1cf6fbb0b71348
  rank-7/106.pcs                        106 event          72       204 6a1cf6fbb0b71348
  rank-7/108.pcs                        108 entity         72        16 6a1cf6fbb0b71348
  rank-7/122.pcs                        122 entity         64        79 6a1cf6fbb0b71348
  rank-7/124.pcs                        124 event          80       120 6a1cf6fbb0b71348
  rank-7/590.pcs                        590 artifact       80         2 6a1cf6fbb0b71348
```

koc296797@alogin2.bsc.es%% du -sh ~/demo-out
```out
84M	/home/koc/koc296797/demo-out
```

---

%% rsync -az alogin2.bsc.es:demo-out ~/waste/pcomm/demo/
%% ~/pockt/uni/lab/pcomm/agent/run.sh ~/waste/pcomm/demo/demo-out 8777 --bind 100.64.0.3

```out
!! serving on 100.64.0.3 — no authentication on either half.
pcomm ui: capture …/demo-out — 16 ranks, 21 tables, 968956 rows
pcomm ui: http://100.64.0.3:8777/  (5 embedded assets)

  open  http://100.64.0.3:8777/?agent=http://100.64.0.3:8778

pcomm agent bridge: http://100.64.0.3:8778  ->  capture http://100.64.0.3:8777
  model: muse-glimmer-30b
```

`--bind` is what makes the page reachable from the laptop over tailscale; drop
it and both halves stay on loopback. It has to be one flag, because it sets
three things that must agree — the server's bind, the bridge's bind, and the
bridge's `--ui`.

Browser: **Matrix** → **Timeline** → **Chat**, one question.

The heaviest edges, for reference — every spoke into rank 0, and the whole
column is that one hub:

```out
src_rank  dst_rank  sends  bytes
--------  --------  -----  -----
8         0         12318  50172
12        0         12303  50112
14        0         12302  50048
15        0         12302  50048
2         0         12304  49276
3         0         12289  49216
1         0         12303  49212
4         0         12303  49212
```

And the two things moving, separated by opcode — this is the query worth
having ready if anyone asks why the matrix ratio is not exactly 8:

```out
opcode      len  n       bytes
----------  ---  ------  ------
rdma_write  4    207360  829440
send        4    960     3840
send        64   960     61440
rdma_write  128  54      6912
```

207360 = 15 spokes × 12288 puts + the hub's 15 × 1536 replies, exactly. So the
application's own traffic is 8× asymmetric to the element; `send` is NVSHMEM's
synchronisation, 60 per rank regardless of message size, and `comm_edges` sums
both.

---

% echo 'pcomm run -p ib,gpumem,nccl -o out -- <your-app>'

---

## fallback

%% ~/pockt/uni/lab/pcomm/agent/run.sh ~/waste/pcomm/corpus/hubspoke_ib-44698048 8777

## check first

- `pcomm list` prints six providers (a stale `pcomm` on `PATH` fails at step 1)
- `~/waste/pcomm/corpus` exists on the presenting machine — syncthing skips it
- `agent/run.sh`, never `pcomm ui`: the latter starts no bridge and says nothing
- `pcomm read` prints all 16 ranks × 21 streams — 336 lines. Pipe through
  `head -24` or it scrolls off the projector
- the capture is 84 MB, so start the `rsync` before you need it, not during
