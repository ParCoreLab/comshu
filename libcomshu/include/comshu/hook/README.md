# comshu::hook — interception engine (L2)

This is where SigmaTrace's `Hook/` framework lands, **as-is** (it is the canonical
implementation, see `../../../../ARCHITECTURE.md` §4):

- `HookTrait.hpp` — tag + trait per hooked symbol (`DEFINE_PRELOAD_HOOK`,
  `DEFINE_OBJECT_OPS_HOOK`), renamed to the `comshu::hook` namespace.
- `PreloadHookManager` / `ObjectOpsHookManager` / `CompositeHookManager` — CRTP,
  zero-virtual-call fast path.
- `CallbackManager` — pre/post observer lists.
- `SmartDlsym` — symbol resolution.

Port checklist (phase 1, alongside the ibverbs proof provider):

- [x] Copy `SigmaTrace/Hook/` + `SigmaTrace/SmartDlsym/` here (headers under
      `hook/`, `hook/detail/`, `hook/dlsym/`; runtime `.cpp` under
      `libcomshu/src/hook/`, compiled into the `comshu::hook` static lib).
- [x] Namespaces retargeted: `hook` -> `comshu::hook`, `smart_dlsym` ->
      `comshu::smart_dlsym`, `Helpers` -> `comshu::helpers`; include paths fixed.
- [x] Build/link gate: `tests/hook_smoke.cpp` (trait machinery + ELF symbol
      resolver, vendor-free).
- [ ] Hook managers emit records through `rec::Recorder<>` — done **in the
      provider's EventSource**, not the engine (the engine is recorder-free;
      lands with the ibverbs provider port).
- [ ] Retarget SigmaTrace's Hook unit tests.

The engine has no recorder or vendor coupling: per-symbol trait definitions
for e.g. `ibv_post_send` live in the ibverbs *provider*, which supplies the
vendor types at its own compile time. Nothing here may include vendor headers.
