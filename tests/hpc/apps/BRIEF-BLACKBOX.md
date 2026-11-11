# Black-box brief — validating comshu against applications nobody here wrote

Round 2, started 2026-08-29. Read `README.md` beside this file first: the four
claim types, the hard rules and the machine table all still apply. This file
says what is **different** this round, and the difference is the point.

## The one rule that defines this round

**You may not read comshu's implementation.** Specifically, these are off limits:

```
providers/        libcomshu/          comshu-read/       comshu-ui/
gpu-backends/     tools/             plans/            BUGS-FOUND.md
```

You get what a competent outside user gets: the command-line interface, the
record schema, the derived-table contracts, and the application's own source.
Nothing else. The allowed reading list is at the foot of this file.

**Why.** Round 1's agents had the whole repo. When a capture looked strange they
opened `providers/ucx/ucx_provider.cpp` and explained it — which is a fine way to
debug a tracer and a useless way to find out whether the tracer explains itself.
Every question you cannot answer from the capture and its documentation is a
finding: it means a real user, six months from now, cannot answer it either.

So when you get stuck, **do not guess at the implementation and do not ask for
it. Write the question down.** A list of "things the capture would not tell me"
is a first-class deliverable of this round, ranked equally with the four claim
types in `README.md`.

If you believe a column is wrong, say what you expected and why, from the
application's behaviour. Do not go looking for the line of C++ that produced it.

## The interface, in full

```
comshu run     -p <list> [-o DIR] -- <app> [args]   run a workload under providers
comshu list                                          which providers exist, what each captures
comshu read    <capture>                             stream inventory: what is in a capture
comshu dump    <capture> <type-id>                   decode one raw stream to stdout
comshu export  <capture> --csv|--parquet|--perfetto|--pftrace <out>
comshu link    <export>                              derive the rank-space tables
comshu sql     <export> "<query>"                    query an export
comshu types                                         type-id -> table name for this build
comshu ui      <capture|export> [--port N]           serve the capture as a page
```

Providers, selected by `-p` (comma-separated; the hook-based ones co-run in a
single pass, and `comshu list` says which are which):

| name | scope | captures |
|---|---|---|
| `ib` | inter-node | InfiniBand verbs — QPs, sends, completions |
| `ucx` | inter-node | UCX application intent — contexts, workers, endpoints, tagged sends, one-sided ops |
| `ucx_deep` | inter-node | UCX transport layer — interfaces, lanes, the wire operations UCX issues |
| `ibgda` | inter-node | GPU-initiated InfiniBand — DEVX queue pairs, doorbell pages |
| `gpumem` | intra-node | GPU-initiated load/store over NVLink (NVBit) |
| `kernel` | device | GPU kernel execution windows |
| `nccl` | collective | NCCL communicators, collectives, peers, groups |

The derived tables `comshu link` builds, with their column contracts:

```
comm_edges         src_rank,dst_rank,transport,unit,scope,sends,bytes
edge_sizes         src_rank,dst_rank,transport,unit,scope,size_class,sends,bytes
endpoint           rank,qp_num,lid,peer_lid,peer_qp_num,peer_rank,transport,sends,bytes
object_access      owner_rank,object,obj_offset,size,source,actor_rank,actor_device,exact
rank_identity      rank,node_ix,node_hash,device,devices,label
run_provenance     rank,pid,argv,providers,build,images,env
vocabulary         vocab,value,name
transport_mix      rank,tl_name,dev_name,scope,op,ops,out_bytes,in_bytes,advertised_bytes,header_bytes
tag_flow           rank,tag,status,ucp_sends,ucp_units,uct_ops,uct_payload,doors
tag_recv_resolved  rank,worker,message,status,sender_tag,offered,delivered
nccl_execution     rank,nccl_comm_hash,nccl_rank,nranks,nnodes,func,algo,proto,datatype,count,n_channels,n_warps,seq_number,event_id,parent_event_id,t0_ns,t1_ns
nccl_peer_traffic  rank,nccl_comm_hash,func,algo,proto,peer,dir,channel_id,n_steps,chunk_size,planned_bytes,event_id,parent_event_id,t0_ns,t1_ns
link_health        finding,severity,n,note
```

## What NCCL chose — ADDED 2026-09-02

`nccl_execution` is the **only** table that says which algorithm and protocol a
run actually used. `nccl_collective` is what the application asked for; NCCL
picks Ring vs Tree vs NVLS and Simple vs LL vs LL128 inside the enqueue, after
the API call has returned. Case 02's own notes record that pinning
Ring/Simple/1-channel moved its wire volume from 57,280 sends to 26,880 — the
choice is roughly half the traffic.

Three traps, all of which have a wrong-looking-but-plausible failure mode:

