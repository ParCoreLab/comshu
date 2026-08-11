// pcomm — the launcher (successor of SigmaTrace's Profiler in spirit): resolves
// providers by short name, plans how to run them, and drives the workload.
//
// Providers co-run by default: they share one process, one recorder and one
// timeline, all hosted by libpcomm_engine.so (which the launcher preloads first
// because it owns the dlsym interposer). A provider only gets its own pass if
// it is marked `exclusive` — a demonstrated conflict, not a suspected one. When
// a selection mixes the two, pcomm *replays*: it re-runs the workload once per
// exclusive provider and drops every pass under one output dir, wall-aligned so
// pcomm-read stitches them together. Replay is the fallback, not the default:
// its passes are separate executions and cannot carry cross-transport
// causality.
//
//   pcomm run -p ib,p2p -- ./app args...  # co-run what can co-run, replay the rest
//   pcomm list                            # what can I capture, and how?
//
#include <cerrno>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "cli.hpp"

namespace fs = std::filesystem;

namespace {

// ── Provider registry ─────────────────────────────────────────────────────
// One row per capture engine. `aliases[0]` is the short name we print; the rest
// are accepted on the command line. `exclusive` = must own the process → gets
// its own replay pass. Prefer co-running: replay re-executes the workload, so
// its passes are different executions and cannot express cross-transport
// causality (this GPU peer-write caused that IB send). Mark a provider
// exclusive only for a demonstrated, un-fixable conflict — not a suspected one.
struct Provider {
    const char*              canonical;  // matches ProviderInfo::name
    const char*              so;         // library basename
    std::vector<std::string> aliases;    // short, then long
    bool                     exclusive;
    bool                     built;  // false = planned, not yet shipped
    const char*              what;   // one-line capability blurb
    const char*              scope;  // "inter-node" / "intra-node" / "host"
};

const std::vector<Provider>& registry() {
    static const std::vector<Provider> r = {
        {"ibverbs",
         "libibverbs_trace.so",
         {"ib", "net", "ibverbs"},
         /*exclusive*/ false,
         /*built*/ true,
         "InfiniBand wire traffic — QPs, sends, completions (host-side verbs)",
         "inter-node"},
        // No longer exclusive as of 7a5b55a. It was marked so because
        // dual-preload with ibverbs segfaulted, which we attributed to NVBit
        // fighting for dlsym — wrong on both counts: NVBit only *imports* dlsym,
        // and the crash was two embedded copies of the engine sharing a Tracer
        // singleton through weak-symbol interposition. With that fixed (and
        // gpumem's DT_RPATH turned into DT_RUNPATH so load order stops
        // mattering), gpumem co-runs in the shared pass — which is what makes one
        // unified NVLink+IB timeline possible at all, since replay's two
        // executions can't carry cross-transport causality. Case 06 is the
        // regression test.
        {"gpumem",
         "libgpumem_trace.so",
         {"p2p", "nvlink", "gpumem", "snoopie"},
         /*exclusive*/ false,
         /*built*/ true,
         "GPU-initiated peer read/write over NVLink (NVBit, à la Snoopie)",
         "intra-node"},
        // Separate from ibverbs rather than folded into it, because it captures
        // a different plane rather than more of the same one. IBGDA never calls
        // ibv_create_qp, ibv_modify_qp or ibv_post_send — it drives DEVX
        // directly and the GPU rings the doorbell — so a GPU-initiated run
        // traced with `-p ib` comes back with empty tables and no error. The two
        // co-run happily: a workload that uses both verbs and DEVX needs both.
        {"mlx5dv",
         "libmlx5dv_trace.so",
         {"devx", "mlx5", "mlx5dv", "ibgda"},
         /*exclusive*/ false,
         /*built*/ true,
         "GPU-initiated InfiniBand (IBGDA) — DEVX queue pairs, doorbell pages",
         "inter-node"},
        {"cupti",
         "libcupti_trace.so",
         {"memcpy", "cupti"},
         /*exclusive*/ true,
         /*built*/ false,
         "host-driven cudaMemcpy / copy-engine transfers (CUPTI)",
         "host"},
    };
    return r;
}

const Provider* lookup(std::string_view sel) {
    for (const auto& p : registry())
        for (const auto& a : p.aliases)
            if (a == sel)
                return &p;
    return nullptr;
}

// Directory the pcomm binary lives in — providers ship beside it. /proc/self/exe
// only: it is accurate under any argv[0], and argv[0] is no longer the program
// name here anyway (dispatch hands each verb its own name in that slot). On a
// host without /proc this stays empty and find_so simply drops those two roots,
// leaving PCOMM_PROVIDER_PATH — which is what such a host has to set regardless.
fs::path g_exe_dir;
void     init_exe_dir() {
    std::error_code ec;
    fs::path        self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec)
        g_exe_dir = self.parent_path();
}

