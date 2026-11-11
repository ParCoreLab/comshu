# comshu — 20 minute lab talk

Slide-by-slide content, the figure each one carries, and what to say over it.
Figures are in [`figures/`](figures/) as SVG at 1600×900 (16:9, drops straight
into any deck). The runnable demo is [`talk.bsh.md`](talk.bsh.md).

**Budget.** ~10 min of slides, ~8 min of demo, 2 min of slack. The clock
markers below are cumulative; if you are past 14:00 when you reach slide 10,
cut slide 8 and go straight to the demo — the demo shows the UI anyway.

The shape: one scene, established on slide 2, that every later slide points
back at. Slides 2 and 3 are the *same figure*, the second one annotated. That
repetition is what makes the deck feel like an argument instead of a tour.

---

## 1 — Title · 0:00

> **comshu**
> one profiler for GPU communication
> ParCore Lab

Nothing else on it. Say your name and move.

---

## 2 — The run we actually care about · 0:30

**Figure:** `figures/01-scene.svg`

Two nodes, four H100 each, NVLink inside, NDR InfiniBand between. One
`ncclAllReduce` at the top, dashed lines down into both transports.

> Here is a completely ordinary job. One collective call. To understand what it
> cost you, you need three different kinds of evidence: which GPU wrote into
> whose memory over NVLink, what went onto the wire as IB verbs, and which
> collective caused both. Hold this picture — the rest of the talk is about it.

Don't explain comshu yet. This slide is the *problem statement*, and it works
because everyone in the room has debugged exactly this job.

---

## 3 — One tool per layer · 2:00

**Figure:** `figures/02-coverage.svg`

Coverage matrix: five layers across, four tools down, then two columns —
*log format* and *viewer* — and the comshu row beneath.

> We have built a tool for almost every row here. Snoopie sees GPU load/store
> and NVLink. SigmaTrace sees the verbs. ComScribe sees the API level. UCTrace
> sees UCX. Each of them is good at its row.
>
> The rows are not the problem. Look at the last two columns.

Beat. Then:

> Four log formats and four viewers. Which means there is no question you can
> ask that crosses two rows. "Which GPU write caused which wire send" is not a
> hard question — it's an *unaskable* one, because the two facts live in two
> files that share no vocabulary, no ids, and no clock.

⚠️ **Before you present:** the "log format" and "viewer" cells are generic
placeholders. Fix them to what those tools actually emit — the claim that
carries the slide is that they are all *different*, and a wrong specific will
get you interrupted by whoever wrote that tool.

---

## 4 — So why not just merge the logs? · 4:00

No figure — this one is bullets, and it's short. It exists so the architecture
on slide 5 reads as a *consequence* rather than a taxonomy.

> - **The capture mechanisms are incompatible.** `LD_PRELOAD` interposition,
>   NVBit binary instrumentation, a driver export-table patch, a library's own
>   plugin API. These are not four implementations of one idea.
> - **Some of them cannot share a process.** NVBit takes over the CUDA runtime
>   and `dlsym`. Nothing else can live in there with it.
> - **Clocks disagree.** Two nodes, two timebases. Merging traces without
>   solving this produces a timeline that is confidently wrong.
> - **No shared vocabulary.** Every tool invented its own word for "a
>   transfer", and none of them mean quite the same thing.

> So the merge cannot happen at the log level. It has to happen *above* the
> capture mechanism and *below* the analysis.

That last sentence is the thesis of the talk. Land it.

---

## 5 — comshu · 5:30

**Figure:** `figures/03-architecture.svg`

App → providers → recorder → `.pcs` streams → readers, with the L0–L2 /
L3 band underneath.

> Providers are plugins, one `.so` each, and they are the *only* place vendor
> types are allowed — no `cuda*`, no `ibv_*`, no `Sanitizer_*` anywhere to the
> right of the recorder, and the build enforces that.
>
> Everything a provider records goes through one recorder, into one clock
> domain, in one record model. After that point nothing downstream knows or
> cares which transport it came from.

On the "they can't share a process" problem from slide 4:

> That one turned out to be solvable rather than structural. NVBit *looked*
> like it had to own the process, so gpumem ran in its own replay pass — but the
> crash we blamed on it fighting for `dlsym` was actually two embedded copies of
> our engine sharing a singleton. With that fixed, all six co-run: one process,
> one recorder, one timeline. Replay is still there for a provider that genuinely
> must own the runtime, but nothing needs it today — which matters, because
> replay's passes are separate executions and can't carry cross-transport
> causality.

**The axis worth naming, if you have 30 seconds** — it's the question people
ask, and it's what makes the provider list read as a design rather than a pile:

| provider | initiator | mechanism |
|---|---|---|
| `gpumem` | GPU — an SM instruction | load / store |
| `ib` | **CPU** — posts a work request | RDMA, NIC does the DMA |
| `ibgda` | **GPU** — rings the doorbell | RDMA, NIC does the DMA |
| `nccl` | CPU (API call) | none — the operation layer |

