# pcomm demo — two captures on a MareNostrum 5 allocation

`pcomm` is the ParCore lab's GPU-communication profiler. This session captures
the two transports it exists to see, one demo each:

1. **GPU memory writes over NVLink** (`-p p2p`, the gpumem/NVBit provider) —
   GPU-initiated peer stores, per instruction, attributed to device pairs.
2. **InfiniBand verbs** (`-p ib`, the ibverbs provider) — MRs, QPs, posted
   sends and completions on the wire between two nodes.

Payloads are deliberately tiny (KBs, not GBs) so every number in the output can
be predicted from the workload's arguments before you run it — that is the
whole trick of reading a capture.

This is a bsh session: point at a `% cell` and press `<CR>` to run it; the
out fences below cells are owned by bsh and refresh on re-run. The fences in
this file are **sample output from a prior session** (counts match the suite's
committed goldens, scaled to this demo's sizes) — re-run to make them yours.

## 0. The allocation this session assumes

Run once from a login node (alogin2 if alogin1 refuses ssh), then open this
file inside it. Two accelerated nodes, 4× H100 each, NDR InfiniBand between:

koc296797@alogin2.bsc.es%% salloc --account=etur02 --qos=acc_debug --nodes=2 --ntasks-per-node=4 --gres=gpu:4 --cpus-per-task=20 --time=00:30:00

Environment for every cell below (persistent `%%` session — the suite's
common.sh sets $PCOMM, $PCOMM_READ, provider paths, and the gcc-13.2 runtime):

koc296797@alogin2.bsc.es%% export PCOMM_ROOT=$HOME/proj/pcomm
koc296797@alogin2.bsc.es%% source $PCOMM_ROOT/tests/hpc/suite/common.sh && pcomm_load_runtime && $PCOMM --list

```out
pcomm providers — pick with -p <name>[,<name>...]

  name     scope        capture
  ───────  ───────────  ─────────────────────────────────────────────
  ib       inter-node   InfiniBand wire traffic — QPs, sends, completions (host-side verbs)
           aliases: ib, net, ibverbs

  p2p      intra-node   GPU-initiated peer read/write over NVLink (NVBit, à la Snoopie)
           aliases: p2p, nvlink, gpumem, snoopie
  ...
```

---

## 1. Capturing GPU memory writes (NVLink peer stores)

The workload: `p2p_kernel` — each GPU stores into the *next* GPU's buffer over
NVLink and loads its own. Deterministic by construction: N GPUs × ELEMS threads
× ITERS launches ⇒ a store count you can compute on paper.

Build it once (nvcc wants the gcc-11.4 host compiler; run time wants 13.2):

koc296797@alogin2.bsc.es%% pcomm_load_build && nvcc -O2 -gencode arch=compute_90,code=sm_90 -Wno-deprecated-gpu-targets $PCOMM_ROOT/tests/hpc/cases/03_p2p_kernel/p2p_kernel.cu -o ~/p2p_kernel -lcudart && pcomm_load_runtime

### 1.1 Run it under pcomm

1024 elements × 2 iters on one node's 4 GPUs. Expected peer stores:
4 GPUs × 2 iters × 1024 threads = **8192 stores of 4 B** — 2048 per ring edge.
(`PCOMM_RANK=0` pins the rank; no MPI here, so it would fall back to the pid.)

koc296797@alogin2.bsc.es%% rm -rf /tmp/demo-p2p && srun --nodes=1 --ntasks=1 --gres=gpu:4 --overlap env PCOMM_RANK=0 $PCOMM -p p2p -o /tmp/demo-p2p -- ~/p2p_kernel 1024 2

```out
pcomm: 1 provider → 1 pass
  · p2p          → /tmp/demo-p2p
p2p_kernel OK: 4 GPUs, 1024 elems, 2 iters (ring peer writes)
```

(NVBit spends ~20-30 s instrumenting modules before the first launch — that is
startup cost, not the workload; it is why the payload being small matters.)

### 1.2 Parse the capture

The inventory reads each stream's own header — the capture tells you what it
is, you never infer from filenames:

koc296797@alogin2.bsc.es%% $PCOMM_READ /tmp/demo-p2p

```out
streams in /tmp/demo-p2p:
  file                                 type kind       stride   records           schema
  rank-0/700.pcs                        700 event          ..      8192 6dfd9bc459452a50
  rank-0/720.pcs                        720 entity         ..         4 6dfd9bc459452a50
  rank-0/740.pcs                        740 artifact       ..         2 6dfd9bc459452a50
  rank-0/590.pcs                        590 artifact       ..         2 6dfd9bc459452a50
  rank-0/510.pcs ...
```

Export to relational CSVs (one table per record type, intern handles resolved):

koc296797@alogin2.bsc.es%% $PCOMM_READ /tmp/demo-p2p --csv /tmp/demo-p2p.csv

```out
pcomm-read: 1 ranks, 8192 gpu-accesses, 0 ib-sends -> /tmp/demo-p2p.csv/{gpu_access,mem_object,source_line,clock_anchor,comm_edges,ib_*}.csv
```

### 1.3 Insights

**Who writes into whose memory?** The NVLink ring, straight from the data —
`run_dev` is where the SM ran, `mem_dev` is where the memory lives, and rows
with `run_dev != mem_dev` are exactly the traffic that crossed NVLink:

koc296797@alogin2.bsc.es%% sqlite3 :memory: -cmd '.mode csv' -cmd '.import /tmp/demo-p2p.csv/gpu_access.csv gpu_access' -cmd '.mode column' -cmd '.headers on' "SELECT run_dev, mem_dev, count(*) AS stores, sum(CAST(size AS INTEGER)) AS bytes FROM gpu_access WHERE op='store' AND run_dev != mem_dev GROUP BY 1,2 ORDER BY 1"

```out
run_dev  mem_dev  stores  bytes
-------  -------  ------  -----
0        1        2048    8192
1        2        2048    8192
2        3        2048    8192
3        0        2048    8192
```

That table *is* the topology: 0→1→2→3→0, the ring the kernel encodes — and the
counts match the paper prediction (2 iters × 1024 threads per edge, 4 B each).

**Total instrumented traffic by op** (local loads stay `run_dev == mem_dev`;
the RemoteFilter drops local loads from the capture, so stores dominate):

koc296797@alogin2.bsc.es%% sqlite3 :memory: -cmd '.mode csv' -cmd '.import /tmp/demo-p2p.csv/gpu_access.csv gpu_access' -cmd '.mode column' -cmd '.headers on' "SELECT op, count(*) AS n, sum(CAST(size AS INTEGER)) AS bytes FROM gpu_access GROUP BY op"

```out
op     n     bytes
-----  ----  -----
store  8192  32768
```

**Which allocations were hit?** The provider tracked every cudaMalloc as an
entity; accesses attribute to them by id:

koc296797@alogin2.bsc.es%% sqlite3 :memory: -cmd '.mode csv' -cmd '.import /tmp/demo-p2p.csv/mem_object.csv mem_object' -cmd '.mode column' -cmd '.headers on' "SELECT id, base, size, device FROM mem_object ORDER BY device"

```out
id  base            size  device
--  --------------  ----  ------
1   0x7f...000      4096  0
2   0x7f...000      4096  1
3   0x7f...000      4096  2
4   0x7f...000      4096  3
```

---

## 2. Capturing InfiniBand traffic (two nodes, real NDR IB)

The workload: stock `ibv_rc_pingpong` — server on node 0, client on node 1,
**10 iterations of 1024 B**. So the oracle is: 1 MR per side, 10 sends of
1024 B per side, 10 send + 10 recv completions per side, all `success`.

koc296797@alogin2.bsc.es%% nodes=($(scontrol show hostnames $SLURM_JOB_NODELIST)); echo "server=${nodes[0]} client=${nodes[1]}"

```out
server=as04r1b02 client=as04r1b09
```

### 2.1 Run it under pcomm

Both endpoints run under `pcomm -p ib` into one shared output dir, with
explicit ranks (server backgrounded; the client connects to it out-of-band):

koc296797@alogin2.bsc.es%% rm -rf /tmp/demo-ib && DEVCMD='DEV=$(ibv_devinfo | awk "/hca_id/{d=\$2} /PORT_ACTIVE/{print d; exit}")'
koc296797@alogin2.bsc.es%% srun --nodes=1 --ntasks=1 --nodelist=${nodes[0]} --overlap bash -c "$DEVCMD; PCOMM_RANK=0 $PCOMM -p ib -o /tmp/demo-ib -- ibv_rc_pingpong -d \$DEV -n 10 -s 1024" & sleep 6
koc296797@alogin2.bsc.es%% srun --nodes=1 --ntasks=1 --nodelist=${nodes[1]} --overlap bash -c "$DEVCMD; PCOMM_RANK=1 $PCOMM -p ib -o /tmp/demo-ib -- ibv_rc_pingpong -d \$DEV -n 10 -s 1024 ${nodes[0]}"; wait

```out
pcomm: 1 provider → 1 pass
  · ib           → /tmp/demo-ib
  local address:  LID 0x1eda, QPN 0x0023e5, PSN 0x123456
  remote address: LID 0x1f0e, QPN 0x015f84, PSN 0x654321
20480 bytes in 0.01 seconds = ... Mbit/sec
10 iters in 0.01 seconds = ... usec/iter
```

### 2.2 Parse the capture

koc296797@alogin2.bsc.es%% $PCOMM_READ /tmp/demo-ib

```out
streams in /tmp/demo-ib:
  file                                 type kind       stride   records           schema
  rank-0/100.pcs                        100 entity         88         1 6dfd9bc459452a50
  rank-0/102.pcs                        102 entity         64         1 6dfd9bc459452a50
  rank-0/104.pcs                        104 event          72         3 6dfd9bc459452a50
  rank-0/110.pcs                        110 event          88        10 6dfd9bc459452a50
  rank-0/112.pcs                        112 event          80        20 6dfd9bc459452a50
  rank-1/...                            (same shape)
```

Read that as physics before touching SQL: 1 memory region, 1 queue pair,
3 QP state transitions (INIT→RTR→RTS), 10 posted sends, 20 completions —
per rank. The oracle, straight from the stream headers.

koc296797@alogin2.bsc.es%% $PCOMM_READ /tmp/demo-ib --csv /tmp/demo-ib.csv

```out
pcomm-read: 2 ranks, 0 gpu-accesses, 20 ib-sends -> /tmp/demo-ib.csv/{gpu_access,mem_object,source_line,clock_anchor,comm_edges,ib_*}.csv
```

### 2.3 Insights

**Traffic** — every posted send by rank and opcode (enums decode to stable
names, never raw ABI integers):

koc296797@alogin2.bsc.es%% sqlite3 :memory: -cmd '.mode csv' -cmd '.import /tmp/demo-ib.csv/ib_post_send.csv ib_post_send' -cmd '.mode column' -cmd '.headers on' "SELECT rank, opcode, count(*) AS n, sum(CAST(length AS INTEGER)) AS bytes FROM ib_post_send GROUP BY rank, opcode"

```out
rank  opcode  n   bytes
----  ------  --  -----
0     send    10  10240
1     send    10  10240
```

**Topology** — `comm_edges` is derived rank-space: pcomm-read maps each QP's
RTR destination to the rank that owns it, collapsing run-varying wire ids
(QPNs, LIDs) into a stable src→dst graph:

koc296797@alogin2.bsc.es%% sqlite3 :memory: -cmd '.mode csv' -cmd '.import /tmp/demo-ib.csv/comm_edges.csv comm_edges' -cmd '.mode column' -cmd '.headers on' "SELECT * FROM comm_edges ORDER BY src_rank"

```out
src_rank  dst_rank  sends  bytes
--------  --------  -----  -----
0         1         10     10240
1         0         10     10240
```

**Completion health** — one `err*` row here would mean a dropped or failed
transfer; a healthy run is all `success`:

koc296797@alogin2.bsc.es%% sqlite3 :memory: -cmd '.mode csv' -cmd '.import /tmp/demo-ib.csv/ib_poll_cq.csv ib_poll_cq' -cmd '.mode column' -cmd '.headers on' "SELECT rank, status, opcode, count(*) AS n FROM ib_poll_cq GROUP BY 1,2,3 ORDER BY 1,2,3"

```out
rank  status   opcode  n
----  -------  ------  --
0     success  recv    10
0     success  send    10
1     success  recv    10
1     success  send    10
```

**How the connection was wired** — the RTR transition carries the remote
endpoint, which is how the graph above was possible at all:

koc296797@alogin2.bsc.es%% sqlite3 :memory: -cmd '.mode csv' -cmd '.import /tmp/demo-ib.csv/ib_modify_qp.csv ib_modify_qp' -cmd '.mode column' -cmd '.headers on' "SELECT rank, qp_num, dest_qp_num, dest_lid, state FROM ib_modify_qp WHERE state='rtr'"

```out
rank  qp_num  dest_qp_num  dest_lid  state
----  ------  -----------  --------  -----
0     9189    89988        7950      rtr
1     89988   9189         7898      rtr
```

**Timeline** — export a native Perfetto trace (one track per QP, sends and
completions as slices with real durations, cross-node clocks aligned via each
rank's ClockAnchors) and load it at https://ui.perfetto.dev:

koc296797@alogin2.bsc.es%% $PCOMM_READ /tmp/demo-ib --pftrace /tmp/demo-ib.pftrace

```out
pcomm-read: 2 ranks, 60 events -> /tmp/demo-ib.pftrace (4821 bytes; load in ui.perfetto.dev)
```

---

## 3. Where this goes

The point of one tool for both transports: a real application (NCCL, NVSHMEM)
moves data over NVLink *and* IB in the same run, and the providers co-run in
one process, one recorder, one timeline:

% echo 'pcomm -p ib,p2p -o out -- <your-app>   # one pass, both transports, one merged capture'
```out
pcomm -p ib,p2p -o out -- <your-app>   # one pass, both transports, one merged capture
```

The suite's case 06 (`nvshmem_coexist`) is exactly that, asserted with goldens;
`pcomm-read out --csv` merges both providers' tables by rank, and the same SQL
as above answers "which GPU write caused which wire send" questions across
transports. See `tests/hpc/cases/` for the full oracle set.
