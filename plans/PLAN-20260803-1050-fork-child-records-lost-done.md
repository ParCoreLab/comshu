# PLAN — the fork child records nothing; give it back its arenas

- **Created:** 2026-08-03 10:50 +03
- **Status:** DONE 2026-08-03. Fixed as designed — one engine function, no
  hot-path change. fork_smoke goes 130/0 -> 130/50, and now also checks *whose*
  records each stream holds. Verified both directions (removing the call
  reproduces 0/50 exactly). Local suite 13/13. **MN5 gate still owed.**
- **Finding:** `things-to-circle-back-to.md` — "fork-aware capture is broken".
- **Test:** `tests/fork_smoke.cpp`, currently `WILL_FAIL` in
  `tests/CMakeLists.txt`. Fixing this plan means removing that marker.

## Symptom

```
child exit=0; parent rank-0=130 (expect 130), child rank-2484172=0 (expect 50)
```

The parent is perfect. The child writes **none** of its 50 records. The feature
was validated at exactly 130/50 when it landed (ec32ab2), so this regressed
since — invisibly, because `NDEBUG` had compiled the assertion away until
`bef17aa`.

## Cause

Not what it first looks like. The tempting diagnosis is that `Recorder::append`
caches the arena in a `static thread_local`, so after `fork()` the child is
still pointing at the parent's arena. That is true, and it is *harmless*:

```cpp
void Arena::drain() override {
    if (pub.count == 0) return;
    auto& tracer = Tracer::instance();
    if (!tracer.has_recorder()) { pub.count = 0; return; }
    tracer.recorder().sink().submit(...);   // resolved HERE, not at construction
}
```

`Arena` holds no sink pointer. It asks the Tracer for the current sink at drain
time, and by then `atfork_child` has already called `start(cfg)` with the
child's private directory. **A cached arena pointer drains to the right place.**

The actual break is one line in `ThreadBufferRegistry::reset_for_fork()`:

```cpp
m_buffers.clear();
```

The child forgets its arenas exist. `finalize()` flushes via
`ThreadBufferRegistry::drain_all()`, which now iterates an empty list, so the
child's 50 records sit in an arena nobody drains and are lost at `_exit`.

The intent behind `clear()` was right — the parent's buffered records must not
be flushed from the child, or they land in the child's files (and the COW-shared
`FILE*`s made it worse historically). But it discards the **registration** when
what needs discarding is the **content**. The arenas themselves are exactly what
the child needs going forward.

`atfork_child`'s comment describes a mechanism that no longer exists:

> The fresh Recorder has a new address, so the per-thread buffer_store rebuilds
> against the child's sink.

True of the old `{recorder, type_id}`-keyed map. Since the shared-engine C ABI
(4dd6a69) the arenas are keyed by `type_id` alone in a thread-local map, and
nothing rebuilds. The comment outlived its design and hid the bug.

## Fix

On the child side of the fork, for every arena belonging to this thread:

1. **`pub.count = 0`** — drop the parent's buffered records. This is the part
   `reset_for_fork` was reaching for; doing it on the arena is what makes it
   actually true, because the records live there, not in the registry.
2. **re-register** with the `ThreadBufferRegistry`, so `finalize()` finds them.

Both are engine-side, on the arena map that `pcomm_engine_arena` already owns.
Nothing on the hot path changes; no generation counter, no extra load per
record, no ABI change to `pcomm_arena`.

Only the forking thread survives into the child, so only its arena map matters —
which is exactly the map `arenas_for_this_thread()` returns in the handler.

### Shape

```c
/* engine_abi.h */
void pcomm_engine_reset_arenas_for_fork(void);
```

```cpp
/* engine.cpp */
void pcomm_engine_reset_arenas_for_fork(void) {
    for (auto& [id, a] : pcomm::arenas_for_this_thread()) {
        a->pub.count = 0;                                              // (1)
        pcomm::rec::ThreadBufferRegistry::instance().register_buffer(a.get()); // (2)
    }
}
```

```cpp
/* tracer.hpp, atfork_child, after start(cfg) re-inits the sink */
pcomm_engine_reset_arenas_for_fork();
```

Ordering matters: **after** `start(cfg)`, because `register_buffer` takes the
registry mutex that `reset_for_fork()` placement-reset, and because draining
before the child's sink exists would be the original bug.

## Steps

1. Add `pcomm_engine_reset_arenas_for_fork` to `engine_abi.h` + `engine.cpp`.
2. Call it from `Tracer::atfork_child` after `start(cfg)`.
3. Rewrite the stale comment in `atfork_child` to describe the arenas, and the
   one on `reset_for_fork` to say it clears registrations *and* that the arenas
   are re-registered by the engine immediately after.
4. Remove `set_tests_properties(fork_smoke PROPERTIES WILL_FAIL TRUE)` and its
   note from `tests/CMakeLists.txt`.
5. Strengthen `fork_smoke`: it currently checks counts only. Add that the
   child's stream contains the child's *own* records — record a distinguishable
   payload in the child (`device` field differs) and assert on it, so a fix that
   accidentally routed the parent's 100 into the child would still fail.

## Gate

- Local: `fork_smoke` green with the marker removed, whole suite green.
- MN5: full `tests/hpc/run_on_mn5.sh`. Fork is not on any case's path today, so
  the expectation is **8/8 unchanged**; the run is to confirm the engine change
  did not disturb the arena path that every capture uses.

## Why not the other fixes

- **Generation counter in `append`.** Would work, and was the first design —
  rejected once `Arena::drain()` turned out to resolve the sink dynamically.
  It puts a load and a compare on the hot path of every record to solve a
  problem that does not exist.
- **Have `reset_for_fork` drain instead of clear.** That is the original bug: it
  writes the parent's buffered records through COW-shared `FILE*`s.
- **Give `Arena` a sink pointer and rebind it.** More invasive, and it would
  *remove* the dynamic resolution that makes the current fix small.
