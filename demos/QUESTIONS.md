# Questions you will get, and the answers with the evidence

Read on the way. Every claim here is checked against the tree at `7a52e58` —
file and line where it matters, so you can say "it's in `X.hpp`" and mean it.

Ordered roughly by how likely you are to be asked. The last section is the
things you should **not** claim.

---

## 1. Where does DuckDB come in? Is the capture a database?

**No. The capture is flat files; DuckDB is a reader, and an optional one.**

A capture is per-rank directories of `.pcs` streams — one file per record type,
a header naming the type and schema hash, then fixed-stride records. Nothing in
the capture path links DuckDB, and nothing in the *record* path knows it exists.

DuckDB appears in exactly three places, all on the reading side:

| where | what for |
|---|---|
| `pcomm-read/capture_load.hpp` | mount a capture as tables in memory |
| `pcomm-read/link_sql.hpp` | the derived layer — `comm_edges`, `mem_owner`, `link_health` |
| `pcomm-ui/main.cpp` | the `/query` endpoint behind the panels |

It is behind a CMake flag that defaults **off** (`PCOMM_DUCKDB`, `CMakeLists.txt:32`).
Build without it and you still get capture, `read`, `dump`, and `export --csv`;
you lose `link`, `sql`, `--parquet` and `ui`. That is the point of the split: a
compute node needs none of it.

> Follow-up you should expect: *"so why DuckDB and not SQLite?"* — the derived
> layer is analytic (group-bys over millions of sends), it reads Parquet
> natively so an export needs no load step, and it embeds as one library. The
> demo's own `comm_edges` queries run in sqlite3 precisely because nothing about
> the exported CSV depends on the engine.

---

## 2. What if two providers need the same fact?

**They are forbidden from asking each other. The shared fact moves *below* both,
into core.**

This is a written rule (`PLAN-20260806-1329`) and the reasoning is in
`libpcomm/include/pcomm/core/object_registry.hpp`, which is worth reading before
the talk because it is the cleanest statement of the architecture you have.

The worked example: **memory objects**. gpumem needs a buffer's extent at drain
time to attribute an access. ibverbs and mlx5dv see registrations. If "what did
the program allocate" lived in gpumem, then answering it would cost you NVBit —
measured at ~435 s of module lifting on case 05 — just to learn a buffer's name.
And a sibling "objects provider" would reintroduce exactly the coupling the rule
forbids, because gpumem would have to ask it a question at runtime.

So `ObjectRegistry` sits in core, below every provider. The engine interposes the
allocator doors itself (`nvshmem_malloc`, `nvshmem_calloc`, `nvshmem_align`,
`nvshmem_free` — `libpcomm/src/engine.cpp:637`).

**The measurement that justifies it**, and it is a good one to quote: on one
NVSHMEM workload captured with mlx5dv alone, the capture holds 20 `mem_object`
rows of 8, 40 and 327680 bytes — NVSHMEM's own control structures and work
queues, because NVSHMEM registers what *it* hands the NIC — and the two 4096-byte
buffers **the application actually allocated are absent**. With gpumem they are
there. One end of the join was simply missing.

> **The distinction to hold onto if pressed:** core does *not* own every
> allocation sighting. It owns allocator APIs that hand back an address.
> Providers keep emitting what only they can see. Dedup is automatic because
> everyone interns the base in `InternDomain::Pointer` and therefore mints the
> same id for the same buffer.

---

## 3. What *is* the engine, and where does it stop?

`libpcomm_engine.so`. One shared object, preloaded first in every selection,
holding exactly the things there must be **one** of:

- the `Tracer` singleton and its `Recorder`
- the thread-buffer registry
- the intern tables (so two providers naming one pointer mint one id)
- the id tracker
- the `dlsym` interposer
- `ObjectRegistry`, and the QP registry

