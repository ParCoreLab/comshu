# 06_llm_scaleout — what the capture and its documentation could not tell me

Black-box: `providers/`, `libcomshu/` (beyond `schema/`), `comshu-read/`,
`comshu-ui/`, `gpu-backends/`, `tools/`, `plans/` and `BUGS-FOUND.md` were not
read. Every entry is a place where I went to the documentation an outside user
has and did not find the answer. Ordered by cost. Job **45330280**, 16 GPUs
across 4 MN5 nodes.

---

## Q1 — `comshu types` omits a type the tool emits, exports, and documents

**The one that would mislead a user furthest, and I predicted the opposite.**

`PREDICTION.md` P5.2 said `gpu_device` would be **absent**, because `comshu types`
— documented as *"type-id -> table name, for this build"* — prints 59 type rows
and 528 is not among them, though `libcomshu/schema/comshu.fbs:210` declares it. I
named the alternative in advance: *"If it appears anyway, then `comshu types` is
incomplete, which is a worse finding than the table being missing, because it is
the one place a user is told to look."*

It appears. Three surfaces of the same tool disagree:

```
$ comshu types | grep -c '^[0-9]'
59                                        # and 528 is not one of them
$ comshu read <capture> | grep 528
  rank-7/528.pcs   528  artifact  80  4  7e61512d00383e13
$ ls <export>.pq | grep gpu
gpu_device.parquet
```

So the record is emitted, streamed, exported and queryable, and the verb whose
entire job is to map a type id to a table name does not know about it. A user
who checks `comshu types` before writing a query concludes the data is not there.

**Ask:** generate `comshu types` from the schema. A hand-maintained list of the
types a build carries is a list kept in step with the build by nobody.

## Q2 — `link_health` files two findings that any NCCL capture must produce, and the third one explains why

Measured on both arms, and it is the one assertion in this directory that fails:

```
wr_flight_unretired                 69470   work requests posted but never matched to a completion
wr_flight_unsignalled_qps             160   queue pairs posting more WRs than they complete — unsignalled, so unpairable
wr_flight_selective_signalling_qps    160   unpairable queue pairs that declared sq_sig_all = 0 — certainly selective signalling
```

69,470 of 105,948 posts, 65.6 %. 160 queue pairs is ten per rank, on all sixteen.

**The third finding's own note establishes that the first two are not defects.**
It says the queue pairs declared `sq_sig_all = 0`, i.e. selective signalling, and
that is *certain* rather than inferred. A work request with no completion is what
selective signalling means; NCCL's proxy posts unsignalled sends and reaps in
batches. So the link has already proved the traffic is unpairable by design and
then counts the unpairable work requests as a finding anyway.

The consequence for a user is concrete: **every ibverbs capture of every NCCL
workload files two findings it cannot avoid**, so `link_health` — the table whose
purpose is "is this capture trustworthy" — is permanently dirty for the most
common multi-node GPU workload there is, and a real defect appearing beside them
would be one line in three rather than one line in zero.

The other fifteen findings are all zero, including the four that would matter
here (`colliding_endpoints`, `unresolved_sends`, `self_edges`,
`ambiguous_recv_buffers`).

**Ask:** exclude the queue pairs `wr_flight_selective_signalling_qps` has already
identified from `wr_flight_unretired`, or fold the three into one finding whose
count is "unretired work requests on queue pairs that are NOT selectively
signalled".

## Q3 — `rank_identity.node_ix` is not the node's index in rank order, and nothing says what it is

```
rank,node_ix,node_hash,device,devices,label
0..3    -> node_ix 1
4..7    -> node_ix 2
8..11   -> node_ix 0
12..15  -> node_ix 3
```

Under `--map-by ppr:4:node` ranks 0-3 are on the first node, so the obvious
reading of `node_ix` — a small dense index over the nodes, in rank order — is
wrong. The *partition* is perfectly correct (four groups of four, contiguous,
matching the launcher), and my assertion is written on `node_hash` equality
rather than on `node_ix`, so nothing here depended on it. But a user who orders
a report by `node_ix` gets nodes in an order that is neither rank order nor
anything else they can predict, and the label prints it: rank 0 is labelled
`n1 · GPU 0`.

