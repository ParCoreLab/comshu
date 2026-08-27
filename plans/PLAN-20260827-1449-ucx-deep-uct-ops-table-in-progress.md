# PLAN — `ucx_deep`: the UCT ops table is the send door UCX does not export

- **Created:** 2026-08-27 14:49 +03
- **Parent:** `PLAN-20260815-1540-ucx-provider-and-attribution-in-progress.md`,
  *Future work 1*, which concluded that UCX "writes WQEs and rings the UAR
  inline, with no interposable symbol".
- **Correction this plan makes:** that conclusion is right about *symbols* and
  wrong about *doors*. UCX's entire data plane dispatches through a function
  pointer table that lives in a **public installed header**. The door is a
  table, not a symbol, and pcomm already owns the machinery for exactly that
  kind of door.

## Stage 0 — how a tagged send actually reaches the wire

Established from the UCX source (github.com/openucx/ucx, tags v1.10.0 through
`origin/master`) and from the HPC-X 2.22.1 tree installed on MN5. No allocation
was used; the confirming experiment ran on a laptop.

### The dispatch

`uct/api/uct.h` defines every UCT data operation as `UCT_INLINE_API`:

```c
UCT_INLINE_API ucs_status_t uct_ep_am_short(uct_ep_h ep, uint8_t id, uint64_t header,
                                            const void *payload, unsigned length)
{
    return ep->iface->ops.ep_am_short(ep, id, header, payload, length);
}
```

That is why the ELF sweep found nothing: `uct_ep_am_short` is not a symbol in
anybody's PLT. It is a one-line inline over a function pointer.

The pointer lives in **`uct_iface_ops_t`**, declared in **`uct/api/tl.h`** —
which is `#include`d by `uct/api/uct.h` and is **installed**, present in both
the local sysroot (UCX 1.19.0) and MN5's HPC-X tree (UCX 1.18.0). Its own
doc comment states the contract:

> Transport interface operations. Every operation exposed in the API must appear
> in the table below, **to allow creating interface/endpoint with custom
> operations.**

`tl.h` also declares the two structs that reach it, and they are one field each:

```c
typedef struct uct_iface { uct_iface_ops_t ops; } uct_iface_t;   /* ops at offset 0, BY VALUE */
typedef struct uct_ep    { uct_iface_h     iface; } uct_ep_t;
```

So `ep->iface->ops.ep_am_short` is reachable with **zero opaque offsets** — the
compiler computes every offset from a header shipped with the runtime.

### Which slots carry data, for the transports MN5 uses

`uct_rc_mlx5_iface_tl_ops` (`src/uct/ib/mlx5/rc/rc_mlx5_iface.c`) and
`uct_dc_mlx5_iface_tl_ops` (`src/uct/ib/mlx5/dc/dc_mlx5.c`) both fill:

| group | slots |
|---|---|
| active message | `ep_am_short`, `ep_am_short_iov`, `ep_am_bcopy`, `ep_am_zcopy` |
| put | `ep_put_short`, `ep_put_bcopy`, `ep_put_zcopy` |
| get | `ep_get_bcopy`, `ep_get_zcopy` (**no `ep_get_short`** on either mlx5 transport) |
| atomics | `ep_atomic{32,64}_{post,fetch}`, `ep_atomic_cswap{32,64}` |
| HW tag matching (`#if IBV_HW_TM`) | `ep_tag_eager_{short,bcopy,zcopy}`, `ep_tag_rndv_{zcopy,request,cancel}` |

`uct_rc_mlx5_base_ep_am_short` and friends are `static` inside `libuct_ib.so` —
never exported, which is what the door sweep measured. They are one call above
the WQE build and the UAR doorbell write.

Every one of the 21 data ops is called from `src/ucp`:

```
uct_ep_am_short 7   uct_ep_am_bcopy 17   uct_ep_am_zcopy 6   uct_ep_am_short_iov 2
uct_ep_put_short 7  uct_ep_put_bcopy 2   uct_ep_put_zcopy 6
uct_ep_get_short 2  uct_ep_get_bcopy 2   uct_ep_get_zcopy 6
uct_ep_tag_eager_{short 4, bcopy 2, zcopy 3}  uct_ep_tag_rndv_{zcopy 2, request 2}
uct_ep_atomic{32,64}_{post 2, fetch 2}  uct_ep_atomic_cswap{32,64} 2
```