**Its boundary is a hard rule: no vendor header, ever.** No `cuda*`, no `ibv_*`,
no `Sanitizer_*` anywhere in L0–L2. The build enforces it. Where the engine must
touch a vendor object it does so through `void*` and never dereferences —
`ibv_destroy_qp` takes a `void*` that is "ABI-identical to `ibv_qp*` and never
dereferenced, so the engine stays vendor-header-free" (`engine.cpp`).

**Why it exists at all is a real bug story**, and it is your best answer to "isn't
that over-engineering":

> All of this used to be function-local statics in headers. Every provider `.so`
> therefore carried its own copy with weak linkage, and the dynamic linker bound
> all references to whichever loaded first. So two providers shared a `Tracer`
> **by accident**, and destroyed each other's `Recorder` on the way up. We blamed
> NVBit for fighting over `dlsym` and marked gpumem exclusive. It wasn't NVBit —
> NVBit only *imports* `dlsym`. Defining them in exactly one shared object made
> the sharing deliberate, and gpumem stopped needing its own pass. Commit
> `7a5b55a`.

That is also why the singletons are deliberately **leaked**: backends flush from
exit-time vendor callbacks (NVBit's `atexit`, CUPTI's) that can run *after* C++
static destructors, so a destroyed singleton would be a use-after-free on the way
out.

---

## 4. Object tracking — inside the providers or outside?

**Outside, in core** — see §2. But the honest full answer is *both*, and the
split is principled:

- **Core** owns allocator doors that return an address. It is below everything,
  so any provider can attribute against it without asking anyone.
- **Providers** emit what only they can see — an `ibv_reg_mr` registration is a
  fact about the NIC, not about the allocator, and it belongs to ibverbs.

They do not collide because both intern the base pointer in the same domain and
get the same handle. `pcomm-read`'s `mem_owner` resolves an address to its
containing object at read time.

The same shape appears twice more, and pointing that out makes it look like a
pattern rather than a special case:

- `core/mem_attr.hpp`'s `device_of` — same reasoning.
- **QP teardown is owned by the engine, not a provider.** A QP created through
  `ibv_create_qp` and one created through `mlx5dv_create_qp` are *both* destroyed
  through `ibv_destroy_qp`. Two providers coexist by design, and interposition
  wants exactly one owner of a symbol — so no provider can own that door and
  account both. The engine can, because it is always loaded first. Before this,
  a `-p ibgda`-only run left every queue pair opened and never closed, since the
  old owner (ibverbs) wasn't in the process at all.

---

## 5. IBGDA vs ibverbs — aren't they the same thing?

**No, and a capture proves it: trace an IBGDA run with `-p ib` and you get empty
tables and no error.** That is the sentence to lead with.

They are separated by **who initiates**, not by what the hardware does:

| | `ib` | `ibgda` |
|---|---|---|
| who starts the transfer | the **CPU** posts a work request | the **GPU** rings the doorbell |
| the door | `ibv_post_send` and friends | DEVX / `mlx5dv`, direct |
| what pcomm hooks | libibverbs, via `LD_PRELOAD` | the driver's export table, doorbell pages |

IBGDA never calls `ibv_create_qp`, `ibv_modify_qp` or `ibv_post_send`. It drives
DEVX directly. So a GPU-initiated run traced with `-p ib` is not *wrong*, it is
**silent** — which is worse, and is exactly why they are two providers rather
than one with a flag. They co-run happily; a workload using both verbs and DEVX
needs both. (The reasoning is in the comment above the registry entry,
`pcomm/run.cpp`.)

> If someone asks why the `.so` is still called `libmlx5dv_trace.so` when the
> provider is named `ibgda`: the library contains the mechanism, the provider is
> named for the thing being captured. `mlx5dv`/`devx`/`mlx5` are all aliases.

---

## 6. Why is an `ib_post_send` an interval? What is it the duration *of*?

**It is the duration of the `ibv_post_send` call**, from an rdtsc stamp taken by
the hook engine before the call to one after — same clock domain as the recorder
and the clock anchors (`providers/ibverbs/ibverbs_provider.cpp:149`). The comment
there says why: "so post_send/poll_cq render as slices with real durations
instead of instants."