Related and also undocumented: `node_hash` is **unsigned** 64-bit — one of the
four values here is 13,086,696,872,099,297,363, which is greater than 2^63 — so a
consumer reading it into a signed column gets a negative number.

**Ask:** say what `node_ix` is ordered by, or make it rank order.

## Q4 — `comm_edges.unit` says `messages` here and `datatype-units` on a UCX capture, and both columns are called `bytes`

Same table, same column names, two captures on the same machine in the same week:

```
verbs lane (this study):    5,9,verbs,messages,network,4392,102763008
ucx lane (02_lammps r2):    0,12,ucx,datatype-units,network,120,10239889
```

`unit` is doing real work — it is telling you that the `bytes` column means
different things on the two lanes — and its vocabulary is published nowhere. On
the UCX lane `datatype-units` is only bytes because Open MPI pre-multiplies for a
contiguous power-of-two datatype; for `MPI_Type_contiguous(3, MPI_FLOAT)` it
would be elements, off by twelve, with the same column name and the same `unit`
string.

**Ask:** publish the values `unit` can take and what each implies about `bytes`.

## Q5 — Nothing in the capture says which algorithm NCCL used, and the measurement is one part in 170 from a ring

Confirming 04_llmc's Q1 at four times the scale, and sharpening it into a number.

Measured IB payload, all 16 ranks: **1,809,714,624 B**. A flat ring over 16 GPUs
would put `4 · 2 · (15/16) · S = 1,821,450,240 B` on the fabric with
`S = 242,860,032`. That is **99.36 %** — the capture is telling me, arithmetically
and unmistakably, that NCCL ran a ring across all sixteen GPUs rather than a
hierarchical or tree algorithm.

**But no field in the capture says so.** I had to derive it from a byte total and
a formula. `NCCL_DEBUG=INFO` on the bare arm says it directly (232 `via P2P`
lines against 160 `via NET`), which is why this study puts that on the bare run —
but that is NCCL's diagnostic, not comshu's record, and it is not in any capture.

**Ask:** a door for NCCL's per-collective tuning decision, or a documented note
that the algorithm is not recoverable. It is the difference between "the trace
shows what happened" and "the trace shows enough that you can work out what
happened if you already know the formula".

## Q6 — `transport_mix` cannot see NCCL, and the contract does not say so

Confirmed by measurement rather than assumed. The `z0u` arm loads
`nccl,ib,ucx,ucx_deep` in one pass; `uct_binding` reports `accepted = 1` on all
sixteen ranks, so the UCT layer is genuinely engaged:

```
scope,tl_name,ops,out_b,in_b
intra-node,sysv,   1466,284896,     0
network,   rc_mlx5,1190,169194,125824
```

**579,914 bytes of UCT traffic against 1,809,714,624 bytes of InfiniBand
traffic — 0.032 %.** Everything `transport_mix` sees is llm.c's MPI bootstrap;
not one byte of the gradient appears, because NCCL's IB transport is its own and
does not go through UCX.

That is correct behaviour and it is a scope statement worth writing down,
because `BRIEF-BLACKBOX.md` presents `transport_mix.scope` as *the* answer to
"did this leave the node" — *"If you are asking whether traffic left the node,
group by `scope`"* — with no qualification. For a NCCL workload that advice
produces a confident answer over 0.03 % of the traffic.

**Ask:** one sentence saying `transport_mix` covers UCX-transported traffic only,
and that `comm_edges.scope` is the portable answer.

## Q7 — `-p kernel` needs `libcupti.so` on `LD_LIBRARY_PATH` and nothing says so

The `z0k` arm died at exec:

```
train_gpt2cu: error while loading shared libraries: libcupti.so.12: cannot open
shared object file: No such file or directory
Exit code: 127
```

