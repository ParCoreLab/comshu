# plans/

Design plans that outlive a single working session — written down so the
reasoning behind a direction survives even when the conversation that produced
it doesn't.

## Naming

```
PLAN-{YYYYMMDD-HHMM}-{short-title}-{status}.md
```

`status` ∈ `todo` · `backlog` · `in-progress` · `done`

The status lives in the **filename** so `ls` is the status board. When a plan
changes state, rename the file (`git mv`) and update the `Status:` line in its
header — keep the datetime unchanged, it's the creation stamp and the sort key.

## What belongs here

A plan, not a log: the decision, the options that were rejected and *why*, the
constraints that must be honoured, and the steps. Enough that someone picking it
up cold doesn't re-derive the dead ends. Findings about how the system actually
behaves go in `things-to-circle-back-to.md`; plans link to them rather than
restating them.
