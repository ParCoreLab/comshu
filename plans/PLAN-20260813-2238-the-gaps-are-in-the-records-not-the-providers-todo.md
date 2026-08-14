# The gaps are in the records, not the providers

A coverage diff against SigmaTrace (`SigmaTrace-playground` @ `8d8141a`) turned
up five things it records that we do not. Two of them are entire providers —
libfabric and UCX — and they look like the headline. They are not, and this plan
is mostly about why, because getting that order wrong costs weeks.

---

## The finding that sets the order

**We already capture every verbs operation, whoever called it.** libfabric and
UCX both terminate on verbs queue pairs, so their traffic is *already in the
corpus* as ordinary `ib_post_send` / `ib_poll_cq` rows. A provider for either
does not add traffic. It adds **attribution** — the knowledge that a given
`post_send` came from `ucp_tag_send` rather than from NCCL's net plugin. That is
exactly what SigmaTrace's `UcpEpQpMapping` and `FiEndpointQpMapping` exist to
carry, and it is the whole value on offer.

Which is worth having only if the workloads route through those layers. Ours
mostly do not:

| workload | path to the wire | would a UCX/libfabric provider see it? |
|---|---|---|
| NVSHMEM | IBGDA / verbs direct | no — `mlx5dv` + DEVX already covers it |
| NCCL | own net plugin → verbs | no |
| MPI (`mpirun`, nvidia-hpc-sdk) | OpenMPI → UCX → verbs | yes, for whatever traffic it actually carries |

Measured on `scale_hubspoke_ib_kt-44552862`: 5,806,314 `IBV_WR_RDMA_WRITE` on
196 queue pairs against 24,576 `IBV_WR_SEND` on 475, with 123 carrying both. The
two-sided path is real but small, and nothing in the capture said whether it was
MPI — so it was measured rather than argued.

### SETTLED, by `tests/hpc/probes/ucxcheck` (job 44578355)

`/proc/self/maps` read from inside a real MPI rank, after an `MPI_Allreduce`, on
two nodes:

```
libucp  mapped : YES      libfabric  : no
libucs  mapped : YES      libibverbs : YES
libuct  mapped : YES      libmlx5    : YES
```

**UCX is on the MPI path.** `libfabric` is not merely unused — it is not
installed on MN5 at all (`ldconfig`, the HPC SDK's `comm_libs`, and
`/usr/lib64` all have nothing).

So the two providers are not one decision:

- **UCX — worth building.** MPI traffic really does route through it, so a
  provider would attribute a real share of the verbs rows to `ucp_*` calls.
  This is Stage B and it stays.
- **libfabric — closed, not scheduled.** A provider for a transport the machine
  does not have would attribute nothing, forever. Stage C is deleted; reopen it
  only if the corpus moves to a machine that ships libfabric.

`ldd` was not enough to answer this and neither was the module list: OpenMPI
selects its PML at runtime and dlopens it, so the launched binary's `DT_NEEDED`
says nothing. Only the mapped-library test settles it. (Three attempts: the
probe first failed on `ompi_info`/`mpirun` not being on PATH, then on the SDK's
`mpicc` wrapper demanding `nvc`, then on building into node-local `/tmp` so the
second rank could not exec it.)

The other three gaps need no such argument. They degrade data we already have.

---

## Stage A — the record gaps

Three additions, all additive, all validated by **one** MN5 run. This is the
whole of the near-term work.

### A1. `send_flags`, and why it is not optional

A completion can be matched to its send because a reliable-connected queue pair
returns completions **in posting order** — a transport guarantee, not a
heuristic. That is why a constant `wr_id` costs nothing: NVSHMEM posts all
5.8 M work requests with `wr_id = 1` and the pairing is still exact.

It holds only while every work request is signalled. An unsignalled WR produces
no CQE and is retired silently by the next completion, so Nth-to-Nth matching
attributes a completion to a send that did not produce it — and prints a latency
for it.

Measured:

| capture | sends | send-side completions | pairing |
|---|---|---|---|
| `scale_hubspoke_ib_kt-44552862` | 5,830,890 | 5,830,890 on all 862 QPs | exact |
| `kt-combined-44546852` | 3,570 per QP | 1,800 per QP | **wrong** |

`kt-combined` was producing confident, wrong latencies until the guard landed
(`_wr_pairable`, and the `pairable` set in `build_trace`). The guard is the
right behaviour and it is not the fix: it refuses those queue pairs entirely, so
every NCCL-shaped capture now has **no flows and no flight lane at all**.

`send_flags` is the field that ends this. With it the signalled set is known
exactly, and the correct reading follows: a signalled completion retires the run
of work requests posted since the last one, so the latency bounds *the group*
rather than a single send. Drawn that way the flight lane also becomes fewer,
fatter spans — which happens to help the density problem the completion lane
has.

