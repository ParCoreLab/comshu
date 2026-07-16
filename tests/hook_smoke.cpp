// hook_smoke — the SigmaTrace interception engine, lifted into pcomm::hook +
// pcomm::smart_dlsym, compiles and links, and its ELF symbol resolver works.
// Functional interception (real LD_PRELOAD callbacks) is validated end-to-end
// by the ibverbs provider; this is the vendor-free build/link gate.

#include <cassert>
#include <cstdio>

#include "pcomm/hook/CompositeHookManager.hpp"
#include "pcomm/hook/dlsym/Dlsym.hpp"
#include "pcomm/hook/dlsym/SymbolResolver.h"

// Vendor-free preload-hook definition: exercises the trait/macro machinery
// (tag creation + HookTrait specialization + arg-name tuple).
DEFINE_PRELOAD_HOOK(demo, puts, "puts", int, const char*);

int main() {
    using T = pcomm::hook::HookTrait<demo::puts_tag>;
    static_assert(T::type == pcomm::hook::HookType::Preload);
    static_assert(pcomm::hook::has_hook_trait_v<demo::puts_tag>);
    static_assert(pcomm::hook::is_preload_hook_v<demo::puts_tag>);
    std::printf("hook trait: name=%s symbol=%s\n", T::name, T::symbol_name);

    // SmartDlsym runtime (SymbolResolver.cpp) links and resolves a real export.
    void* p = find_symbol_in_lib("libc", "puts");
    std::printf("find_symbol_in_lib(libc, puts) = %p\n", p);
    assert(p != nullptr);

    // Interposed-dlsym real pointers, initialized by the load constructor(0).
    assert(pcomm::smart_dlsym::real_dlsym != nullptr);
    std::printf("smart_dlsym::real_dlsym = %p\n",
                reinterpret_cast<void*>(pcomm::smart_dlsym::real_dlsym));

    std::puts("hook_smoke OK");
    return 0;
}