**Be precise about what that does *not* mean.** It is the cost of *handing the
work request to the NIC*, not the time the bytes spend on the wire. The wire time
is bounded by the matching completion, which is a separate record (`ib_poll_cq`),
and pairing them is a derived-layer job.

That pairing is worth knowing because it has a real trap in it:

> NVSHMEM posts 5.8 M work requests with `wr_id = 1` — all of them. So joining a
> send to its completion on `(rank, qp_num, wr_id)` is a cartesian product with a
> million rows on each side, not a lookup. The join key is
> `(rank, qp_num, wr_id, temporal_a_ticks)`, and **the ticks are not optional**.
> The interval is what makes the join possible at all.

And the derived layer refuses to pair a queue pair it cannot certify: without
`sq_sig_all` you can only count, and "posted 3,570, completed 1,800" is either
selective signalling or a capture that stopped mid-flight — counting cannot tell
those apart. `ib_qp_signalling` is one row per queue pair that turns "refused,
cause unknown" into "refused, and here is why".

---

## 7. So what *is* an Instant then? And a Window?

The five kinds are in `libpcomm/include/pcomm/core/time.hpp:13`. Real producers
in the tree today:

| kind | real example | why |
|---|---|---|
| `None` | `source_line` | a PC → source mapping computed once, at instrumentation time |
| `Instant` | `entity_close` | the free happened at a point; nothing spans |
| `Interval` | `ib_post_send`, `ib_poll_cq` | [call-start, call-end], both known |
| `Window` | `gpu_access` | we know it happened *somewhere inside* a span |
| `Counter` | **none yet** | the kind exists; nothing produces one |

**`Window` is the one worth defending, and the number makes it vivid.** A
`gpu_access` carries a window of [launch enqueue, host drain] in host TSC —
sound, but measured at **1.94 s wide against a 48 µs kernel** on
`hubspoke-44713195`, because "drain" is in practice the end-of-run flush.

The obvious question is "so why not use the kernel's own `%globaltimer` span,
which is three orders tighter?" The answer is that it **cannot be resolved when
the record is written**: `t1` is a max over block exits that closes only when the
last block retires, and `t0` a min over entries. Reading either at drain time
could yield a window that *excludes* the true access time — which is worse than
a wide one. So the producer records a join key (`launch`) and the reader does the
join, where the whole capture is available and the ordering problem doesn't
exist. `_tl_raw` in `link_sql.hpp` narrows to the span's window when one matches
and keeps the TSC window when none does.

> That is the whole argument for `Window` as a first-class kind in one example:
> the alternative to an honest wide interval is a precise wrong one.

---

## 8. "You said a star — but the matrix shows spokes talking to each other"

**They do, and none of it is application data.** You can say that as a fact, not
a hunch, because the accounting closes exactly.

Split this capture's 134 edges by whether rank 0 is on them:

| | edges | sends | bytes |
|---|---|---|---|
| touches the hub | 30 | 207,636 | 842,208 |
| spoke ↔ spoke | 104 | 1,698 | 59,424 |

Now split the same run by opcode:

| opcode | len | n | bytes |
|---|---|---|---|
| `rdma_write` | 4 | 207,360 | 829,440 |
| `send` | 4 | 960 | 3,840 |
| `send` | 64 | 960 | 61,440 |
| `rdma_write` | 128 | 54 | 6,912 |

207,360 is exactly the application: 15 spokes × 12,288 puts + the hub's
15 × 1,536 replies. Everything else — 1,974 sends, 72,192 bytes — is NVSHMEM.
And it lands where you'd expect:

```
hub edges  − application  =    276 sends,  12,768 bytes   (control on hub edges)
spoke ↔ spoke             =  1,698 sends,  59,424 bytes   (control, all of it)
                    total =  1,974 sends,  72,192 bytes   = all control, exactly
```

**So every one of the 207,360 application writes is on a hub edge, and the
spoke↔spoke mesh carries zero of them.** The star is real; what you are also
seeing is NVSHMEM's bootstrap and synchronisation, which is near all-to-all by
construction — 104 of the 210 possible spoke pairs here.