- `algo`/`proto` come back **UPPERCASE** (`RING`, `SIMPLE`) while
  `NCCL_ALGO`/`NCCL_PROTO` are spelled mixed-case. Fold case before comparing to
  `run_provenance.env`.
- **An empty `nccl_execution` is ambiguous.** Check
  `link_health.nccl_profiler_door_shut`: non-zero means the profiler plugin
  never loaded, so the capture makes no statement about the algorithm. That is
  not "the workload ran no collectives".
- **Broadcasts may have no row at all.** `NCCL_ALLGATHERV_ENABLE` defaults to 1
  and rewrites a broadcast onto a queue NCCL's own profiler never walks, so the
  call appears in `nccl_collective` and nowhere in `nccl_execution`. This is
  NCCL's limitation, not the tool's, and
  `link_health.nccl_bcast_invisible_to_profiler` counts it. Do not report it as
  dropped work.
- **`nccl_peer_traffic` is legitimately empty on a single node**, because proxy
  operations only exist for traffic through NCCL's CPU proxy — NVLink/P2P and
  device-initiated traffic never reach it. Its `planned_bytes` is
  `n_steps * chunk_size`, a **schedule**, and is not comparable to
  `ib_post_send.bytes`, which are posts that happened.

## The string vocabularies — ADDED 2026-09-02

Four columns in the tables above hold strings from a **closed** set, and until
now not one of those sets was written down anywhere a reader of this brief could
see. Three separate round-2 studies filed that as a finding, correctly: `n = 0`
on five rows and `n = 0` on twenty rows look equally clean when you cannot tell
how many rows there should be.

`comm_edges.unit` — **what `sends` counts, and the three values are not
comparable.** Never add `sends` across transports without saying so.

| unit | on transport | one `sends` is |
|---|---|---|
| `messages` | `verbs`, `devx` | one posted work request |
| `accesses` | `nvlink` | one recorded GPU access |
| `datatype-units` | `ucx` | one UCP datatype count — **equal to bytes only for a contiguous 8-bit datatype**. On any other datatype you are off by the type size. |

`comm_edges.transport` — `verbs`, `devx`, `nvlink`, `ucx`.

`tag_flow.status` — `tag_matched` (both layers present; `doors` says which
hardware tag-matching door), `no_tagged_wire_op` (a UCP send with no *tagged*
wire operation — **not lost traffic**: the message took a door that carries no
tag, and `transport_mix` accounts for it by transport instead), `wire_only` (a
tagged wire operation with no UCP send).

`tag_recv_resolved.status` — `probe_matched`, `no_probe`. **`no_probe` is a real
signal and not an error**: the receive consumed a message no probe announced.

`link_health.finding` and `link_health.severity` — the full closed list is in the
`link_health` paragraph below.

`rank_identity.node_ix` — **not rank order.** It is a dense rank over the
node_hash *value*, so ranks 0–3 may resolve to `n1` and ranks 4–7 to `n0`.
Nothing is wrong when they do. `node_hash` itself is **unsigned 64-bit**; a
reader that renders it signed shows large hashes as negative.

The numeric vocabularies (`MemOp`, `MemSpace`, `TimeKind`, `RecordKind`,
`EdgePredicate`, `KernelSource`) are in the `vocabulary` table, one row per
`(vocab, value, name)`. That table exists because `--csv` decodes enums,
`--parquet` does not, and the derived layer is Parquet-only — so the export that
carries `comm_edges` is exactly the one that shows `3` where the other shows
`Prefetch`. Join it rather than hard-coding a mapping:

```sql
SELECT v.name AS op, count(*) FROM gpu_access a
JOIN vocabulary v ON v.vocab = 'MemOp' AND v.value = a.op GROUP BY 1;
```

`transport_mix.scope` — ADDED 2026-08-30, and it was my omission rather than the
tool's. The first version of this brief gave `transport_mix` a prose description
where every other table got its columns, because I extracted the contracts with a
line-based search and this one wraps onto a second line. The 03_quantumespresso
agent consequently reverse-engineered 8 of its 10 columns out of case 21's tests
and then, needing "did this leave the node", fell back on matching `tl_name`
against `'%mlx5%'` — binding its headline claim to UCX's transport namespace when
the table already answers the question directly.

`scope` takes exactly three values: **`intra-node`**, **`network`**, `unknown`.
They are assigned from `tl_name`: `self`, `sysv`, `posix`, `mm`, `cma`, `knem`,
`xpmem`, `cuda_ipc`, `cuda_copy`, `gdr_copy`, `rocm_ipc`, `rocm_copy`,
`rocm_gdr` are intra-node; `rc_verbs`, `rc_mlx5`, `dc_mlx5`, `ud_verbs`,
`ud_mlx5`, `ib`, `srd`, `tcp` are network; anything else is `unknown`. If you
are asking whether traffic left the node, group by `scope` — do not
pattern-match transport names.