// Provider .so files are searched in PCOMM_PROVIDER_PATH, then next to the
// launcher binary (the build tree), then in ../lib/pcomm relative to it (an
// installed tree). The last root is what lets an installed `pcomm` find its
// providers with no environment at all: bin/pcomm -> lib/pcomm/*.so.
std::string find_so(const Provider& p) {
    std::vector<fs::path> roots;
    // PCOMM_PROVIDER_PATH is a colon-list (providers may build into separate
    // per-provider dirs), searched before the launcher's own directory.
    if (const char* e = std::getenv("PCOMM_PROVIDER_PATH")) {
        std::string_view list = e;
        for (size_t s = 0; s <= list.size();) {
            size_t c = list.find(':', s);
            auto   seg = list.substr(s, c == std::string_view::npos ? c : c - s);
            if (!seg.empty())
                roots.emplace_back(seg);
            if (c == std::string_view::npos)
                break;
            s = c + 1;
        }
    }
    if (!g_exe_dir.empty()) {
        roots.emplace_back(g_exe_dir);
        roots.emplace_back(g_exe_dir.parent_path() / "lib" / "pcomm");
    }
    for (const auto& root : roots)
        if (fs::exists(root / p.so))
            return (root / p.so).string();
    std::fprintf(stderr, "pcomm: %s provider library not found (%s)\n", p.aliases[0].c_str(), p.so);
    if (!p.built)
        std::fprintf(stderr, "       (%s is planned, not yet built — see `pcomm list`)\n", p.canonical);
    std::exit(1);
}

// ── A pass = one execution of the workload with one preload set ────────────
struct Pass {
    std::string              label;     // output subdir + banner name
    std::vector<std::string> so_paths;  // LD_PRELOAD chain for this pass
};

void list_providers() {
    std::puts("pcomm providers — pick with -p <name>[,<name>...]\n");
    std::puts("  name     scope        capture");
    std::puts("  ───────  ───────────  ─────────────────────────────────────────────");
    for (const auto& p : registry()) {
        std::printf("  %-7s  %-11s  %s%s\n", p.aliases[0].c_str(), p.scope, p.what,
                    p.built ? "" : "  [planned]");
        std::printf("  %-7s  %-11s  aliases: ", "", "");
        for (size_t i = 0; i < p.aliases.size(); ++i)
            std::printf("%s%s", p.aliases[i].c_str(), i + 1 < p.aliases.size() ? ", " : "");
        std::printf("%s\n\n", p.exclusive ? "   · runs alone (replayed)" : "");
    }
    std::puts("Providers that must own the runtime run in their own pass: pcomm "
              "re-runs");
    std::puts("the workload once each and merges the passes under one -o directory.");
}

// The engine's own absolute path, for the LD_PRELOAD chain.
//
// dlopen'd rather than linked, and RTLD_LOCAL rather than global. The engine
// EXPORTS `dlsym` — that is its whole job — so a DT_NEEDED on it would put the
// interposer into the global symbol scope of every `pcomm` invocation,
// including `pcomm read` and `pcomm ui`, which have no business resolving
// symbols through a capture hook (`ui` links DuckDB, which dlopens). RTLD_LOCAL
// keeps it out of that scope, and loading it here means it is not loaded at all
// unless a run actually needs it.
//
// The path still comes from the engine's own pcomm_engine_so_path(), which is a
// dladdr on itself: the authoritative answer, not a search that has to be kept
// in step with the build and install layouts.
std::string engine_so_path() {
    // $ORIGIN/../lib/pcomm (installed) and the build tree both come from this
    // binary's RUNPATH, so a bare soname is the right thing to ask for.
    void* h = dlopen("libpcomm_engine.so", RTLD_LAZY | RTLD_LOCAL);
    if (!h)
        return {};
    using PathFn = const char* (*)();
    auto fn = reinterpret_cast<PathFn>(dlsym(h, "pcomm_engine_so_path"));
    const char* p = fn ? fn() : nullptr;
    return p ? std::string(p) : std::string();
}