> **This is why case 16's assertion is threshold-free**, and it's worth saying:
> "is there an edge between these ranks" is true for almost every pair, so it is
> the wrong predicate. The assertion instead takes exactly `2*(ranks-1)` heaviest
> edges and requires *that set* to be the star. No byte threshold to tune, and
> no assumption about how chatty a future NVSHMEM is.

And to answer the sub-question directly: the hub **both** receives and sends.
Fifteen spokes push a full slot in; the hub answers each with a slot/8. That's
the 8× asymmetry — the column into rank 0 is heavy, the row out of it is light.

---

## 9. "The Objects view has objects with no owner, touched by many ranks — where do they live?"

Two different things are being noticed, and they have different answers.

### The ones with no device

`device` is `65535` = `0xFFFF` = `kUnknownDevice`, a sentinel, not a device that
exists. In this capture:

| device | rows | distinct bases | sizes |
|---|---|---|---|
| 0, 1, 2, 3 | 80 | 5 each | 2 KiB – 1 GiB |
| **65535** | **2,027** | 1,638 | 8 B – 38 MB |

All of them come from `ib_mem_region` — `ibv_reg_mr` registrations. Note that 32
registrations *did* resolve to a GPU (8 per device), so this is not the
attribution being broken; it is 1,664 registrations that are not device memory.

**Say "not a GPU allocation", not "host memory".** The distinction is real and
`core/mem_attr.hpp` states it outright:

> `kUnknownDevice` is ambiguous by construction: it means "host memory" *and*
> "CUDA is not present" *and* "the call failed". Those three imply completely
> different work and look identical in the capture.

On this run CUDA is obviously present, so host memory is by far the likeliest
reading — NVSHMEM's own bounce buffers, control structures and work queues. But
the capture does not *prove* it, and the field is a sentinel rather than a value
called `host` precisely so it never asserts what it doesn't know. Same discipline
as `Window`.

> **The bug that ambiguity hid**, if you want to make the point stick: the first
> version of this shipped with `kAttrDeviceOrdinal = 4`, which is actually
> `CU_POINTER_ATTRIBUTE_HOST_POINTER`, not `DEVICE_ORDINAL`. It survived the
> provider's entire life, because every failure mode of the function looks
> identical from the capture. `PCOMM_TRACE_HOOKS=1` prints the first attribution
> attempt and is the only thing that tells the three apart — which is why it
> stays in the tree.

> And the phenomenon behind *which* objects these are is what
> `object_registry.hpp` was written about: captured with a network provider alone
> you get NVSHMEM's internals and *not* the application's buffers, because
> NVSHMEM registers what it hands the NIC.

### "Then why don't you report which host it belongs to?"

**It does.** That fact is in the capture and in the UI already — you saw it in
the demo, where the agent said *"rank 0 = `n3 · GPU 0`"*. It just isn't in the
`device` column, because `device` answers a different question.

- `node_identity` (type 524) is emitted **once per rank by the engine at start** —
  "every provider needs it and none owns it", the same reasoning that put
  `MemObject` in core. It carries FNV-1a of the hostname; this capture has 16
  rows resolving to 4 distinct nodes.
- `rank_identity` in `link_sql.hpp:977` turns that into the label you see:
  `'n' || dense_rank() over (order by node) || ' · GPU ' || device`.
- Every `mem_object` row carries `rank`, so the owning host is one join away.

So: **`device` means "which GPU", `rank`→`node_identity` means "which machine".**
Collapsing them would break exactly the case being asked about — a host buffer
has no GPU but definitely has a host, and writing `device = 0` for it would be a
lie in the column people aggregate on.

> Expect one more: *"why hash the hostname instead of storing it?"* — because the
> id only has to be **comparable between ranks of one job**, and putting a machine
> name on disk in every capture is a cost with no matching benefit. 64 bits of
> FNV-1a makes a within-job collision not worth defending against. It's in the
> comment above the record.

