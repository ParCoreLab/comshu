// hook_smoke — the interception engine (comshu::hook + comshu::smart_dlsym),
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

#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "comshu/hook/CompositeHookManager.hpp"
#include "comshu/hook/dlsym/Dlsym.hpp"
#include "comshu/hook/dlsym/SymbolResolver.h"

// Vendor-free preload-hook definition: exercises the trait/macro machinery
// (tag creation + HookTrait specialization).
DEFINE_PRELOAD_HOOK(demo, puts, "puts", int, const char*);

// A preload hook whose "real" implementation lives in this binary, so
// invoke_preload's dispatch can be exercised without a vendor library.
DEFINE_PRELOAD_HOOK(demo, preload_fn, "demo_preload_fn", int, int);

// A preload hook for testing register_hook's self-recursion guard.
DEFINE_PRELOAD_HOOK(demo, self_guard_fn, "demo_self_guard_fn", int, const char*);

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
    : comshu::hook::CompositeHookManager<DemoHooks, comshu::hook::DefaultHookManagerConfig,
                                        demo::op_add_tag, demo::op_maybe_tag, demo::puts_tag,
                                        demo::preload_fn_tag> {
    template<comshu::hook::ValidCallbackTag Tag> auto& get_op(fake_ctx* c) const noexcept {
        if constexpr (std::is_same_v<Tag, demo::op_add_tag>) {
            return c->add;
        } else {  // op_maybe_tag
            return c->maybe;
        }
    }
};
}  // namespace