sixteen times, and `mpirun` reported it as processes exiting non-zero — so the
failure is loud, which is the good half. The bad half is that `comshu run` had
already accepted `-p nccl,ib,kernel`, found the backend library, and injected it;
the missing dependency is the backend's own, one level down, and the error names
the *application* rather than the provider. On MN5 `libcupti.so.12` lives in
`$COMSHU_SDK/cuda/extras/CUPTI/lib64`, which nothing in `common.sh` puts on the
path.

So the kernel layer is unanswered in this study: the launch-count oracle
(35 NCCL device kernels per rank at `-z 0`) is not re-tested at sixteen ranks.
Everything else stands; the arm was written to fail without taking the study
with it.

**Ask:** have the kernel backend check for its own runtime dependency and say
which one is missing, or document the path.

## Q8 — An assertion cannot claim a table is empty on a Parquet export, and the two exporters still disagree

Carried from 04_llmc and hit again. `comshu export --parquet` omits a raw table
with no rows entirely, so `SELECT count(*) FROM nccl_p2p` is a `Catalog Error`
and `-- requires: nccl_p2p` reports `[skip]` — while `comshu export --csv` writes
a header-only file and `comshu link` writes its derived tables out even when
empty. Three behaviours, one capture. `comshu link` accepts only Parquet, so
every derived-layer assertion lives on the representation where an empty raw
table is a parse error, and a parse error reads like a pass unless the runner
looks for it.

My runner resolves table presence from the filesystem (`ls *.parquet`) because
there is no documented query that answers it. `duckdb_tables()` returns zero
rows — an export is a set of views — which is the first thing anyone will get
wrong.

## Q9 — `comshu sql` still cannot run an ordinary `.sql` file, and now there are three warts

Documented: a leading comment is rejected. Undocumented: a trailing semicolon is
rejected too (the text is wrapped in `COPY (<query>) TO`). Undocumented and new
this round: because there is no way to get a `params` table into a `comshu sql`
query, a runner must splice one in as a leading CTE — and then **any assertion
that opens with its own `WITH` is a syntax error**. That killed five of nine
assertions on the sibling study's first job.

Every one of the three is three lines to work around and all three had to be
found by running into them.

**Ask:** does `comshu sql` accept more than one statement? If it did, an assertion
could materialise its own `params` and no runner would be needed. Nothing says,
and this is now the fourth private runner in `tests/hpc/apps/`.

## Q10 — Integer division is float division, which is a DuckDB fact that the assertion style guide should carry

`a / b` on two integers returns a DOUBLE in DuckDB, so `rank / 4 = rank / 4`
compares 3.5 against 3.75 and every node-membership test written the obvious way
is false for two ranks that are on the same node. It cost a queue slot on the
sibling study and it is invisible in the failure message — the symptom was
`rank_identity` appearing to group the ranks wrongly when it was perfect.

`tests/hpc/cases/*/asserts/*.sql` is the documented model for assertion style
and none of them divides. `//` or `div()`; and **not** `CAST(a/b AS BIGINT)`,
which rounds half away from zero.

## Q11 — `gpu_device` has 4 rows per rank and the schema comment reads as if it had one

Not a defect — the fuller behaviour is better — but it is undocumented and it
changes every query written against the table. Measured: **64 rows, 16 ranks,
4 rows each**, so every rank enumerates all four GPUs on its node rather than
only the one it uses. The consequences are exactly what the record was designed
to show, and they are worth recording since this is the first application to
produce them:

| | |
|---|---|
| distinct UUIDs | **16** — one per physical GPU, globally unique |
| distinct `pci_bus` | **4** — the bus address repeats on all four identical nodes |
| distinct UUIDs per `node_hash` | **4**, on every node |
| distinct UUIDs per `ordinal` | **4** — ordinal 0 is four different GPUs |

`comshu.fbs:210`'s rationale — *"the bus id repeats across nodes"* — is confirmed
exactly. What is not written down is that the table is per (rank, ordinal) rather
than per rank, or what `name` (an intern handle, 1 on every row here) holds.
