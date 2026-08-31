# A GPU needs a stable identity, and it must be scoped by node

**Status:** todo. Supersedes the widened UNION in `_rank_device`, which is a
patch over the symptom.

## The problem the UNION does not fix

`rank_identity.device` and `_device_owner` key a GPU by its **runtime ordinal**.
That ordinal is wrong as an identity in two independent ways:

1. **Node-local.** `pcomm.fbs` already says it: "A device ordinal is node-local,
   so `run_device 0` on two nodes is two different GPUs."
2. **Remap-local, which is not written down anywhere.** `CUDA_VISIBLE_DEVICES`
   renumbers per process. Four ranks pinned with
   `CUDA_VISIBLE_DEVICES=$LOCAL_RANK` — the single most common idiom in real
   MPI+CUDA launch scripts — **each report device 0**. `_device_owner` then
   resolves every NVLink peer on that node to `min(rank)`, silently.

Latent rather than live: our suite does not pin that way, so ranks see all four
GPUs and pick distinct ordinals. Any outside user hits it on their first run.

## What to record

A per-(rank, device) fact, emitted by the ENGINE — it already owns `GpuKernel`
(530) and `MemObject` (520) and already hooks CUDA, so it is the natural owner
and no GPU backend is involved.

```
GpuDevice  (Entity, shared 5xx range)
    ordinal       uint16   the runtime ordinal, post-CUDA_VISIBLE_DEVICES
    uuid_hi/lo    uint64   cuDeviceGetUuid, the 16 raw bytes
    pci_bus       uint32   domain<<16 | bus<<8 | device  (function is always 0)
    name          Handle   interned, e.g. "NVIDIA A100-SXM4-40GB"
```

## Three identifiers, because none of them is trustworthy alone

Measured on du04 (bare-metal A100, 2026-08-31) — all three healthy there:

```
0, GPU-52dfa0f6-007f-ca52-b80c-7aa6a073a712, 00000000:01:00.0, 1564720026413
1, GPU-75652abe-562f-e426-5d4c-1345963e8940, 00000000:41:00.0, 1564720027110
2, GPU-00c745d5-0eb3-c003-241f-634bcacac840, 00000000:81:00.0, 1564720027366
3, GPU-42f78f03-7ce2-2830-0a86-a71e7a1dca38, 00000000:C1:00.0, 1564720026371
```

But healthy here is not healthy everywhere, and the failure is quiet:

- **UUIDs can be degenerate.** Under vGPU, some container runtimes and older
  drivers they come back zero-filled or as small incrementing integers. A
  degenerate UUID is *worse than no UUID*, because "GPU-0" on node A and
  "GPU-0" on node B look identical — which resurrects exactly the bug the
  ordinal has, while looking like it was fixed.
- **MIG changes the shape.** A MIG instance reports
  `MIG-GPU-<uuid>/<gi>/<ci>`; the parent UUID is shared by every instance on
  that physical card, so parent UUID alone merges instances that are separate
  devices.
- **PCI bus id is the reliable NODE-LOCAL identifier.** It comes from the PCI
  topology rather than the driver's identity database, so it is populated
  whenever the device is, and is distinct within a machine by construction. It
  is NOT globally unique — `0000:01:00.0` exists on every node in the cluster.

## The rule that falls out

**Scope every device identity by node, always — never trust a device
identifier to be globally unique.** `node_identity.node_hash` (FNV-1a of the
hostname) already exists and is already what `_node` uses.

So the key is `(node_hash, pci_bus)`, with the UUID as a *cross-check* rather
than the key:

1. `(node_hash, pci_bus)` — primary. Correct under `CUDA_VISIBLE_DEVICES`
   remapping, correct across nodes, correct when the UUID is degenerate.
2. `uuid` — used to detect the case the ordinal cannot: the same physical card
   appearing under two ordinals in one process, and MIG instances of one
   parent. Ignored when it is all-zero, and a `link_health` finding when two
   devices on one node share a non-zero UUID that is not a MIG parent.
3. `ordinal` — kept as a column because it is what every other record joins on
   today, but it stops being an identity. It becomes a per-process label that
   resolves to a device THROUGH `GpuDevice`, scoped by rank.

Even if a UUID were globally unique and trustworthy, node-scoping costs nothing
and removes a whole class of silent cross-node merge. Do it unconditionally.

## Why this is worth a schema-hash change

It is one — `kSchemaHash` moves and capture headers written before it stop
parsing. That is the accepted trade (2026-08-30, the user's rule: better schema
over back support). Captures are ~15 minutes to retake, a full 22-case suite;
a table that says "device 0" for four different GPUs costs every future reader,
permanently.

## Order of work

1. `GpuDevice` in `pcomm.fbs` + `records.hpp`, regenerate bindings, bump
   `kSchemaHash`, keep `PCOMM_SCHEMA_MATCH` / `expect_identical` in step.
2. Engine emits it at first CUDA use, for every visible device, not just the
   current one — a rank that never calls `cudaSetDevice` still has a device.
3. `_rank_device` and `_device_owner` rekey onto `(node_hash, pci_bus)`.
   Both must change together; a rank that resolves in one and not the other is
   how a peer lookup returns a rank the identity table says owns nothing.
4. A `link_health` row for a degenerate identity, so a capture taken where the
   UUIDs are fake SAYS so rather than quietly resolving to whatever `min(rank)`
   returns.
5. Measure the MIG and `CUDA_VISIBLE_DEVICES=$LOCAL_RANK` cases on real
   hardware before claiming either works. Neither is exercised by the suite
   today, which is why both are in this plan and not in a commit message.
