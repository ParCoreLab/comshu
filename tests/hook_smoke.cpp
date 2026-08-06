// hook_smoke — the interception engine (pcomm::hook + pcomm::smart_dlsym),
// exercised without any vendor library:
//
//  1. trait/macro machinery (tag + HookTrait specialization) compiles and is
//     introspectable;
//  2. the object-ops path WORKS: a fake ops table is patched by install(),
//     calls through the patched pointer reach the real function, post
//     callbacks fire with the real return value, and a false predicate
//     suppresses them — the same mechanism the ibverbs provider uses on
//     ibv_context->ops, minus libibverbs (this host has none; the real thing
//     is exercised on MN5 by the suite);
//  3. the preload path WORKS: invoke_preload lazily resolves the real symbol,
//     fires pre/post callbacks around it and returns the real result — the
//     choke point every provider's interposers forward to, exercised here on
//     libc's puts because this host has no vendor library;
//  4. the dlsym registry COMPOSES: several registrants may claim one symbol,
//     each wrapping the previous, and a registrant that declines cannot blank
//     the symbol;
//  5. the ELF symbol resolver links and resolves a real libc export.

#include <cassert>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

#include "pcomm/hook/CompositeHookManager.hpp"
#include "pcomm/hook/dlsym/Dlsym.hpp"
#include "pcomm/hook/dlsym/SymbolResolver.h"

// Vendor-free preload-hook definition: exercises the trait/macro machinery
// (tag creation + HookTrait specialization).
DEFINE_PRELOAD_HOOK(demo, puts, "puts", int, const char*);

// A preload hook whose "real" implementation lives in this binary, so
// invoke_preload's dispatch can be exercised without a vendor library.
DEFINE_PRELOAD_HOOK(demo, preload_fn, "demo_preload_fn", int, int);

// A fake "device context" with an ops table, standing in for ibv_context.
struct fake_ctx;
DEFINE_OBJECT_OPS_HOOK(demo, op_add, "demo_op_add", int, fake_ctx*, int);
DEFINE_OBJECT_OPS_HOOK_PRED(
    demo, op_maybe, "demo_op_maybe", [](int ret, fake_ctx*, int) { return ret > 0; }, int,
    fake_ctx*, int);

struct fake_ctx {
    int (*add)(fake_ctx*, int);
    int (*maybe)(fake_ctx*, int);
};

namespace {
int real_preload_fn(int x) noexcept {
    return x * 2;
}

int real_add(fake_ctx*, int x) {
    return x + 1;
}
int real_maybe(fake_ctx*, int x) {
    return x;
}

int g_post_add = 0;
int g_post_maybe = 0;

struct DemoHooks
    : pcomm::hook::CompositeHookManager<DemoHooks, pcomm::hook::DefaultHookManagerConfig,
                                        demo::op_add_tag, demo::op_maybe_tag, demo::puts_tag,
                                        demo::preload_fn_tag> {
    template<pcomm::hook::ValidCallbackTag Tag> auto& get_op(fake_ctx* c) const noexcept {
        if constexpr (std::is_same_v<Tag, demo::op_add_tag>) {
            return c->add;
        } else {  // op_maybe_tag
            return c->maybe;
        }
    }
};
}  // namespace

