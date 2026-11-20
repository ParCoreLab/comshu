#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>

#include <dlfcn.h>
#include <vector>
#include <link.h>

#include <functional>
#include <string>

#include "comshu/hook/dlsym/SymbolResolver.h"

#include "comshu/hook/dlsym/Dlsym.hpp"

namespace comshu::smart_dlsym {
static constexpr bool LogDebug{false};

void* DL_RTLD_DEFAULT = RTLD_DEFAULT;

void*     DL_RTLD_NEXT = RTLD_NEXT;
const int DL_RTLD_NOW = RTLD_NOW;
const int DL_RTLD_GLOBAL = RTLD_GLOBAL;
const int DL_RTLD_NOLOAD = RTLD_NOLOAD;

void* (*real_dlopen)(const char*, int) = nullptr;
void* (*real_dlsym)(void*, const char*) = nullptr;
void* (*real_dlvsym)(void*, const char*, const char*) = nullptr;
void* (*real_dlclose)(void*) = nullptr;

// The registry: symbol name -> registrants, innermost first.
//
// ONE MUTEX, NEVER HELD ACROSS A CALLBACK (CR-11). dlsym is called from any
// thread, providers register from their constructors while the application may
// already be resolving symbols, and the map used to be read and written with no
// synchronisation at all. A lock held while the chain runs would deadlock the
// first registrant that resolves a symbol of its own, so a lookup copies the
// symbol's chain under the lock and runs it after releasing it.
//
// Created on first use and never destroyed: dlsym is called during static
// destruction, and from constructors that run before this file's.
using SymbolMap = std::unordered_map<std::string, std::vector<CallbackType>>;
static std::mutex& registry_mutex() {
    static std::mutex m;  // constant-initialised: safe from constructor(0)
    return m;
}
static SymbolMap& registry_locked() {  // caller holds registry_mutex()
    static SymbolMap* m = new SymbolMap();
    return *m;
}

void register_symbol(std::string symbol_name, CallbackType callback) {
    std::lock_guard<std::mutex> lk(registry_mutex());
    registry_locked()[std::move(symbol_name)].push_back(std::move(callback));
}

std::size_t registrant_count(const char* symbol_name) {
    if (!symbol_name)
        return 0;
    std::lock_guard<std::mutex> lk(registry_mutex());
    const auto&                 map = registry_locked();
    const auto                  it = map.find(symbol_name);
    return it == map.end() ? 0 : it->second.size();
}

// DIAGNOSTIC, gated on COMSHU_TRACE_DLSYM=1 — which vendor symbols did the
// application RESOLVE, and which of those do we not intercept?
//
// This exists because "a door we do not hook" has cost this project two long
// hunts, and both times the missing symbol was sitting in plain sight in a
// dlsym the application made. entity_close_accounted currently reports 33
// closes of memory regions nothing opened; the candidate set is whatever
// registration symbol NCCL resolves and we ignore.
//
// It reports RESOLUTION, not invocation. That distinction is the whole of the
// case 08 lesson — a symbol can bind and never be called — so this narrows the
// search and never concludes it. A name appearing here is a candidate; proving
// it is called still takes an entry trace.
//
// POSITIVE CONTROL: any line at all means the diagnostic ran. An earlier
// version of this comment claimed the control was the presence of a `hooked`
// line, which is wrong and would have been read as a broken instrument on its
// first use — job 44367535 printed 24 UNHOOKED lines and zero hooked ones,
// because the application resolved CUDA through dlsym and ibverbs by some other
// route. "No hooked lines" is a finding, not a malfunction.
static bool trace_dlsym_enabled() {
    static const bool on = [] {
        const char* v = std::getenv("COMSHU_TRACE_DLSYM");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

// Only the vendor surfaces comshu interposes. Everything else is libc noise and
// would bury the answer.
static bool trace_dlsym_interesting(const char* s) {
    return std::strncmp(s, "ibv_", 4) == 0 || std::strncmp(s, "mlx5dv_", 7) == 0 ||
           std::strncmp(s, "rdma_", 5) == 0 || std::strncmp(s, "cuMem", 5) == 0 ||
           std::strncmp(s, "cuGetProc", 9) == 0 || std::strncmp(s, "cuGetExport", 11) == 0;
}

// ENTRY trace, distinct from the resolution trace below. Called as the FIRST
// statement of dlsym/dlvsym, above init() and above every guard, because the
// question it answers is "is this interposer entered at all" and any guard
// placed above it answers a different one.
//
// It exists because four facts are established and inconsistent: NCCL resolves
// ibv_* through dlvsym (source + ELF), our dlvsym is exported and routes to
// on_symbol_load (readelf + the body), RegisteredSymbols is shared (the
// provider's symbol is UND), and job 44367535 traced zero ibv_* lookups on a
// run whose ibverbs hooks demonstrably fired. Five hypotheses died explaining
// that; this measures instead of guessing.
//
// POSITIVE CONTROL: the `<first entry>` line, printed once per function for
// whatever symbol arrives first, interesting or not. dlsym's line proves the
// instrument works. If dlsym prints it and dlvsym never does, our dlvsym is not
// being entered and the question becomes one about resolution — which is when
// LD_DEBUG finally becomes the right tool, because it would finally be a
// resolution question. If both print and only dlsym shows ibv_* names, the loss
// is downstream in on_symbol_load, whose null-check sits above its trace.
static void trace_dlsym_entry(const char* fn, const char* symbol_name, const char* version) {
    if (!trace_dlsym_enabled())
        return;
    static std::mutex           m;
    std::lock_guard<std::mutex> lk(m);

    static std::set<std::string> first_entry;
    if (first_entry.insert(fn).second) {
        std::fprintf(stderr, "[comshu-entry] %s <first entry> sym=%s\n", fn,
                     symbol_name ? symbol_name : "(null)");
        std::fflush(stderr);
    }

    if (!symbol_name || !trace_dlsym_interesting(symbol_name))
        return;
    // Keyed by function too: dlsym and dlvsym must not suppress each other's
    // line for the same symbol, since which one saw it is the whole question.
    static std::set<std::string> seen;
    if (!seen.insert(std::string(fn) + ":" + symbol_name).second)
        return;
    std::fprintf(stderr, "[comshu-entry] %-7s %-32s ver=%s\n", fn, symbol_name,
                 version ? version : "-");
    std::fflush(stderr);
}

static void trace_dlsym_lookup(const char* symbol_name, bool hooked) {
    if (!trace_dlsym_enabled() || !symbol_name || !trace_dlsym_interesting(symbol_name))
        return;
    // Once per distinct name: an application resolves the same symbol on every
    // reconnect and the repetition says nothing the first line did not.
    static std::mutex            m;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex>  lk(m);
    if (!seen.insert(symbol_name).second)
        return;
    std::fprintf(stderr, "[comshu-dlsym] %-32s %s\n", symbol_name, hooked ? "hooked" : "UNHOOKED");
    std::fflush(stderr);
}

void* on_symbol_load(void* real_symbol, void* handle, const char* symbol_name) {
    // A null name: there is nothing to look up, and std::string(nullptr) is
    // undefined behaviour (CR-11). (glibc itself segfaults on dlsym(h, NULL) —
    // measured, even for RTLD_DEFAULT — so this is defence, not a service.)
    if (!real_symbol || !symbol_name) {
        return real_symbol;
    }

    std::vector<CallbackType> chain;
    {
        std::lock_guard<std::mutex> lk(registry_mutex());
        const auto&                 map = registry_locked();
        const auto                  it = map.find(symbol_name);
        if (it != map.end())
            chain = it->second;
    }
    trace_dlsym_lookup(symbol_name, !chain.empty());
    if (chain.empty()) {
        return real_symbol;
    }

    // Walk the chain, each registrant wrapping the previous one. This is
    // ordinary interposition stacking, not a merge: a registrant is handed the
    // current front — what it must forward to in order to reach the real
    // implementation — and returns the new front. So the last registrant is
    // what the application receives, its replacement calls the one before it,
    // and the first registrant's calls land on the real symbol.
    void* fn = real_symbol;
    for (const auto& callback : chain) {
        void* next = callback(fn, handle, symbol_name);
        // A registrant that declines, or that fails to build a replacement,
        // must not be able to blank the symbol. The previous code returned the
        // callback's result unconditionally, so a null propagated to the
        // application as a failed resolution — which is exactly how a probe of
        // the mlx5dv interface once convinced NVSHMEM that UAR allocation had
        // failed and made it disable its entire transport. Declining leaves the
        // chain untouched.
        if (next) {
            fn = next;
        }
    }
    return fn;
}
}  // namespace comshu::smart_dlsym

// On glibc < 2.34, dl* functions live in libdl.so; on >= 2.34 they're in libc.so
static void* find_dl_symbol(const char* name) {
    void* sym = find_symbol_in_lib("libc", name);
    if (!sym) {
        sym = find_symbol_in_lib("libdl", name);
    }
    return sym;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wprio-ctor-dtor"
// If you see an undebuggable double free, however, it's this. Don't load this module more than once !!!!!
// Resolves the real dl* functions exactly once, whichever arrives first: this
// constructor, or a dlsym/dlvsym that runs ahead of it. Running it twice used to
// replace the registry and silently drop every registration made in between
// (CR-11).
static std::once_flag g_init_once;
static std::atomic<bool> g_init_done{false};
static void init_once();
static void __attribute__((constructor(0))) init() {
    std::call_once(g_init_once, init_once);
}
bool comshu::smart_dlsym::interposer_ready() {
    return g_init_done.load(std::memory_order_acquire);
}
static void init_once() {
    comshu::smart_dlsym::real_dlopen =
        reinterpret_cast<decltype(comshu::smart_dlsym::real_dlopen)>(find_dl_symbol("dlopen"));

    comshu::smart_dlsym::real_dlsym =
        reinterpret_cast<decltype(comshu::smart_dlsym::real_dlsym)>(find_dl_symbol("dlsym"));
    comshu::smart_dlsym::real_dlvsym =
        reinterpret_cast<decltype(comshu::smart_dlsym::real_dlvsym)>(find_dl_symbol("dlvsym"));

    comshu::smart_dlsym::real_dlclose =
        reinterpret_cast<decltype(comshu::smart_dlsym::real_dlclose)>(find_dl_symbol("dlclose"));

    if (!comshu::smart_dlsym::real_dlopen || !comshu::smart_dlsym::real_dlsym ||
        !comshu::smart_dlsym::real_dlvsym || !comshu::smart_dlsym::real_dlclose) {
        fprintf(stderr, "Failed to load real real dl functions\n");
        std::abort();
    }

    g_init_done.store(true, std::memory_order_release);
}
#pragma GCC diagnostic pop

// The libraries loaded AFTER the one containing `caller`, in search order, by
// path. Names are collected first and opened afterwards: dl_iterate_phdr holds
// the loader's lock, and dlopen inside its callback is asking for a deadlock.
static std::vector<std::string> libraries_after(void* caller) {
    struct Ctx {
        void*                    caller;
        bool                     passed = false;
        std::vector<std::string> names;
    } ctx{caller};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            auto* c = static_cast<Ctx*>(data);
            if (!c->passed) {
                for (size_t i = 0; i < info->dlpi_phnum; ++i) {
                    const auto& ph = info->dlpi_phdr[i];
                    if (ph.p_type != PT_LOAD)
                        continue;
                    const auto lo = info->dlpi_addr + ph.p_vaddr;
                    const auto a = reinterpret_cast<ElfW(Addr)>(c->caller);
                    if (a >= lo && a < lo + ph.p_memsz) {
                        c->passed = true;
                        break;
                    }
                }
                return 0;
            }
            if (info->dlpi_name && info->dlpi_name[0])
                c->names.emplace_back(info->dlpi_name);
            return 0;
        },
        &ctx);
    return ctx.names;
}

// dlvsym(RTLD_NEXT, sym, ver) relative to the real caller: the first library
// after it in which glibc finds THAT version. The version is asked of each
// library directly, so a symbol present there only as a compat version (@V1,
// no default) is found — the earlier route through find_symbol_next's
// default-version walk missed it and fell back to resolving from the engine,
// which hands an interposer its own definition (CR-10 audit).
static void* find_versioned_next(void* caller, const char* symbol, const char* version) {
    for (const std::string& name : libraries_after(caller)) {
        void* lib = comshu::smart_dlsym::real_dlopen(name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
        if (!lib)
            continue;
        void* sym = comshu::smart_dlsym::real_dlvsym(lib, symbol, version);
        comshu::smart_dlsym::real_dlclose(lib);
        if (sym)
            return sym;
    }
    return nullptr;
}

static void log_dlsym(const char* symbol, void* caller, void* result, void* glibc_result) {
    if constexpr (comshu::smart_dlsym::LogDebug) {
        Dl_info caller_info = {};
        Dl_info ours_info = {};
        Dl_info glibc_info = {};
        dladdr(caller, &caller_info);
        if (result)
            dladdr(result, &ours_info);
        if (glibc_result)
            dladdr(glibc_result, &glibc_info);
        fprintf(stderr, "[dlsym] RTLD_NEXT '%s' caller=%s ours=%s(%p) glibc=%s(%p)%s\n", symbol,
                caller_info.dli_fname ? caller_info.dli_fname : "?",
                ours_info.dli_fname ? ours_info.dli_fname : "(nil)", result,
                glibc_info.dli_fname ? glibc_info.dli_fname : "(nil)", glibc_result,
                (result && result != glibc_result) ? " MISMATCH" : "");
    }
}

extern "C" void* dlsym(void* handle, const char* symbol) {
    comshu::smart_dlsym::trace_dlsym_entry("dlsym", symbol,
                                          nullptr);  // FIRST. Before init(), before anything.
    init();  // once; a no-op after the first

    // Note[RTLD_NEXT]: RTLD_NEXT must resolve relative to the *original caller*, not us.
    // Forwarding real_dlsym(RTLD_NEXT, ...) would search after our library
    // instead of after the caller's, which can cause infinite recursion when
    // a wrapper library tries to find the real symbol behind it.
    // __builtin_return_address(0) must be called here, not in a helper,
    // to capture the actual caller of dlsym.
    // A null name goes to glibc untouched, so the program fails exactly as it
    // would without us (glibc segfaults on it) rather than in our ELF walk.
    if (!symbol)
        return comshu::smart_dlsym::real_dlsym(handle, symbol);
    if (handle == RTLD_NEXT) {
        void* caller = __builtin_return_address(0);
        void* result = find_symbol_next(caller, symbol);
        // Always call real_dlsym to let glibc populate its internal state
        // (lazy binding, symbol caches, etc.) even though we prefer our result.
        void* glibc_result = comshu::smart_dlsym::real_dlsym(handle, symbol);
        // ...but if glibc failed where we succeeded, its error must not
        // outlive a call that returned a symbol: `dlerror()` after a
        // successful dlsym is how callers tell a NULL symbol from a miss.
        if (result && !glibc_result)
            (void)dlerror();

        // Debug
        log_dlsym(symbol, caller, result, glibc_result);

        auto* real_symbol = result ? result : glibc_result;
        return comshu::smart_dlsym::on_symbol_load(real_symbol, handle, symbol);
    }

    auto* real_symbol = comshu::smart_dlsym::real_dlsym(handle, symbol);
    return comshu::smart_dlsym::on_symbol_load(real_symbol, handle, symbol);
}

extern "C" void* dlvsym(void* handle, const char* symbol, const char* version) {
    comshu::smart_dlsym::trace_dlsym_entry("dlvsym", symbol,
                                          version);  // FIRST. Before init(), before anything.
    init();  // once; a no-op after the first

    if (!symbol)
        return comshu::smart_dlsym::real_dlvsym(handle, symbol, version);

    // See Note[RTLD_NEXT]
    //
    // glibc's own answer is relative to the object that CALLED it, which here is
    // libcomshu_engine.so, not the application's library — so it skips every
    // object between the caller and us, and an interposer asking RTLD_NEXT for
    // the "real" function can be handed its own definition back. So the
    // requested VERSION is asked of each library after the real caller, in
    // order (CR-10), and glibc's engine-relative answer is only the fallback —
    // asked for only when that finds nothing, so it cannot leave a dlerror()
    // behind a call that succeeded.
    if (handle == RTLD_NEXT) {
        void* caller = __builtin_return_address(0);
        void* result = find_versioned_next(caller, symbol, version);
        void* glibc_result =
            result ? nullptr : comshu::smart_dlsym::real_dlvsym(handle, symbol, version);

        // Debug
        log_dlsym(symbol, caller, result, glibc_result);

        auto* real_symbol = result ? result : glibc_result;
        return comshu::smart_dlsym::on_symbol_load(real_symbol, handle, symbol);
    }

    auto* real_symbol = comshu::smart_dlsym::real_dlvsym(handle, symbol, version);
    return comshu::smart_dlsym::on_symbol_load(real_symbol, handle, symbol);
}