A1 splits in two, and the split is the useful part.

**A1a — the queue pair's policy. DONE.** `sq_sig_all` lives on
`ibv_qp_init_attr`, which both `create_qp` hooks already receive, so it needs no
query and costs one row per queue pair. It is an *additive entity* —
`ib_qp_signalling`, type_id 126 — on exactly the reasoning `IbRecvCqe` records:
widening `ib_queue_pair` would force every reader to carry two layouts of that
table forever, and a version bump for a field this sparse is not worth it. It
does not make selective signalling pairable; it names the cause, turning
"refused, cause unknown" into `wr_flight_selective_signalling_qps`. A queue pair
that declared `sq_sig_all = 0` and completed fewer WRs than it posted *is* doing
selective signalling — no inference, it said so at creation.

**A1b — DONE, inline on `ib_post_send`.** `send_flags` is a per-work-request
field, so it belongs on the per-work-request record. Added in place and
`kSchemaHash` bumped, which is exactly the PRE-ALPHA EXCEPTION `pcomm.fbs`
already describes.

I first built this as a sparse companion record (`ib_post_send_flags`, emitted
only on `sq_sig_all = 0` queue pairs) specifically so that captures already
written would keep working. **That was the wrong trade and the record of it is
kept here deliberately.** Preserving a corpus is not a design argument: pcomm is
pre-alpha, MN5 cycles are plentiful, and re-capturing is cheap next to carrying
a shape that exists only to avoid re-capturing. The inline field is simpler at
every layer — no companion record, no per-QP side table in the provider, no
four-column join in `_wr_signalled`, one fewer thing to keep in step.

Consequences, all intended:

- `sizeof(Stamped<IbPostSend>)` goes 88 -> 96, so **every capture written before
  this is refused** stream-by-stream with `record stride differs`. Loud, by
  design, and not a misparse.
- `tests/data/ib_pingpong-44368973` is a committed fixture and is now stale.
  `flow_pairing_agree` and `schema_python_reader` skip on it, each naming the
  file and the remedy. **Re-capturing it is a required action, not optional** —
  two tests prove nothing until it happens.
- The new-WR-API path reads `ibv_qp_ex::wr_flags` at the opcode call, where it
  already reads `wr_id`, and `WrBuilder` carries it through to `wr_complete`.

`_wr_flight` gained a second branch pairing completions against SIGNALLED sends,
guarded by `signalled = comps`, and disjoint from the first by `sends <> comps`.
That disjointness clause is load-bearing: a queue pair that completes everything
AND marks everything signalled satisfies both branches, and without it every one
of its sends lands in the flight lane twice.

### A2. Per-scatter-gather-entry sends

SigmaTrace's `IbvPostSendSg` records `addr`, `length`, `lkey` for **each** SGE.
We sum the lengths into one `length` and saturate `num_sge` at 255
(`ibverbs_provider.cpp:372`).

The cost is not the arithmetic, it is the addresses. We keep no *local* buffer
address on the send path, so a send can only be attributed to an object through
`remote_addr` — which exists for RDMA ops and is zero for two-sided sends. Every
one of the 24,576 `IBV_WR_SEND` rows above is invisible to `object_access` on
the sending side.

New record, not a version bump: `PCOMM_RECORD(IbPostSendSg, Event, 128,
"ib_post_send_sg")` carrying `(wr_id, qp_num, addr, length, lkey)`, one row per
entry, joined to `ib_post_send` on `(rank, qp_num, wr_id)` — the same key the
flight lane already uses. This is the `IbRecvCqe` pattern: an additive record
beside the one that exists, not an edit to it.

Volume is the open question. One row per SGE against one per WR is at worst a
doubling on a corpus that already writes 5.8 M sends per rank-set, so this
belongs behind `TraceMode::Full` like completions are, and the plan should
measure before committing.

### A3. Port LID

SigmaTrace's `IbvPortRegistration` maps LID → hostname from a port query. We
derive LIDs only from `ib_modify_qp.src_lid`, so a rank that never transitions a
queue pair contributes no LID and falls out of `_node_by_lid`. Additive entity,
`ib_port` at type_id 122, from `ibv_query_port` at device open.

---

## Stage B — the UCX provider (libfabric is closed)

Confirmed above: OpenMPI on MN5 uses UCX. The provider is ~the size of the
existing ibverbs one and lives or dies on the endpoint→QP mapping, which must be
designed in from the first record rather than bolted on — without it the
provider produces a second, parallel timeline that never joins the verbs one.
SigmaTrace's `UcpEpQpMapping` exists for exactly this and is the piece to copy.

Not started. The `libucp` check that gated it has now been done.