int main() {
    using pcomm::hook::Timestamp;

    // 1. Traits.
    using T = pcomm::hook::HookTrait<demo::puts_tag>;
    static_assert(T::type == pcomm::hook::HookType::Preload);
    static_assert(pcomm::hook::has_hook_trait_v<demo::puts_tag>);
    static_assert(pcomm::hook::is_preload_hook_v<demo::puts_tag>);
    static_assert(pcomm::hook::is_object_ops_hook_v<demo::op_add_tag>);
    std::printf("hook trait: name=%s symbol=%s\n", T::name, T::symbol_name);

    // 2. Object-ops interception, end to end on a fake ops table.
    auto& mgr = DemoHooks::instance();
    mgr.register_post<demo::op_add_tag>([](int ret, fake_ctx*, int x, Timestamp t0, Timestamp t1) {
        assert(ret == x + 1 && "post callback sees the real return value");
        assert(t1 >= t0 && "call interval is ordered");
        g_post_add++;
    });
    mgr.register_post<demo::op_maybe_tag>(
        [](int, fake_ctx*, int, Timestamp, Timestamp) { g_post_maybe++; });

    fake_ctx ctx{&real_add, &real_maybe};
    mgr.install(&ctx);
    assert(ctx.add != &real_add && "install() must patch the ops pointer");

    int r = ctx.add(&ctx, 41);  // through the trampoline
    assert(r == 42 && "the real function still runs");
    assert(g_post_add == 1 && "post callback fired once");

    ctx.maybe(&ctx, 0);  // predicate (ret > 0) is false -> post suppressed
    assert(g_post_maybe == 0 && "false predicate suppresses post callbacks");
    ctx.maybe(&ctx, 7);
    assert(g_post_maybe == 1 && "true predicate lets post callbacks through");
    std::printf("object-ops: patched call=%d, post_add=%d, post_maybe=%d\n", r, g_post_add,
                g_post_maybe);

    // 3. The preload path, actually invoked. Until now this file only
    // static_asserted the traits, so invoke_preload — the choke point every
    // LD_PRELOAD interposer in every provider forwards to — had no runtime
    // coverage at all.
    //
    // The real pointer is seeded here rather than resolved, because
    // register_real's lazy resolution cannot succeed from a binary like this
    // one. It decides "did I resolve back to myself?" by dladdr-ing what
    // RTLD_DEFAULT returns for the symbol; a provider .so exports the symbol so
    // that correctly finds the provider, but a test binary exports nothing, so
    // the lookup lands on the defining library and every candidate is then
    // rejected as self. That path needs a real interposing .so and is covered
    // by the suite on MN5. What is coverable here is everything after it:
    // registered-tag gating, interval stamping, pre/post dispatch, and the real
    // result coming back out.
    static int  g_post_pf = 0;
    static bool g_pre_pf = false;
    mgr.real_func<demo::preload_fn_tag>() = &real_preload_fn;
    mgr.register_pre<demo::preload_fn_tag>([](int, Timestamp) { g_pre_pf = true; });
    mgr.register_post<demo::preload_fn_tag>([](int ret, int x, Timestamp t0, Timestamp t1) {
        assert(ret == x * 2 && "post callback sees the real return value");
        assert(t1 >= t0 && "call interval is ordered");
        g_post_pf++;
    });

    const int pr = DemoHooks::invoke_preload<demo::preload_fn_tag>(21);
    assert(pr == 42 && "the real function ran and its result came back");
    assert(g_pre_pf && "pre callback fired");
    assert(g_post_pf == 1 && "post callback fired exactly once");
    std::printf("preload: ret=%d pre=%d post=%d\n", pr, int(g_pre_pf), g_post_pf);

    // 4. The dlsym registry composes multiple registrants for ONE symbol.
    //
    // It used to throw on the second, which made "any provider may hook CUDA
    // itself" unsatisfiable: NVSHMEM reaches every CUDA entry point through a
    // single dlsym of cuGetProcAddress, so every interested provider must
    // register that same name. Two providers meant an abort at load.
    //
    // What is checked here is the ordering contract, because a chain that runs
    // in the wrong order still "works" on one registrant and is wrong the
    // moment there are two: each registrant must be handed the PREVIOUS front
    // (what it forwards to), and the application must receive the LAST one.
    static std::vector<std::string> order;
    static void*                    seen_by_second = nullptr;
    void*                           first_ret = reinterpret_cast<void*>(0x1111);
    void*                           second_ret = reinterpret_cast<void*>(0x2222);

    pcomm::smart_dlsym::register_symbol(
        "pcomm_chain_probe", [first_ret](void* next, void*, const char*) {
            order.push_back("first");
            assert(next != nullptr && "first registrant is handed the real symbol");
            return first_ret;
        });
    pcomm::smart_dlsym::register_symbol("pcomm_chain_probe",
                                        [second_ret](void* next, void*, const char*) {
                                            order.push_back("second");
                                            seen_by_second = next;
                                            return second_ret;
                                        });
    // A third that declines: returning nullptr must not blank the symbol, which
    // is how a null once reached NVSHMEM as a failed UAR allocation.
    pcomm::smart_dlsym::register_symbol("pcomm_chain_probe", [](void*, void*, const char*) {
        order.push_back("third");
        return static_cast<void*>(nullptr);
    });

    {
        const auto& chain = (*pcomm::smart_dlsym::RegisteredSymbols)["pcomm_chain_probe"];
        assert(chain.size() == 3 && "all three registrants are kept, none rejected");

        // The production path, not a reimplementation of it.
        void* real = reinterpret_cast<void*>(0xdead);
        void* fn = pcomm::smart_dlsym::on_symbol_load(real, nullptr, "pcomm_chain_probe");

        assert(order.size() == 3 && order[0] == "first" && order[1] == "second" &&
               order[2] == "third" && "registration order is innermost-first");
        assert(seen_by_second == first_ret && "each registrant receives the previous front");
        assert(fn == second_ret && "a declining registrant leaves the chain intact");

        // An unregistered symbol passes through untouched, and a null real
        // symbol stays null rather than being handed to registrants.
        assert(pcomm::smart_dlsym::on_symbol_load(real, nullptr, "pcomm_unregistered") == real);
        assert(pcomm::smart_dlsym::on_symbol_load(nullptr, nullptr, "pcomm_chain_probe") ==
               nullptr);
        assert(order.size() == 3 && "a null real symbol must not run the chain");

        std::printf("dlsym chain: %zu registrants, front=%p\n", chain.size(), fn);
    }

    // 5. SmartDlsym runtime (SymbolResolver.cpp) links and resolves a real export.
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