> RDMA is split across two providers not because either is half-finished, but
> by *who starts it*. IBGDA never calls `ibv_post_send`, so a GPU-initiated run
> traced with `-p ib` comes back with empty tables and no error at all. And
> `gpumem` is neither of those: it instruments SASS load/store, which is exactly
> why it cannot see a copy-engine `cudaMemcpy` — no instruction executes.

---

## 6 — The record model · 7:30

**Figure:** `figures/04-record-model.svg`

The four archetype cards, and the temporal-kind strip beneath them.

This is the actual contribution. Give it the most time of any slide.

> Every profiler I have read ends up re-deriving these four things and giving
> them four different names. We named them once, and made them the only
> vocabulary a provider is allowed.
>
> An **Entity** is a thing with a lifetime — an allocation, a queue pair, a
> communicator. It opens and it closes. An **Event** is something that
> happened. An **Artifact** is a one-off fact recorded once and resolved later
> — a source line, an intern key. An **Edge** relates two handles.
>
> And every one of them carries a temporal kind, because "when" turns out to
> be the thing each tool improvises differently. A posted send is an *Instant*.
> A kernel is an *Interval*. But a GPU memory access under NVBit is a
> **Window** — we know it happened somewhere inside the kernel's span and we
> refuse to pretend to a precision we don't have. That honesty is a schema
> feature, not a comment.

The `Window` example is the one that convinces people. Don't skip it.

---

## 7 — What that buys · 9:30

**No figure — live-ish code on the slide.** Two blocks and a result table.

Left: the NVLink ring, from a `-p gpumem` capture.

```sql
SELECT run_dev, mem_dev, count(*) AS stores, sum(size) AS bytes
FROM gpu_access WHERE op='store' AND run_dev != mem_dev
GROUP BY 1,2 ORDER BY 1;
```

Right: the rank graph, from a `-p ib` capture.

```sql
SELECT src_rank, dst_rank, sends, bytes FROM comm_edges ORDER BY 1;
```

> Same dialect. Same table shape. Same capture directory, if you ran both.
> Because there are only four archetypes, the reader is written once — a new
> provider gets relational tables, SQL, a Perfetto timeline and the UI for
> free, without anybody downstream writing a line of code for it.

The point isn't the SQL, it's that these two queries *look the same*. Say that.

---

## 8 — The views · 11:00

**Figure:** screenshots — take these yourself, they should be your captures.
Grab them from `comshu ui` on a corpus capture the night before:

- **Matrix** — rank × rank bytes, the classic comm matrix
- **Timeline** — or a Perfetto screenshot, cross-node clocks aligned
- **Objects** or **Sankey** — whichever looks better on your capture
- **Chat** — one question, its answer, and the SQL underneath it

On the chat pane specifically:

> There is a chat pane, and the interesting thing about it is what it does
> *not* do. It never returns data. It answers with **a query** — prose, plus
> the SQL that produced the number, plus a chart spec. The rows come back
> through the same path every other panel uses. So every number it gives you
> is one click from being re-run by hand. It cannot quietly make something up,
> because it is not the thing holding the data.

That framing is the reason to show it at all. Without it the pane is just
another chatbot; with it, it's a design position.

Cut this slide first if you're behind — the demo covers it.

---

## 9 — Is any of it right? · 12:30

**Figure:** a screenshot of a green suite board (`run_on_mn5.sh` output).

> Twenty cases on MareNostrum 5. What makes them worth anything is that each
> one's expected numbers are computed **on paper from the workload's arguments,
> before the run**. Four GPUs, two iterations, 1024 threads: 8192 peer stores,
> 2048 per ring edge. If the capture says something else, the capture is wrong
> — we are not fitting the oracle to the output.
>
> That suite is the merge gate. A change to capture semantics isn't done until
> that board is green, and it refuses to run on a dirty tree, so every board is
> attributable to a commit.

Then the closer:

> And there's a file in the repo called `BUGS-FOUND.md` — defects in *other*
> people's software that comshu found, each with the evidence that identified it
> and the explanations we ruled out. The claim a tool makes is what it found,
> not what it can in principle see.

Pick your single best entry from that file and tell it as a 20-second story.
This is the most credible slide in the deck; it's worth rehearsing.

---

## 10 — Demo · 14:00

Just the word. Switch to the terminal. Cells are in
[`talk.bsh.md`](talk.bsh.md) and contain no commentary — everything you say
over them is here.

One run, end to end. Four nodes on MareNostrum 5, all sixteen H100s, NVSHMEM
hub-and-spoke: fifteen spokes push a full 16 KB slot to rank 0, rank 0 answers
each with a slot/8 — asymmetric by 8× on purpose. `NVSHMEM_DISABLE_P2P=1` is
what makes every edge cross the NIC: without it the three spokes sharing rank
0's node would move over NVLink and be invisible to `-p ib`, and the star would
come up missing three arms with no error to explain it.