### The ones on many ranks: the symmetric heap

Five bases appear on **all 16 ranks**, at the identical virtual address:

```
0x10020000000   512 MiB      0x1004b005000   256 KiB
0x10040000000     1 GiB      0x1004b045000    16 KiB   <- 4096 elems x 4 B
                             0x1004b049000     2 KiB   <- the slot/8 reply
```

That is not one object being touched by sixteen ranks. **`nvshmem_malloc` is
collective and returns the same address on every PE** — that is the whole point
of a symmetric heap, and it is what lets a PE compute a peer's address without
being told it. So one base is sixteen distinct physical buffers that share a
name, on four different devices (0–3, whichever GPU each PE owns).

The capture keys objects by `(rank, base)`, which is why those five bases are
**80 rows** rather than 5. If it keyed on base alone, the symmetric heap would
collapse sixteen buffers into one and every per-rank number would be wrong.

> Nice detail to land: the 16 KiB one is the application's buffer — 4096 elements
> × 4 bytes, straight off the command line — and the 2 KiB one is the slot/8 the
> hub replies with. You can point at the demo's arguments and then at the
> Objects panel and they match.

---

## 10. How do providers share a process? What about NVBit?

All six co-run in one process, one recorder, one timeline. **Nothing is marked
exclusive today.**

`replay` still exists for a provider that genuinely must own the runtime: pcomm
re-runs the workload once per exclusive provider and drops each pass under one
`-o` directory, wall-clock aligned, stitched back by rank on read (each provider
owns a disjoint record-id range, so the merge is clean). It is the fallback, not
the default, and the reason matters: **replay's passes are different executions,
so they cannot carry cross-transport causality** — "this GPU peer-write caused
that IB send" is unanswerable across two runs.

There is one hard conflict that `exclusive` cannot express, and it is a good
detail: **CUPTI permits a single subscriber per process, and NVBit dlopens
libcupti and takes it.** That is a conflict with *one other provider*, not with
sharing the process — marking cupti exclusive would pointlessly replay the
workload just to co-run with ibverbs, which it does happily. So it is a specific
check in `build_passes()` instead.

---

## 11. What's the overhead?

Say what you measured and no more:

- SigmaTrace measured the completion stream (`poll_cq`, `recv_cqe`) as the
  difference between **<1.5% and 8–13%** overhead; `-m simple` drops it and costs
  you latency work, receive attribution and the CQE probe. That's ~45% of a
  capture's rows.
- NVBit's cost is **fixed, not proportional**: case 05 takes ~435 s regardless of
  message size, measured at N=100 through 262144 — a 2621× range — while the
  capture grows 0.6 MB → 47 MB. Uninstrumented is 6 s. It is module lifting: 223
  device-function specialisations, 434k instructions. Restricting to the single
  launched kernel does not help (438 s vs 436 s).
- Today's demo: 84 MB and 209,334 sends for a 3-iteration toy.

**Do not** claim an end-to-end overhead number for the whole tool. You don't have
one.

---

## 12. Why should I believe any of it?

The suite: 20 cases, expected numbers computed **on paper from the workload's
arguments before the run**. Four GPUs, two iterations, 1024 threads → 8192 peer
stores, 2048 per ring edge. If the capture disagrees, the capture is wrong.

The two properties worth naming:

- **It is the merge gate.** A change to capture semantics isn't done until the
  board is green, and it refuses a dirty tree, so every board is attributable to
  a commit.
- **The assertions are threshold-free where they can be.** Case 16 doesn't ask
  "is there an edge between these ranks" — NVSHMEM's bootstrap is a near
  all-to-all control mesh (43 control edges carrying 644 sends beside 2 data
  edges carrying 393,240 on case 07), so that predicate is true for almost every
  pair. Instead: take exactly `2*(ranks-1)` heaviest edges and require **that
  set** to be the star. No byte threshold to tune, and no assumption about how
  chatty a future NVSHMEM is.