`gdr_copy` was added 2026-08-31 after the 01_gromacs round-2 run put 9,348
operations into `unknown` — the first time this list was caught missing a name
on real hardware. **Treat `unknown` as a finding worth reporting, not as
noise**: it means the tool met a transport nobody has classified, and the row
count tells you how much of your traffic is unaccounted for.

`comm_edges.scope` — ADDED 2026-09-01, and it is why the round-2 studies were
worth running. `comm_edges` looks like the fabric matrix and was not one: three
unrelated applications put intra-node UCX traffic into it indistinguishable
from traffic that crossed the wire (OSU 2 operations, GROMACS 83,892, Quantum
ESPRESSO 1,435,037). On OSU the whole table was four rows and two of them —
`0->0` and `1->1`, the MPI self endpoint — never touched a network.

It takes the same three values as `transport_mix.scope` and is derived
differently, which is the point. `transport_mix.scope` is a claim about the
TRANSPORT UCX chose; `comm_edges.scope` is a claim about the two RANKS — same
node or not, from `node_identity`. So it is available on every lane (verbs,
nvlink, ucx) and needs no join between the UCP and UCT layers, which is the
join that does not exist. `unknown` means the peer rank or a node identity
could not be resolved, and is never folded into `network`.

If you previously wrote `WHERE transport = 'ucx'` and read the result as fabric
traffic, that is the query this column fixes.

`transport_mix`'s **four byte columns are four different quantities and you must
not add them** — ADDED 2026-08-30, after the 02_gromacs agent ranked "`in_bytes`
has no published meaning" as the gap most worth closing, on the grounds that its
headline fraction was arithmetic over a column whose direction it had to guess.
It guessed right, and should not have had to:

| column | which UCT ops | what it means |
|---|---|---|
| `out_bytes` | `Am*`, `Put*`, `TagEager*` | payload that **left this node** during the call |
| `in_bytes` | `GetShort/Bcopy/Zcopy` | payload this rank **requested**; it arrives later |
| `advertised_bytes` | `TagRndvZcopy` | payload **offered** for the peer to pull — only `header_bytes` actually went out |
| `header_bytes` | all | protocol header, every op |

The three payload columns partition the ops between them, so no byte is counted
in two of them. Crucially **`in_bytes` is recorded once, at the rank that issued
the Get** — it is not the far end's copy of the same transfer, so a
one-directional fraction like `out/(out+in)` has an honest denominator.

What you must not do is fold `advertised_bytes` in: a rendezvous advertisement
would count as a transfer, and then the peer's `Get*` counts the same buffer
again in the opposite direction. That is the whole reason these are four columns
and not one.

`link_health` is a **fixed rubric, not an empty table** — CORRECTED 2026-08-29
after the 04_llmc agent measured it. It emits one row per finding KIND with a
count, so a healthy capture has every row present with `n = 0`. An earlier
version of this brief said the table was expected to be empty; that would have
made a clean capture look like it had a dozen defects, and it came from the
table's own head comment in the comshu source, which contradicted the SQL
beneath it.

**`severity` — ADDED 2026-09-02, and the second correction to this paragraph.**
The query above used to be `WHERE n > 0`. It is now

```sql
SELECT finding, n, note FROM link_health WHERE n > 0 AND severity = 'defect';
```

because `WHERE n > 0` alone was still wrong, in the opposite direction from the
first mistake. Three rows in the rubric count things that are **not faults**:
a queue pair's own declaration that it does not signal every work request
(`wr_flight_selective_signalling_qps`, `wr_flight_unretired_selective`), and
kernels timed at submission rather than execution
(`kernel_rows_submission_only`). The 06_llm_scaleout study filed three findings
against a capture whose every genuine defect row was zero, and it was right to
under the contract it was given. `severity` is `'defect'` or `'note'`; a
non-zero `'note'` is the capture explaining itself and must not be reported as a
problem.

**Look for `uct_ops_patch_refused` whenever `transport_mix` comes back empty.**
That row (added at the same time) says `ucx_deep` declined to instrument an
un-verified UCX version, so the UCT layer was never captured for those ranks.
Before it existed the only evidence was a line on stderr, and the 02_lammps
study spent two days treating a correct refusal as a tool defect while its
central prediction went unanswered — because nothing distinguished "the answer
was withheld" from "the workload sent nothing".

**`nccl_profiler_door_shut` is the same row for NCCL, and reads the same way.**
Non-zero means the profiler plugin did not load, so `nccl_execution` is empty
because nobody asked what algorithm ran — not because the workload chose none.
It is classified `defect` for exactly the reason above: a withheld answer that
looks like a negative one is the failure this rubric exists to prevent.