**`comshu list`**

> Providers are plugins. This is the menu, printed by the binary — not a slide
> I wrote.

**`ls` the application**

> Stock NVSHMEM. Not built for comshu, not linked against it, not recompiled.

**`mpirun … comshu run -p ib -o ~/demo-out -- …`**

> The only change is the four words in front of the program.

**`ls -R` then `comshu read`**

> One directory per rank, one file per record type. Nothing is named after a
> tool or a version — each stream's header says what it is, so the capture
> tells you what it is and you never infer from filenames.

**`rsync` home, then `agent/run.sh`**

Browser, in order — and only these three:

**Matrix**

> Sixteen ranks, and one column lit up: every spoke into the hub, the hub
> answering every spoke. A star in *rank* space — and QP numbers and LIDs differ
> on every run, so comshu got here by reading each queue pair's RTR transition
> and mapping the destination back to the rank that owns it.
>
> And the row doesn't match the column — ~50 KB inbound against ~7 KB out.
> Nobody configured that; it's what the workload does. An aggregate that
> symmetrised the edge would paint both at a full slot, and no ring or pingpong
> capture could ever catch it.

The 16×16 grid is the reason to run all the GPUs: a 4×4 shows the same fact,
but a full matrix with one bright cross through rank 0 is the picture people
remember.

If someone does the arithmetic and asks why it's ~6.7× and not the 8× the
workload implies — that's the best question you can get, and the answer is a
second slide's worth of substance:

> Because `comm_edges` sums every send on a rank pair, and two different things
> are moving. Group by opcode and they separate cleanly:

```
rank  opcode      len  n      bytes
0     rdma_write  4    4608   18432     the hub's replies
1     rdma_write  4    12288  49152     a spoke's puts
1     send        4    30     120       NVSHMEM synchronisation
1     send        64   30     1920
```

> The application's data is `rdma_write`, four bytes at a time, because the
> workload does element-wise puts. NVSHMEM's synchronisation is `send` — sixty
> messages per rank, a fixed cost with nothing to do with message size. Filter
> to the writes and the ratio is 49152 to 6144: **exactly 8**, the number the
> workload was written to produce.

That's the point worth making, and it isn't about ratios:

> The tool didn't average those together and hand me one number. It kept the
> opcode, so I could tell the application's traffic from the library's. The
> suite's assertion for this case checks *direction*, not ratio, for the same
> reason — the exact factor is the workload's business; what has to hold for
> the tool to be right is that nothing gets conflated.

**Timeline**

> One track per queue pair, cross-node clocks aligned.

**Chat** — one question: *which rank pair moved the most bytes?*

> Watch what comes back beside the answer: the SQL that produced it. The model
> never returns rows — it returns a *query*, and the panel fetches the rows
> through the same path every other panel uses. Every number here is one click
> from being re-run by hand. It isn't a chatbot sitting on the data; it's a
> chatbot that can only hand you queries.

Then stop. One question is a demonstration; three is a gamble on a local model
in front of an audience.

---

## 11 — Where it is, where it goes · 19:00

> **Today:** six providers — ib, ibgda, gpumem, kernel, ucx, nccl. One
> binary with verbs. CSV, Parquet, Perfetto, a UI, and the chat sidecar. Twenty
> oracle cases green on MN5.
>
> **Next:** *(your call — pick two, be concrete)*
> - the Compute-Sanitizer backend, so gpumem doesn't need NVBit
> - the NCCL device-initiated path (`nccl_window`, `nccl_dev_comm`)
> - ucTrace's remaining coverage folded in as a provider

End on the thesis, not on a list:

> One capture. One schema. One place to look. Adding a transport is adding a
> provider — and nothing downstream has to learn about it.

---

## Pre-flight

The night before:

- [ ] Reinstall `comshu` to `~/.local` on MN5 so the renamed providers are live —
      `comshu list` must print `gpumem` and `ibgda`, six rows
- [ ] `nccl_comm` duplicate-`rank` bug is still open (`gen_raw_columns.py:143`
      injects `rank`; `NcclComm` has its own field called `rank`). The demo
      never opens an NCCL capture, so it isn't a blocker — just don't go
      exploring into one on stage
- [ ] Correct the format/viewer cells in `figures/02-coverage.svg`
- [ ] Take the UI screenshots for slide 8 from a real capture
- [ ] Screenshot a green suite board for slide 9
- [ ] Pick and rehearse one `BUGS-FOUND.md` story
- [ ] Run every cell of `talk.bsh.md` end to end once, and keep those outputs
      as the fallback

The morning of:

- [ ] `salloc` on MN5 **before** the session starts, and leave it held
- [ ] Confirm the fallback captures are on simurgh-base (`~/waste/comshu/corpus`)
- [ ] Confirm the local model endpoint answers (`node agent/ask.mjs --print-prompt`)