void usage() {
    std::fputs("usage: pcomm run -p <name>[,<name>...] [options] -- <app> [args...]\n"
               "\n"
               "  -p, --providers L  comma-separated providers (short names; see "
               "`pcomm list`)\n"
               "  -o, --output DIR   trace output directory (default: pcomm-out)\n"
               "  -v, --verbose      verbose provider logging\n"
               "      --list         show available providers and exit (= pcomm list)\n"
               "      --dry-run      print the run plan without executing\n"
               "      --enable-doorbell-hijack\n"
               "                     capture the IBGDA data plane by redirecting the\n"
               "                     GPU's doorbell page (mlx5dv provider). Sits IN the\n"
               "                     application's data path and PERTURBS its timing —\n"
               "                     never draw a latency conclusion from such a run.\n"
               "  -h, --help         this message\n"
               "\n"
               "  e.g.  pcomm run -p ib -- ./app      # network only, single run\n"
               "        pcomm run -p ib,p2p -- ./app  # network co-runs; NVLink "
               "replayed\n",
               stderr);
}

}  // namespace

int pcomm::cli::run_main(int argc, char** argv) {
    // argv[0] is the verb. `pcomm list` is `pcomm run --list` without the
    // ceremony of a run that is not going to happen.
    if (argc >= 1 && std::string_view(argv[0]) == "list") {
        list_providers();
        return 0;
    }
    init_exe_dir();
    std::vector<const Provider*> selected;
    std::string                  output_dir = "pcomm-out";
    bool                         verbose = false, dry_run = false;
    bool                         doorbell_hijack = false;
    std::vector<char*>           target;

    auto add_selector = [&](std::string_view csv) {
        size_t start = 0;
        while (start <= csv.size()) {
            size_t comma = csv.find(',', start);
            auto   tok = csv.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                           : comma - start);
            if (!tok.empty()) {
                const Provider* p = lookup(tok);
                if (!p) {
                    std::fprintf(stderr, "pcomm: unknown provider '%.*s' (see `pcomm list`)\n",
                                 (int)tok.size(), tok.data());
                    std::exit(2);
                }
                // de-dupe (aliases could resolve to the same provider twice)
                bool seen = false;
                for (auto* s : selected)
                    seen |= (s == p);
                if (!seen)
                    selected.push_back(p);
            }
            if (comma == std::string_view::npos)
                break;
            start = comma + 1;
        }
    };

    int i = 1;
    for (; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--") {
            ++i;
            break;
        } else if ((a == "-p" || a == "--providers") && i + 1 < argc)
            add_selector(argv[++i]);
        else if ((a == "-o" || a == "--output") && i + 1 < argc)
            output_dir = argv[++i];
        else if (a == "-v" || a == "--verbose")
            verbose = true;
        else if (a == "--dry-run")
            dry_run = true;
        else if (a == "--enable-doorbell-hijack")
            doorbell_hijack = true;
        else if (a == "--list") {
            list_providers();
            return 0;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        }
        // back-compat with the pre-registry flags
        else if (a == "--load-gpumem")
            add_selector("gpumem");
        else if (a == "--load-ibtrace")
            add_selector("ibverbs");
        else
            break;  // first non-flag = start of target command
    }
    for (; i < argc; ++i)
        target.push_back(argv[i]);

    if (selected.empty()) {
        std::fputs("pcomm: no providers selected — use -p (see `pcomm list`)\n", stderr);
        return 1;
    }
    if (target.empty()) {
        std::fputs("pcomm: no target command (expected '-- <app> ...')\n", stderr);
        return 1;
    }
    target.push_back(nullptr);

    // ── Plan the passes ─────────────────────────────────────────────────────
    // Non-exclusive providers all share one pass (one dlsym owner hosts them);
    // each exclusive provider is its own pass. Multiple passes ⇒ replay.
    std::vector<Pass> passes;
    Pass              shared{"", {}};
    std::string       shared_label;
    for (const Provider* p : selected) {
        if (p->exclusive) {
            passes.push_back({p->canonical, {find_so(*p)}});
        } else {
            shared.so_paths.push_back(find_so(*p));
            shared_label += (shared_label.empty() ? "" : "+");
            shared_label += p->aliases[0];
        }
    }
    if (!shared.so_paths.empty()) {
        shared.label = shared_label;
        passes.insert(passes.begin(), std::move(shared));
    }

    const bool replay = passes.size() > 1;

    // The engine goes FIRST in every chain. It is not enough that the providers
    // link it: the dynamic linker searches preloaded objects before the
    // executable's own dependencies, and `dlsym` lives in the engine. Left as a
    // mere DT_NEEDED of a preloaded provider it loses the `dlsym` lookup to
    // libc, the SmartDlsym interposer never runs, and anything the target
    // resolves at runtime goes uncaptured (NVSHMEM resolves its verbs entry
    // points that way — it cost 12 of 16 memory regions before this was caught).
    std::string engine_so;
    if (const char* e = std::getenv("PCOMM_ENGINE_PATH"); e && *e)
        engine_so = e;
    else
        engine_so = engine_so_path();
    if (engine_so.empty())
        std::fprintf(stderr, "pcomm: warning — cannot locate libpcomm_engine.so; symbols the "
                             "target resolves via dlsym will not be captured. Set "
                             "PCOMM_ENGINE_PATH.\n");

    // ── Report the plan ─────────────────────────────────────────────────────
    std::fprintf(stderr, "pcomm: %zu provider%s → %zu %s\n", selected.size(),
                 selected.size() == 1 ? "" : "s", passes.size(),
                 replay ? "passes (replay)" : "pass");
    for (const auto& pass : passes) {
        // Single pass writes straight to -o; replay isolates each under -o/<label>.
        fs::path out = replay ? fs::path(output_dir) / pass.label : fs::path(output_dir);
        std::fprintf(stderr, "  · %-12s → %s\n", pass.label.c_str(), out.c_str());
    }
    if (replay)
        std::fprintf(stderr,
                     "pcomm: note — the workload runs %zu times (exclusive providers can't\n"
                     "       share a process); passes are wall-aligned for pcomm-read.\n",
                     passes.size());

    if (dry_run) {
        for (const auto& pass : passes) {
            std::fprintf(stderr, "  [%s] LD_PRELOAD=", pass.label.c_str());
            if (!engine_so.empty())
                std::fprintf(stderr, "%s:", engine_so.c_str());
            for (size_t j = 0; j < pass.so_paths.size(); ++j)
                std::fprintf(stderr, "%s%s", pass.so_paths[j].c_str(),
                             j + 1 < pass.so_paths.size() ? ":" : "");
            std::fputc('\n', stderr);
        }
        return 0;
    }

    // Preserve a caller-supplied LD_PRELOAD as the tail of every chain.
    std::string prev_preload;
    if (const char* prev = std::getenv("LD_PRELOAD"); prev && *prev)
        prev_preload = prev;

    auto set_pass_env = [&](const Pass& pass) {
        std::string preload = engine_so;  // first: it owns dlsym (see above)
        for (const auto& s : pass.so_paths)
            preload += (preload.empty() ? "" : ":") + s;
        if (!prev_preload.empty())
            preload += ":" + prev_preload;
        fs::path out = replay ? fs::path(output_dir) / pass.label : fs::path(output_dir);
        setenv("LD_PRELOAD", preload.c_str(), 1);
        setenv("PCOMM_OUTPUT_DIR", out.c_str(), 1);
        setenv("PCOMM_VERBOSE", verbose ? "1" : "0", 1);
        // The mlx5dv provider reads this and arms the UAR doorbell hijack. Only
        // that provider acts on it; harmless in a pass without it.
        if (doorbell_hijack)
            setenv("PCOMM_DOORBELL_HIJACK", "1", 1);
    };

    // ── Single pass: exec directly (no wrapper process left behind) ─────────
    if (!replay) {
        set_pass_env(passes[0]);
        execvp(target[0], target.data());
        std::perror("pcomm: execvp");
        return 127;
    }

    // ── Replay: fork+exec each pass in turn, aggregate exit status ──────────
    int worst = 0;
    for (size_t k = 0; k < passes.size(); ++k) {
        const auto& pass = passes[k];
        std::fprintf(stderr, "\npcomm: ── pass %zu/%zu [%s] ──\n", k + 1, passes.size(),
                     pass.label.c_str());
        pid_t pid = fork();
        if (pid < 0) {
            std::perror("pcomm: fork");
            return 127;
        }
        if (pid == 0) {
            set_pass_env(pass);
            execvp(target[0], target.data());
            std::perror("pcomm: execvp");
            _exit(127);
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        int code = WIFEXITED(status) ? WEXITSTATUS(status)
                                     : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        std::fprintf(stderr, "pcomm: pass [%s] exit=%d\n", pass.label.c_str(), code);
        if (code != 0)
            worst = code;
    }
    std::fprintf(stderr,
                 "\npcomm: %zu passes complete → %s\n"
                 "       read merged: pcomm read %s\n",
                 passes.size(), output_dir.c_str(), output_dir.c_str());
    return worst;
}