## What is new since round 1, and therefore untested

Round 1's studies were judged against `comm_edges` and the raw UCX tables. Since
then the derived layer has roughly tripled, and **none of the following has ever
been pointed at an application we did not write**:

- the entire `ucx_deep` transport layer (interfaces, lanes, wire operations)
- `transport_mix` — which transport actually carried a message, as opposed to
  which rank sent it
- `tag_flow` — what a UCP send became on the wire, joined on the tag
- `tag_recv_resolved` — the tag and true delivered length of a receive that
  arrived by message handle
- `ucx_rkey` / `ucx_rkey_pack` — remote keys for one-sided operations

Prefer questions that exercise these. "Does `comm_edges` show the right rank
matrix" was answered in round 1; "does `transport_mix` correctly distinguish
shared-memory traffic from fabric traffic on a real decomposition" was not.

## Method, and it is not negotiable

**Write `PREDICTION.md` and commit it BEFORE the first capture exists.** Round 1
did this and it is the only reason its agreement means anything: derive from the
application's source, its input deck and its own log what the capture must
contain, commit that, and only then run. A prediction written after the fact is
not a prediction, and a reader cannot tell the two apart afterwards.

Then one of the four claim types in `README.md`, with evidence. "It ran, N rows
captured" is not a finding — N rows of *what*, and how do you know N is right?

## Machines, and the state of them right now

| | MN5 (`koc296797@glogin1.bsc.es`) | du04 (ssh alias `a100`) |
|---|---|---|
| GPUs | 4x H100/node, multi-node NDR IB | 4x A100, single node |
| queue | SLURM `acc_debug`, **ONE JOB AT A TIME, ACCOUNT-WIDE** | none, run directly |
| apps | GROMACS/LAMMPS/QE as modules | nothing preinstalled |
| repo | `~/proj/comshu` | `/DATA/missa18/comshu` (NOT `$HOME`, it is at 98%) |

**Reach MN5 through `glogin1`, not `alogin`** — CORRECTED 2026-08-30. The ACC
login nodes (`alogin1`, `alogin2`) have been refusing connections on 22/443/80
since 2026-08-29 ~17:00; `glogin1`/`glogin2` answer normally and see the same
GPFS home, the same Slurm controller and the same `acc` partition. There is no
separate ACC cluster to be locked out of.

And `acc_debug` is a **QoS, not a partition**. Submit with
`--account=etur02 --qos=acc_debug` and no `-p` at all; the controller places the
job in partition `acc` itself. `sinfo`/`scontrol show node` return
`Access/permission denied` from the GPP login nodes, which looks like a
permissions wall and is not one — `sbatch --test-only` and `squeue` both work.
Every `tests/hpc/cases/*/job.sbatch` already uses this exact form.

Confirmed module names from round 1: `gromacs/2025.2-gcc-nvhpcx`,
`lammps/10Sep2025`, `quantumespresso/7.5-nvidia-nvhpcx`, plus
`nvidia-hpc-sdk/25.7` and `gcc/13.2.0` for the runtime.

**MN5 is reachable again as of 2026-08-30 via `glogin1`**, with an empty
`acc_debug` queue and immediate start for 1 and 4 nodes. The earlier outage was
`alogin` only. Still do every piece of offline work first — that is most of the
study, and the single slot is handed out by the coordinator, not taken.

The local machine you are running on is **FreeBSD and cannot build or run
comshu at all**. Do not try. Builds and runs happen on du04 or MN5.

## Deliverable for this phase

Because the queue serialises every agent through one slot, this phase stops
before submission unless your app runs on du04. Produce, in your own directory
`tests/hpc/apps/<NN>_<app>/`:

1. `PREDICTION.md` — the oracle, derived from the application, committed first.
2. `asserts/*.sql` — empty result means pass, as in `tests/hpc/cases/*/asserts/`.
   Each one must be falsifiable: say what defect it would catch.
3. `job.sbatch` — bare run first, then the comshu run, in ONE allocation. A
   capture of a workload that did not work is indistinguishable from a tracer
   that captured nothing, and that confusion has cost this project several slots.
4. `QUESTIONS.md` — everything the capture and its documentation could not tell
   you. This is a deliverable, not an apology.

Then report and stop. Do not submit to MN5 without being told the slot is yours.

## Allowed reading

- this file and `README.md`
- `libcomshu/schema/comshu.fbs` — the record schema, and the comments in it
- `libcomshu/schema/README.md`
- `demos/entity-map.html` and `demos/figures/*.svg` — what the records mean
- `tests/hpc/cases/*/asserts/*.sql` — the assertion style to copy
- `tests/hpc/suite/README.md`
- your own application's source, input decks and logs
- `comshu <command>` with no arguments, for that command's usage