**The dispatch IS through an ops table.** Stage 0's stop condition does not fire.

### The install point

`uct_iface_open` is exported by `libuct.so.0` and is an UND reference in
`libucp.so.0` — so it is an ordinary PLT interposition. It returns the
`uct_iface_h` whose `ops` we need, and its `uct_iface_params_t` carries the
transport and device names in the clear (`mode.device.tl_name` / `dev_name`,
gated by `UCT_IFACE_PARAM_FIELD_DEVICE` + `UCT_IFACE_OPEN_MODE_DEVICE`).
`uct_ep_create`, `uct_ep_destroy` and `uct_iface_close` are exported and
imported the same way.

`uct_iface_t`'s ops table is written **once**, by
`UCS_CLASS_INIT_FUNC(uct_iface_t)` (`src/uct/base/uct_iface.c:624`,
`self->ops = *ops`), and a repository-wide search finds no transport that
rewrites a slot afterwards. A patch installed at open therefore stays installed.

### Confirmed by experiment, on a laptop, in 0.1 s

`uct_probe.c` (disposable, not committed): interpose `uct_iface_open`, save
`(*iface_p)->ops`, overwrite the 16 data slots with counting trampolines, run
`tests/ucx_selftest` unchanged.

```
[uct-probe] iface 0x242c7fd0 tl=self  dev=memory      patched
[uct-probe] iface 0x242c58d0 tl=tcp   dev=lo          patched
[uct-probe] iface 0x242caa90 tl=tcp   dev=tailscale0  patched
[uct-probe] iface 0x242cb480 tl=tcp   dev=wlp7s0      patched
[uct-probe] iface 0x242cbf70 tl=sysv  dev=memory      patched
[uct-probe] iface 0x242cc560 tl=posix dev=memory      patched
[uct-probe] ep_am_short 4      ep_am_bcopy 1      (everything else 0)
ucx_selftest: workload ok
```

Six interfaces patched, five UCT operations observed beneath four UCP tagged
sends, and the workload still passed. **This is the first time pcomm has seen a
UCX data-plane call.**

## Stage 1 — the decision the rule demands

PLAN-20260815-1540 says: *"No opaque UCP/UCT structure offsets, debugger-only
inspection, or guessed pointer walks enter the provider."*

**Nothing here is any of those.** There are no offsets, no walks, no debugger:
every field access is spelled by `uct/api/tl.h`, which the runtime ships. The
honest description of the risk is different and narrower — `tl.h` is the
*transport-implementor* half of UCX's API, it carries no ABI-stability promise
across versions, and a layout change would put our trampolines in the wrong
slots.

That risk was measured rather than assumed:

| UCX tag | md5 of `src/uct/api/tl.h` |
|---|---|
| v1.10.0 | `c7392a6f` |
| v1.11.0 – v1.13.0 | `c3e32301` |
| **v1.14.0 – v1.19.1, and `origin/master`** | **`bef986ac`** |
| MN5 HPC-X 2.22.1 (`.../ucx/include/uct/api/tl.h`) | **`bef986ac…`** |
| local `.tools/sysroot` (UCX 1.19.0) | **`bef986ac`** |

Byte-identical across six minor releases and the current development head, and
the file MN5 ships is bit-identical to upstream. The last change to it was in
v1.14.0, and it was a copyright line.

### Option (b) — a supported profiling interface — was checked and refused

`libucs.so.0` exports nine `ucs_profile*` symbols and HPC-X even ships a
profiling build at `$HPCX/ucx/prof/lib`. It cannot do this job, for four
independent reasons:

1. **A profile record cannot carry the facts.** `ucs_profile_record_t` is
   `{uint64 timestamp, uint64 param64, uint32 param32, uint32 location}`. No
   buffer, no length, no tag, no remote address, no lane. Not one pcomm field
   survives.
2. **There is no subscriber API.** The only outputs are `ucs_profile_dump` and
   `ucs_profile_cleanup`, both of which write UCX's own binary file named by
   `UCX_PROFILE_FILE`. Consuming it means parsing an internal binary format at
   process exit — strictly more fragile than reading a public header struct, and
   off the capture stream entirely.