---

## Stage A status

| | | |
|---|---|---|
| A1a `ib_qp_signalling` (126) | **done** | `sq_sig_all` at both create_qp hooks; `_wr_pairable` + `wr_flight_selective_signalling_qps` |
| A1b `send_flags` on `ib_post_send` | **done** | inline, stride 88->96; second `_wr_flight` branch pairs on signalled sends |
| A2 `ib_post_send_sg` (128) | **done** | per-SGE `addr`/`length`/`lkey`, Full mode only, joins on `(rank, qp_num, wr_id, ticks)` |
| A3 `ib_port` (122) | **done** | LID from `ibv_query_port` at device open, unioned into `_rank_lid` |

A1a, A2 and A3 are additive. A1b is not: it widens `ib_post_send`, so **the
entire capture corpus must be re-run** — accepted deliberately, see A1b.

**VALIDATED ON MN5.** Every record above now exists in real captures, and the
suite is GREEN end to end under the new schema (4 cases SKIP for host reasons:
no `PeerMappingOverride=1`, no `nvidia_peermem`).

On the regenerated 64-rank `scale_hubspoke_ib_kt-44580935` (1.6 GB, schema
`2bee292be9d1e1c6`):

| | rows | what it says |
|---|---|---|
| `ib_post_send` | 5,830,890 @ stride 96 | all signalled — `send_flags` = INLINE\|SIGNALED |
| `ib_post_send_sg` | 5,830,890 | **414 distinct local buffers**, previously unrecoverable |
| `ib_qp_signalling` | 14,608 | every QP declares `sq_sig_all = 0` |
| `ib_port` | 5,056 | 4,224 ACTIVE, **832 DOWN** |
| `wr_flow` | 5,830,890 | 100% paired, `link_health` reports nothing |

Three of those are findings the corpus could not previously express:

- **NVSHMEM sends inline.** `send_flags = 10` is `IBV_SEND_INLINE|IBV_SEND_SIGNALED`
  on 5,830,656 of the sends. That is why the latencies are microseconds — the
  4-byte payload never leaves the WQE.
- **Every queue pair declares selective signalling and then signals everything.**
  `sq_sig_all = 0` on all 14,608, yet sends and completions match exactly. The
  count branch pairs them; `sq_sig_all` alone would have said "unpairable". This
  is precisely why the guard keys on counts and treats `sq_sig_all` as evidence
  about the CAUSE rather than as the decision.
- **832 ports are DOWN.** Vindicates including non-ACTIVE ports in `_rank_lid`:
  excluding them would have dropped those ranks out of `_node_by_lid` entirely.

Also fixed by real data: `port_state_name` was written off by one
(`ibv_port_state` starts at `NOP = 0`), so MN5's ACTIVE ports printed as
"armed" — a plausible-looking lie that no local test could have caught.

### The corpus job was running from a different tree

Worth recording because it cost a 16-node allocation. `tests/hpc/suite/common.sh`
defaults `PCOMM_ROOT` to `$HOME/proj/pcomm`; this sbatch overrode it to
`$HOME/pcomm-impl`. A run immediately after a sync therefore produced a "fresh"
619 MB capture written by a schema **three bumps behind** — `ib_post_send` at
stride 88 and none of the new records present. The job already refused a stale
*app* binary; it did not check the *tracer*, which is the same defect and the
more expensive one. It now refuses to start unless `PCOMM_BUILD` is inside
`PCOMM_ROOT` and the binary is newer than `stream_header.hpp`.

(The first version of that guard grepped the binary's `strings` for the schema
hash. `kSchemaHash` is a numeric constant, so it matched nothing — that guard
would have blocked every run rather than the stale ones. Verified before
shipping.)

## Already landed

- **The guard** — `_wr_pairable` + `build_trace`, refusing unpairable queue
  pairs rather than mispairing them, with `wr_flight_unsignalled_qps` in
  `link_health`. `tests/wr_flow_rule.sh` fires on all four clauses of the rule.
- **The build gate** — `tools/get_devheaders.sh` extracts rdma-core, libfabric
  and ucx into `.tools/sysroot`. Fixing it exposed that both providers
  `find_library`'d their `.so` while relying on `/usr/include` for the headers;
  they now `find_path`. Both providers compile on simurgh-base for the first
  time, which is the precondition for any of Stage A being written without
  burning a queue slot on a typo.

## Non-goals

- Matching SigmaTrace event-for-event. `IbvPollCq`-the-call is commented out on
  their side and absent on ours; their `CommunicationSummary` is our
  `comm_edges`, computed in the reader instead of the profiler. Neither is a gap.
- Anything that makes an existing capture unreadable. Every item here is a new
  type_id or a new record; nothing edits a layout.