Then `BUGS-FOUND.md`: defects in *other people's* software, each with the
evidence that identified it and the explanations that were excluded. There is
one entry today, and it is the story to tell — rehearse it, it is 30 seconds and
it is the most convincing thing in the talk:

> **HPC-X's hcoll leaks two InfiniBand memory regions per node.** What fired was
> `entity_close_accounted` — a suite assertion that every entity recorded as
> opened is also recorded as closed. On a 64-rank capture, 32 unclosed
> `ib_mem_region`: exactly two on each of the 16 ranks with `rank % 4 == 0`, and
> zero on the other 48. Local rank 0 of every node. One 40-byte and one 8 MiB
> host region, same PD, `access=3`, origin `reg_mr`.
>
> Nothing else in that capture was abnormal — the 126-edge star resolved
> exactly, zero unresolved peers, zero self-edges across 16 LIDs.
>
> Naming the owner needed one more thing: pcomm `dladdr`s
> `__builtin_return_address(0)` inside each registration interposer, but keyed on
> `(door, library)` it couldn't answer — `ibv_reg_mr` has two callers in that
> workload. Extending the key with the region length separated them:
> `hmca_mcast_vmc.so` registers 20 MiB and deregisters 20 MiB (balanced), and
> registers 8 MiB with no deregistration ever observed.

Two things to draw out of it, because they are the point: the assertion that
fired is **structural** — it knows nothing about hcoll, it only knows opens must
match closes — and the capture said *nothing else* was wrong, which is what let
the finding be specific rather than "something looks off".

---

## 13. Things you should NOT claim

Have these ready as honest answers rather than getting caught:

- **`-p kernel` will not run on MN5 right now.** `pcomm list` advertises it, but
  it builds from `gpu-backends/cupti/` (not `providers/`) and `libcupti_trace.so`
  is **not** in `~/.local/lib/pcomm/` — the installed set is gpumem, ibverbs,
  mlx5dv, nccl, ucx and the engine. If someone says "run that one", say it isn't
  installed in this tree. Don't discover it on stage.
- **`Counter` has no producer.** The temporal kind exists; nothing emits one. Say
  "the model has it, we haven't needed it yet."
- **`nccl_window` / `nccl_dev_comm` are empty** — that's the device-initiated
  NCCL path, which needs a kernel using the NCCL device API. The tables are
  *absent* rather than empty in case 20's capture, which is the honest state.
- **The p2p/gpumem provider has an open illegal-memory-access bug** under some
  workloads (`PLAN-20260810-0854`), which is why cases 16–18 deliberately don't
  load it. Don't run `-p gpumem` live on something you haven't tested.
- **`pcomm ui` currently drops `nccl_comm`** — `gen_raw_columns.py:143` injects a
  `rank` column and `NcclComm` has its own field called `rank`, so DuckDB refuses
  the duplicate and the table vanishes silently. The CSV path already renames it.
  Known, small, not yet fixed.
- **ucTrace is not folded in yet.** `ucx` is a provider; ucTrace's coverage is
  phase 2.
- **No hosted CI.** The gates are local and named. Say that plainly — it's a
  deliberate choice for a repo mirrored between machines, not an oversight.

---

## 14. Two questions you want to be asked

Steer toward these if you get an opening.

**"Why four archetypes and not just events?"** Because Entity/Event/Artifact/Edge
is what every profiler re-derives under four different names, and naming it once
is what makes the reader generic. A new provider gets relational tables, SQL, a
Perfetto timeline and the UI without anyone downstream writing a line for it.
That is the payoff, and it is measurable: adding a provider is one `.so`, a
record-id range, and a registry row.

**"Why is the column called `logical_bytes`?"** Because it is count × the
datatype width — what the application handed over — and **not** what crossed the
wire. A ring all-reduce moves about 2(n−1)/n of it per rank; a broadcast moves it
once. The first person to call it `bytes` would put it on a bandwidth axis and
invent a measurement. Naming is how a schema refuses to lie, and that is a
one-sentence version of the whole design.
