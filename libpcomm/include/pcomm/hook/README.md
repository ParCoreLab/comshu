# pcomm::hook — interception engine (L2)

This is where SigmaTrace's `Hook/` framework lands, **as-is** (it is the canonical
implementation, see `../../../../ARCHITECTURE.md` §4):

- `HookTrait.hpp` — tag + trait per hooked symbol (`DEFINE_PRELOAD_HOOK`,
  `DEFINE_OBJECT_OPS_HOOK`), renamed to the `pcomm::hook` namespace.
- `PreloadHookManager` / `ObjectOpsHookManager` / `CompositeHookManager` — CRTP,
  zero-virtual-call fast path.
- `CallbackManager` — pre/post observer lists.
- `SmartDlsym` — symbol resolution.

Port checklist (phase 1, alongside the ibverbs proof provider):

- [ ] Copy `SigmaTrace/Hook/` + `SigmaTrace/SmartDlsym/` here.
- [ ] s/sigma-namespace/pcomm::hook/ + include paths.
- [ ] Hook managers emit records through `rec::Recorder<>` instead of the old
      `EventRecorder`.
- [ ] Keep the existing SigmaTrace unit tests, retargeted.

Nothing in this directory may include vendor headers; the trait definitions for
e.g. `ibv_post_send` live in the ibverbs *provider*, which supplies the vendor
types at its own compile time.