3. **The layer that is invisible is not profiled.** `grep -c UCS_PROFILE
   src/uct/ib/mlx5/rc/rc_mlx5_ep.c` is **0**. UCX's profiling covers the UCP
   entry points, which pcomm already hooks, and the mlx5 send path — the thing
   the door sweep could not see — carries no instrumentation at all.
4. **It changes the libraries under the application.** It needs
   `--enable-profiling` (or HPC-X's `prof/` tree ahead on `LD_LIBRARY_PATH`),
   which is not what any measured application ran against.

This is not the NCCL `PROFAPI` situation. NCCL's weak/strong alias pair is a
supported door onto the real entry points; UCX profiling is a timing facility
that does not reach the data plane.

### Chosen: **(a)**, `providers/ucx_deep/`, ids 800–899

with the risk paid for in three places rather than hoped away:

- **a runtime version guard** — `ucp_get_version()` must land in the range whose
  `tl.h` we verified identical (1.14 ≤ minor ≤ 1.19). Outside it the provider
  **refuses to patch and says so on stderr**; `PCOMM_UCX_DEEP_FORCE=1` overrides
  with a louder warning. Refusing is the correct failure: a misplaced trampoline
  does not corrupt a capture, it corrupts the application.
- **a structural guard, independent of version numbers** — before patching any
  interface, the twelve slots `UCS_CLASS_INIT_FUNC(uct_iface_t)` itself asserts
  non-`NULL` must be non-`NULL` **and** must point into an executable mapping
  belonging to a `libuc*` object, read once from `/proc/self/maps`. A shifted
  layout puts config pointers, sizes or zeroes in those slots, and every one of
  those fails the test.
- **the guard's verdict goes in the capture** — `uct_binding` (806) records the
  compiled and runtime UCX versions, `sizeof(uct_iface_ops_t)` as compiled, and
  whether the guard accepted. A capture states what it bound to instead of
  requiring this document to be trusted.

## Stage 2 — what was built

`providers/ucx_deep/`, built by the SAME switch as `providers/ucx`
(`PCOMM_PROVIDER_UCX=ON`) and deliberately not by one of its own — a separate
option would be one more configuration no gate builds, which is how the DuckDB
reader stopped compiling for 39 commits.

**One interposed symbol.** `uct_iface_open`. Everything else is a function
pointer in a struct:

| ops slot | why it is patched |
|---|---|
| `ep_am_short`, `ep_am_short_iov`, `ep_am_bcopy`, `ep_am_zcopy` | UCP's tagged and RMA protocols on a stack without HW tag matching |
| `ep_put_short/bcopy/zcopy`, `ep_get_short/bcopy/zcopy` | one-sided, and the only `remote_addr` this class of capture has |
| `ep_tag_eager_short/bcopy/zcopy`, `ep_tag_rndv_zcopy` | ConnectX HW tag matching, where the tag is on the wire |
| `ep_create`, `ep_destroy`, `iface_close` | lifetimes |

`ep_create`/`ep_destroy` are hooked at the SLOT and not at the exported
`uct_ep_create`/`uct_ep_destroy`. Both symbols dispatch straight through these
slots (`uct_iface.c:726`), and a transport can create endpoints of its own that
never pass the public symbol — DC's among them. Hooking the symbol would leave
`uct_send` rows naming endpoints nothing created, which is the defect
`ib_hooks.hpp` spent a week on. Mutation 4 below is exactly this.

**Four records**, 800/802/804/806 — `uct_iface`, `uct_ep`, `uct_send`,
`uct_binding`. `kSchemaHash` `0x3961E4E2C84D593A` -> `0xEA3C45238DB0765F`.

**Not ObjectOpsHookManager**, and the reason is arithmetic rather than taste.
Its slot table is 1024 entries global, consumed one per (tag, object), and it
instantiates one trampoline per (tag, slot). Fourteen data ops would be 14,336
generated functions and a ceiling of 73 interfaces per process — against a
process that opens one interface per (transport, device) per worker, which MN5
job 44369505 already measured at ~20 `ibv_context`s per rank for the same
reason. Being within 4x of a silent-degradation ceiling is not a margin. The
trampolines are hand-written over one shadow table instead, and the table's
behaviour is unit-tested where the record gate cannot reach it.

### What the local gate proves

`tests/ucx_deep_records.sh`, on `tests/ucx_selftest` over the self transport, in
one process, in 0.09 s. It runs `ucx` and `ucx_deep` together, which is also the
first execution of the co-running case PLAN-20260815-1540's definition of done
listed as UNTESTED.

```
uct_binding  1.19.0 / 1.19.0  ops_size 400  accepted 1  forced 0
uct_iface    6      self:memory  tcp:lo  tcp:tailscale0  tcp:wlp7s0  sysv:memory  posix:memory
uct_ep       4      all closed
uct_send     8      am_short x5 (32, 16, 48, 28, 1 B) · am_bcopy x2 (64, 24 B) · put_short (8 B @ 0x404160)
```

The three eager-short sends carry the application's byte counts EXACTLY — UCX's
eager header travels in the ops signature's `uint64 header`, so `payload` on an
`am_short` is the application's bytes and nothing else. The put's `remote_addr`
is the address the workload printed for its own buffer, to the byte.

`tests/ucx_deep_tables.cpp` covers the hot-path helpers directly, and exists
only because the record gate provably cannot reach them — see the mutation table.

### Mutation results

Every clause was broken deliberately. **Twenty mutations, twenty red** — after
three rounds of finding out that some of them were not.

| # | mutation | result |
|---|---|---|
| 1 | `ep_put_short` slot not patched | red — no `put_*` row |
| 2 | `am_short` header taken from `length` | red — header != 8 |
| 3 | put's `remote_addr` written into the `tag` field | red — both the tag clause and the address clause |
| 4 | `ep_create` slot not patched | red — `uct_send` names an unknown endpoint; opens != closes |
| 5 | `iface_close` not recorded | red — opens != closes |
| 6 | `am_short` payload taken from the AM id | red — no 32/16-byte row |
| 7 | verified version range moved past the runtime | red — REFUSING on stderr |
| 8 | ops table read one slot shifted | red — REFUSING, naming `ep_destroy` |
| 9 | `is_ucx_code` always false | red — REFUSING, naming `ep_flush` |
| 16 | `tl_name` faked instead of read from params | red — transport set is wrong |
| 17 | `ep_destroy` slot not patched | red — opens != closes |
| 10' | `HandleCache::lookup` ignores its key | red **in the unit test only** |
| 12' | `PointerTable::find` ignores its key | red **in the unit test only** |
| 13 | `PointerTable::insert` wraps instead of refusing | red in the unit test |
| 14 | `bucket_of` drops the `>> 4` | red in the unit test |
| 15 | insert of a live key allocates a second entry | red in the unit test |
| 18 | `classify_bcopy` keeps a negative return as a length | red **in the unit test only** |
| 19 | `iov_bytes` drops the stride multiply | red in the unit test |
| 20 | `saturate_iovcnt` truncates instead of saturating | red in the unit test |

**Three of these survived the record gate first, and that is the finding.** The
key comparison in each lookup structure, and the status on a `*_bcopy` door, are
untestable through this workload: six interfaces and four endpoints never
collide in tables sized for hundreds, and over the self transport nothing ever
fails. A workload without collisions cannot test a key comparison; a workload
without failures cannot test a status. That is what moved
`ucx_deep_tables.hpp`'s contents out of the provider — and the unit test needs
no UCX, so it runs in the CORE configuration, which is the one `build_all.sh`
always builds.

**One mutation appeared to survive and had not been applied correctly.** #8
patches the layout guard's call site, and after the lazy-map-refresh fix there
are TWO call sites; patching the first left the retry reading the real table and
the guard passed honestly. The guard was right and the mutation was wrong. Worth
recording because it is the same shape as a mutation surviving today because a
test exercised `signal()` while the code path was `sigaction()`.

### Overhead, measured

200,000 tagged send/receive pairs over the self transport, one `uct_send` row
each (counted, not assumed), three runs per configuration, spread under 3%:

| configuration | ns / pair | delta / UCT op |
|---|---:|---:|
| bare UCX | 13.9 | — |
| engine preloaded, no provider | 13.9 | 0 |
| **+ ucx_deep** | **53** | **+39 ns** |
| + the existing `ucx` provider (for scale) | 8,900 | +8,886 ns |

The self transport's own operation is ~14 ns, so this is the least flattering
possible denominator; against `rc_mlx5`, where a send is hundreds of ns of WQE
build and a doorbell write, 39 ns is a few percent. It is also two orders of
magnitude below what the existing public-UCP provider already costs.

**The first measurement was 138 ns and the fix came out of the breakdown**, not
out of guessing:

```
bare UCX                                    13.9 ns/op
+ trampoline and timestamps, no record       25   ns/op   (+11)
+ the record, endpoint handle not interned   53   ns/op   (+28)
+ interning the endpoint pointer            138   ns/op   (+86)   <-- four fifths
```

`rec().intern()` is not a hash lookup: it is a call across the engine's C ABI
into a table behind a per-domain mutex. A UCT endpoint's handle is immutable —
`pcomm_engine_intern_key` is keyed by the raw address and returns the same
handle forever — so a 16-entry per-thread direct-mapped cache is exact rather
than approximate, and it removed the whole 86 ns. **The same 86 ns is being paid
per operation by `providers/ucx`, `providers/ibverbs` and every other provider
that interns on its hot path**, and that is worth someone's attention as an
engine-level change; it is not this provider's to make.

### Where it is wired in

- `providers/CMakeLists.txt` — same `PCOMM_PROVIDER_UCX` block as `ucx`.
- `pcomm/run.cpp` — `-p ucx_deep` (aliases `uct`, `ucxdeep`), non-exclusive.
- `libpcomm/schema/pcomm.fbs` + both generated binding trees;
  `libpcomm/tests/schema_conformance.cpp` (4 `PCOMM_SCHEMA_MATCH` + 4 byte-level
  cases); `pcomm-read/main.cpp` (4 `Table<>` + `TableRecords`).
- `tools/gen_figures.py` — a `uct` family band under the UCX figure, since the
  whole point is that it sits beneath those records.
- **`tools/build_all.sh` now has a `ucx` configuration.** It did not, so
  `tests/ucx_records.sh` ran only for someone who typed the option by hand —
  PLAN-20260815-1540 Future work 5, closed here because a new gate inheriting
  that fate would have been worse than no gate.
- Two hand-written lists of provider record headers in `libpcomm/CMakeLists.txt`
  became one glob. The `schema_check` dependency list was already missing
  `mlx5_records.hpp`, so editing that file did not re-run the check.

## What this does NOT do

- **It does not read a QP number.** `uct_ep_get_address` packs one for the RC
  transports, but only through `uct_rc_mlx5_ep_address_t`, an internal struct —
  the exact thing the rule forbids. The public `uct_ep_query` does not expose
  one. The lane identity in these records is the `uct_ep_h`.
- **It does not relate a UCP operation to the UCT operations beneath it.**
  `ucx_tag_send` and the `uct_send` rows it became are separate facts with no
  join. That needs the engine-owned thread-local call scope PLAN-20260815-1540
  Stage 3 describes — which is now worth building, because for the first time
  the lower half of it exists.
- **Nothing has run on real hardware.** Every measurement here is the `self`,
  `tcp`, `sysv` and `posix` transports on one laptop. `rc_mlx5`/`dc_mlx5`, HW tag
  matching (`ep_tag_*`), zcopy with a real iov, rendezvous, and multi-lane
  endpoints are all uncovered — which is to say the entire reason the provider
  exists is untested on the machine it exists for. One two-node MN5 job settles
  it, and `uct_binding` was designed so that job's capture states its own
  binding rather than requiring this file to be trusted.
- **The atomic slots are not hooked.** Six more slots for the least traffic, and
  UCP-level atomics are already recorded at 620. A decision, not an oversight.
- **`ucx_deep` has no derived-layer lane.** `link_sql.hpp` gains nothing here, so
  `comm_edges` still cannot use the transport name — which is the single most
  valuable thing these records add and the obvious next item.
- **A get can complete with no UCT operation at all.** Measured, not assumed:
  the selftest's `ucp_get_nbx` produced no `get_*` row, because over shared
  memory UCP takes `uct_rkey_ptr` and memcpys. So an empty `get_*` column is not
  evidence that no get happened, and the gate deliberately does not assert one.
- **A statically linked UCX refuses.** The layout guard requires the ops
  pointers to land in an executable mapping belonging to a `libuc*` object. An
  application with UCX linked in refuses instead of instrumenting — safe, and it
  says so.

