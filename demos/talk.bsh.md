# pcomm — live demo

Workload: `asym_patterns hub` — NVSHMEM hub-and-spoke, one rank per node.
Talking points live in [`talk-slides.md`](talk-slides.md).

Every fence below is real output, from job `44748293` on 2026-08-18.

## setup — before the talk

koc296797@alogin2.bsc.es%% ls -l ~/asym_patterns-li-sm90
```out
-rwxr-xr-x 1 koc296797 koc 7583976 Aug 17 10:34 /home/koc/koc296797/asym_patterns-li-sm90
```
koc296797@alogin2.bsc.es%% command -v pcomm
```out
/home/koc/koc296797/.local/bin/pcomm
```

koc296797@alogin2.bsc.es%% salloc --account=etur02 --qos=acc_debug --nodes=4 --ntasks-per-node=1 --gres=gpu:1 --cpus-per-task=20 --time=00:40:00
```out
salloc: Granted job allocation 44748293
salloc: Nodes as02r2b[22,24-25,27] are ready for job
```
koc296797@alogin2.bsc.es%% source ~/proj/pcomm/tests/hpc/suite/common.sh && pcomm_load_runtime && export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:$NCCL_HOME/lib:$LD_LIBRARY_PATH" && echo ready
```out
ready
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
```

---

koc296797@alogin2.bsc.es%% ls -l ~/asym_patterns-li-sm90
```out
-rwxr-xr-x 1 koc296797 koc 7583976 Aug 17 10:34 /home/koc/koc296797/asym_patterns-li-sm90
```

---

koc296797@alogin2.bsc.es%% rm -rf ~/demo-out && mpirun -np 4 --map-by ppr:1:node -x PATH -x LD_LIBRARY_PATH -x NVSHMEM_BOOTSTRAP=MPI pcomm run -p ib -o ~/demo-out -- ~/asym_patterns-li-sm90 hub 4096 3
```out
pcomm: 1 provider → 1 pass
  · ib           → /home/koc/koc296797/demo-out
        … (once per rank)
asym_patterns[hub] PE 0/4: 3 edges, 12288 elems/iter, 3 iters
asym_patterns[hub] PE 1/4: 1 edges, 4096 elems/iter, 3 iters
asym_patterns[hub] PE 2/4: 1 edges, 4096 elems/iter, 3 iters
asym_patterns[hub] PE 3/4: 1 edges, 4096 elems/iter, 3 iters
```

---

koc296797@alogin2.bsc.es%% ls ~/demo-out ~/demo-out/rank-0
```out
/home/koc/koc296797/demo-out:
rank-0  rank-1  rank-2  rank-3

/home/koc/koc296797/demo-out/rank-0:
100.pcs  102.pcs  104.pcs  106.pcs  108.pcs  110.pcs  112.pcs  114.pcs
116.pcs  118.pcs  120.pcs  122.pcs  124.pcs  126.pcs  128.pcs  510.pcs
512.pcs  520.pcs  522.pcs  524.pcs  590.pcs  intern-blob.bin
```

koc296797@alogin2.bsc.es%% pcomm read ~/demo-out | head -24
```out
streams in /home/koc/koc296797/demo-out:
  file                                 type kind       stride   records           schema
  rank-1/100.pcs                        100 entity         96       187 6a1cf6fbb0b71348
  rank-1/510.pcs                        510 artifact       64       715 6a1cf6fbb0b71348
  rank-1/116.pcs                        116 event          56        75 6a1cf6fbb0b71348
  rank-1/118.pcs                        118 entity         88        26 6a1cf6fbb0b71348
  rank-1/110.pcs                        110 event          96     12348 6a1cf6fbb0b71348
  rank-1/520.pcs                        520 entity         80       127 6a1cf6fbb0b71348
  rank-1/522.pcs                        522 event          56       607 6a1cf6fbb0b71348
  rank-1/126.pcs                        126 entity         64       108 6a1cf6fbb0b71348
  rank-1/128.pcs                        128 event          88     12348 6a1cf6fbb0b71348
  rank-1/102.pcs                        102 entity         64       108 6a1cf6fbb0b71348
  rank-1/104.pcs                        104 entity         72       194 6a1cf6fbb0b71348
  rank-1/512.pcs                        512 artifact       72         2 6a1cf6fbb0b71348
  rank-1/120.pcs                        120 entity         88        79 6a1cf6fbb0b71348
  rank-1/112.pcs                        112 event          80     12408 6a1cf6fbb0b71348
  rank-1/114.pcs                        114 event          96     18364 6a1cf6fbb0b71348
  rank-1/524.pcs                        524 entity         56         1 6a1cf6fbb0b71348
  rank-1/106.pcs                        106 event          72       132 6a1cf6fbb0b71348
  rank-1/108.pcs                        108 entity         72        16 6a1cf6fbb0b71348
  rank-1/122.pcs                        122 entity         64        79 6a1cf6fbb0b71348
  rank-1/124.pcs                        124 event          80        60 6a1cf6fbb0b71348
  rank-1/590.pcs                        590 artifact       80         2 6a1cf6fbb0b71348
```

koc296797@alogin2.bsc.es%% du -sh ~/demo-out
```out
19M	/home/koc/koc296797/demo-out
```

---

%% rsync -az alogin2.bsc.es:demo-out ~/waste/pcomm/demo/
%% ~/pockt/uni/lab/pcomm/agent/run.sh ~/waste/pcomm/demo/demo-out 8777 --bind 100.64.0.3

```out
!! serving on 100.64.0.3 — no authentication on either half.
pcomm ui: capture …/demo-out — 4 ranks, 21 tables, 208925 rows
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

The matrix, for reference — six heavy edges, then a cliff into NVSHMEM's
bootstrap mesh:

```out
src_rank  dst_rank  sends  bytes
--------  --------  -----  -----
2         0         12318  50172     spokes → hub
3         0         12303  50112
1         0         12303  49212
0         2         1569   7548      hub → spokes
0         1         1554   7488
0         3         1554   6588
1         3         30     1020      ← bootstrap control mesh
3         1         30     1020
1         2         15     960
2         3         15     960
2         1         15     60
3         2         15     60
```

---

% echo 'pcomm run -p ib,gpumem,nccl -o out -- <your-app>'

---

## fallback

%% ~/pockt/uni/lab/pcomm/agent/run.sh ~/waste/pcomm/corpus/hubspoke_ib-44698048 8777

## check first

- `pcomm list` prints six providers (a stale `pcomm` on `PATH` fails at step 1)
- `~/waste/pcomm/corpus` exists on the presenting machine — syncthing skips it
- `agent/run.sh`, never `pcomm ui`: the latter starts no bridge and says nothing
- `pcomm read` prints all 4 ranks × 21 streams — pipe through `head -24` or it
  scrolls off the projector