int main() {
    using comshu::hook::Timestamp;

    // 1. Traits.
    using T = comshu::hook::HookTrait<demo::puts_tag>;
    static_assert(T::type == comshu::hook::HookType::Preload);
    static_assert(comshu::hook::has_hook_trait_v<demo::puts_tag>);
    static_assert(comshu::hook::is_preload_hook_v<demo::puts_tag>);
    static_assert(comshu::hook::is_object_ops_hook_v<demo::op_add_tag>);
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
    static std::atomic<int>  g_post_pf{0};
    static std::atomic<bool> g_pre_pf{false};
    mgr.real_func<demo::preload_fn_tag>() = &real_preload_fn;
    mgr.register_pre<demo::preload_fn_tag>([](int, Timestamp) { g_pre_pf.store(true); });
    mgr.register_post<demo::preload_fn_tag>([](int ret, int x, Timestamp t0, Timestamp t1) {
        assert(ret == x * 2 && "post callback sees the real return value");
        assert(t1 >= t0 && "call interval is ordered");
        g_post_pf.fetch_add(1, std::memory_order_relaxed);
    });

    const int pr = DemoHooks::invoke_preload<demo::preload_fn_tag>(21);
    assert(pr == 42 && "the real function ran and its result came back");
    assert(g_pre_pf.load() && "pre callback fired");
    assert(g_post_pf.load() == 1 && "post callback fired exactly once");
    std::printf("preload: ret=%d pre=%d post=%d\n", pr, int(g_pre_pf.load()), g_post_pf.load());

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

    comshu::smart_dlsym::register_symbol(
        "comshu_chain_probe", [first_ret](void* next, void*, const char*) {
            order.push_back("first");
            assert(next != nullptr && "first registrant is handed the real symbol");
            return first_ret;
        });
    comshu::smart_dlsym::register_symbol("comshu_chain_probe",
                                        [second_ret](void* next, void*, const char*) {
                                            order.push_back("second");
                                            seen_by_second = next;
                                            return second_ret;
                                        });
    // A third that declines: returning nullptr must not blank the symbol, which
    // is how a null once reached NVSHMEM as a failed UAR allocation.
    comshu::smart_dlsym::register_symbol("comshu_chain_probe", [](void*, void*, const char*) {
        order.push_back("third");
        return static_cast<void*>(nullptr);
    });

    {
        const auto& chain = (*comshu::smart_dlsym::RegisteredSymbols)["comshu_chain_probe"];
        assert(chain.size() == 3 && "all three registrants are kept, none rejected");

        // The production path, not a reimplementation of it.
        void* real = reinterpret_cast<void*>(0xdead);
        void* fn = comshu::smart_dlsym::on_symbol_load(real, nullptr, "comshu_chain_probe");

        assert(order.size() == 3 && order[0] == "first" && order[1] == "second" &&
               order[2] == "third" && "registration order is innermost-first");
        assert(seen_by_second == first_ret && "each registrant receives the previous front");
        assert(fn == second_ret && "a declining registrant leaves the chain intact");

        // An unregistered symbol passes through untouched, and a null real
        // symbol stays null rather than being handed to registrants.
        assert(comshu::smart_dlsym::on_symbol_load(real, nullptr, "comshu_unregistered") == real);
        assert(comshu::smart_dlsym::on_symbol_load(nullptr, nullptr, "comshu_chain_probe") ==
               nullptr);
        assert(order.size() == 3 && "a null real symbol must not run the chain");

        std::printf("dlsym chain: %zu registrants, front=%p\n", chain.size(), fn);
    }

    // 5. SmartDlsym runtime (SymbolResolver.cpp) links and resolves a real export.
    void* p = find_symbol_in_lib("libc", "puts");
    std::printf("find_symbol_in_lib(libc, puts) = %p\n", p);
    assert(p != nullptr);

    // Interposed-dlsym real pointers, initialized by the load constructor(0).
    assert(comshu::smart_dlsym::real_dlsym != nullptr);
    std::printf("smart_dlsym::real_dlsym = %p\n",
                reinterpret_cast<void*>(comshu::smart_dlsym::real_dlsym));

    // 6. CR-02 regressions:
    //    (a) register_real() is idempotent and thread-safe when multiple threads
    //        race on lazy resolution (used to call std::abort() on the second thread).
    //    (b) register_hook()'s dlsym callback refuses to overwrite real_func with
    //        hook_fn itself (e.g. when a caller does dlsym(RTLD_DEFAULT, sym)),
    //        preventing infinite recursion.
    {
        std::atomic<bool>        go{false};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {}
                const int v = DemoHooks::invoke_preload<demo::preload_fn_tag>(5);
                assert(v == 10);
            });
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads)
            t.join();
        assert(g_post_pf.load() == 9);

        // Direct concurrent register_real() on an already-resolved slot must be a no-op,
        // never aborting.
        comshu::hook::detail::PreloadHookManager<demo::preload_fn_tag> pm;
        pm.real_func<demo::preload_fn_tag>().store(&real_preload_fn, std::memory_order_release);
        std::atomic<bool>        go2{false};
        std::vector<std::thread> t2;
        for (int i = 0; i < 8; ++i) {
            t2.emplace_back([&] {
                while (!go2.load(std::memory_order_acquire)) {}
                pm.register_real<demo::preload_fn_tag>();
            });
        }
        go2.store(true, std::memory_order_release);
        for (auto& t : t2)
            t.join();
        assert(pm.real_func<demo::preload_fn_tag>().load() == &real_preload_fn);

        // Self-recursion guard in register_hook()'s smart_dlsym callback:
        // passing hook_fn itself (as dlsym(RTLD_DEFAULT, sym) does) must NOT
        // overwrite real_func; passing an external symbol (e.g. from libc) MUST.
        auto* external_fn =
            reinterpret_cast<comshu::hook::HookTrait<demo::self_guard_fn_tag>::FuncPtr>(p);
        auto* fake_hook = +[](const char*) noexcept -> int { return 0; };
        static comshu::hook::detail::PreloadHookManager<demo::self_guard_fn_tag> guard_pm;
        guard_pm.register_hook<demo::self_guard_fn_tag>(fake_hook);

        void* hook_addr = reinterpret_cast<void*>(fake_hook);
        // Simulate dlsym returning an external library symbol first:
        void* ret1 = comshu::smart_dlsym::on_symbol_load(
            reinterpret_cast<void*>(external_fn), nullptr, "demo_self_guard_fn");
        assert(ret1 == hook_addr);
        assert(guard_pm.real_func<demo::self_guard_fn_tag>().load() == external_fn &&
               "external real symbol updates real_func");

        // Simulate dlsym(RTLD_DEFAULT, sym) returning our own hook_fn:
        void* ret2 =
            comshu::smart_dlsym::on_symbol_load(hook_addr, nullptr, "demo_self_guard_fn");
        assert(ret2 == hook_addr);
        assert(guard_pm.real_func<demo::self_guard_fn_tag>().load() == external_fn &&
               "self hook_fn must never overwrite real_func");
    }

    std::puts("hook_smoke OK");
    return 0;
}
