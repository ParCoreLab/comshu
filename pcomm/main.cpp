// pcomm — the launcher (successor of SigmaTrace's Profiler in spirit): resolves
// providers by short name, plans how to run them, and drives the workload.
//
// Some providers can share one process (the hook-based network ones all route
// through a single dlsym interposer); others must own the whole runtime (NVBit
// takes over the CUDA runtime + dlsym, so it can't share). When a selection
// mixes the two, one process can't host them — so pcomm *replays*: it re-runs
// the workload once per exclusive provider and drops every pass under one
// output dir, wall-clock-aligned so pcomm-read stitches them into one picture.
//
//   pcomm -p ib,p2p -- ./app args...      # co-run what can co-run, replay the rest
//   pcomm --list                          # what can I capture, and how?
//
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ── Provider registry ─────────────────────────────────────────────────────
// One row per capture engine. `aliases[0]` is the short name we print; the rest
// are accepted on the command line. `exclusive` = must own the process (can't
// share dlsym / the CUDA runtime with anything else) → gets its own replay pass.
struct Provider {
    const char*                     canonical;   // matches ProviderInfo::name
    const char*                     so;          // library basename
    std::vector<std::string>        aliases;     // short, then long
    bool                            exclusive;
    bool                            built;        // false = planned, not yet shipped
    const char*                     what;         // one-line capability blurb
    const char*                     scope;        // "inter-node" / "intra-node" / "host"
};

const std::vector<Provider>& registry() {
    static const std::vector<Provider> r = {
        {"ibverbs", "libibverbs_trace.so",
         {"ib", "net", "ibverbs"}, /*exclusive*/false, /*built*/true,
         "InfiniBand wire traffic — QPs, sends, completions (host-side verbs)",
         "inter-node"},
        {"gpumem", "libgpumem_trace.so",
         {"p2p", "nvlink", "gpumem", "snoopie"}, /*exclusive*/true, /*built*/true,
         "GPU-initiated peer read/write over NVLink (NVBit, à la Snoopie)",
         "intra-node"},
        {"cupti", "libcupti_trace.so",
         {"memcpy", "cupti"}, /*exclusive*/true, /*built*/false,
         "host-driven cudaMemcpy / copy-engine transfers (CUPTI)",
         "host"},
    };
    return r;
}

const Provider* lookup(std::string_view sel) {
    for (const auto& p : registry())
        for (const auto& a : p.aliases)
            if (a == sel) return &p;
    return nullptr;
}

// Directory the launcher binary lives in — providers ship beside it. Prefer
// /proc/self/exe (accurate under any argv[0]); fall back to argv[0] on hosts
// without /proc (BSD/macOS dev boxes) so resolution never throws.
fs::path g_exe_dir;
void init_exe_dir(const char* argv0) {
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    g_exe_dir = ec ? fs::path(argv0).parent_path() : self.parent_path();
}

// Provider .so files are searched next to the launcher binary, then in
// PCOMM_PROVIDER_PATH.
std::string find_so(const Provider& p) {
    std::vector<fs::path> roots;
    // PCOMM_PROVIDER_PATH is a colon-list (providers may build into separate
    // per-provider dirs), searched before the launcher's own directory.
    if (const char* e = std::getenv("PCOMM_PROVIDER_PATH")) {
        std::string_view list = e;
        for (size_t s = 0; s <= list.size();) {
            size_t c = list.find(':', s);
            auto seg = list.substr(s, c == std::string_view::npos ? c : c - s);
            if (!seg.empty()) roots.emplace_back(seg);
            if (c == std::string_view::npos) break;
            s = c + 1;
        }
    }
    if (!g_exe_dir.empty()) roots.emplace_back(g_exe_dir);
    for (const auto& root : roots)
        if (fs::exists(root / p.so)) return (root / p.so).string();
    std::fprintf(stderr, "pcomm: %s provider library not found (%s)\n",
                 p.aliases[0].c_str(), p.so);
    if (!p.built)
        std::fprintf(stderr, "       (%s is planned, not yet built — see --list)\n",
                     p.canonical);
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
            std::printf("%s%s", p.aliases[i].c_str(),
                        i + 1 < p.aliases.size() ? ", " : "");
        std::printf("%s\n\n", p.exclusive ? "   · runs alone (replayed)" : "");
    }
    std::puts("Providers that must own the runtime run in their own pass: pcomm re-runs");
    std::puts("the workload once each and merges the passes under one -o directory.");
}

void usage() {
    std::fputs(
        "usage: pcomm -p <name>[,<name>...] [options] -- <app> [args...]\n"
        "\n"
        "  -p, --providers L  comma-separated providers (short names; see --list)\n"
        "  -o, --output DIR   trace output directory (default: pcomm-out)\n"
        "  -v, --verbose      verbose provider logging\n"
        "      --list         show available providers and exit\n"
        "      --dry-run      print the run plan without executing\n"
        "  -h, --help         this message\n"
        "\n"
        "  e.g.  pcomm -p ib -- ./app          # network only, single run\n"
        "        pcomm -p ib,p2p -- ./app      # network co-runs; NVLink replayed\n",
        stderr);
}

}  // namespace

int main(int argc, char** argv) {
    init_exe_dir(argv[0]);
    std::vector<const Provider*> selected;
    std::string output_dir = "pcomm-out";
    bool verbose = false, dry_run = false;
    std::vector<char*> target;

    auto add_selector = [&](std::string_view csv) {
        size_t start = 0;
        while (start <= csv.size()) {
            size_t comma = csv.find(',', start);
            auto tok = csv.substr(start, comma == std::string_view::npos
                                             ? std::string_view::npos
                                             : comma - start);
            if (!tok.empty()) {
                const Provider* p = lookup(tok);
                if (!p) {
                    std::fprintf(stderr, "pcomm: unknown provider '%.*s' (see --list)\n",
                                 (int)tok.size(), tok.data());
                    std::exit(2);
                }
                // de-dupe (aliases could resolve to the same provider twice)
                bool seen = false;
                for (auto* s : selected) seen |= (s == p);
                if (!seen) selected.push_back(p);
            }
            if (comma == std::string_view::npos) break;
            start = comma + 1;
        }
    };

    int i = 1;
    for (; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--") { ++i; break; }
        else if ((a == "-p" || a == "--providers") && i + 1 < argc) add_selector(argv[++i]);
        else if ((a == "-o" || a == "--output") && i + 1 < argc) output_dir = argv[++i];
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "--dry-run") dry_run = true;
        else if (a == "--list") { list_providers(); return 0; }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        // back-compat with the pre-registry flags
        else if (a == "--load-gpumem") add_selector("gpumem");
        else if (a == "--load-ibtrace") add_selector("ibverbs");
        else break;  // first non-flag = start of target command
    }
    for (; i < argc; ++i) target.push_back(argv[i]);

    if (selected.empty()) {
        std::fputs("pcomm: no providers selected — use -p (see --list)\n", stderr);
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
    Pass shared{"", {}};
    std::string shared_label;
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

    // ── Report the plan ─────────────────────────────────────────────────────
    std::fprintf(stderr, "pcomm: %zu provider%s → %zu %s\n",
                 selected.size(), selected.size() == 1 ? "" : "s",
                 passes.size(), replay ? "passes (replay)" : "pass");
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
            for (size_t j = 0; j < pass.so_paths.size(); ++j)
                std::fprintf(stderr, "%s%s", pass.so_paths[j].c_str(),
                             j + 1 < pass.so_paths.size() ? ":" : "");
            std::fputc('\n', stderr);
        }
        return 0;
    }

    // Preserve a caller-supplied LD_PRELOAD as the tail of every chain.
    std::string prev_preload;
    if (const char* prev = std::getenv("LD_PRELOAD"); prev && *prev) prev_preload = prev;

    auto set_pass_env = [&](const Pass& pass) {
        std::string preload;
        for (const auto& s : pass.so_paths) preload += (preload.empty() ? "" : ":") + s;
        if (!prev_preload.empty()) preload += ":" + prev_preload;
        fs::path out = replay ? fs::path(output_dir) / pass.label : fs::path(output_dir);
        setenv("LD_PRELOAD", preload.c_str(), 1);
        setenv("PCOMM_OUTPUT_DIR", out.c_str(), 1);
        setenv("PCOMM_VERBOSE", verbose ? "1" : "0", 1);
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
        std::fprintf(stderr, "\npcomm: ── pass %zu/%zu [%s] ──\n",
                     k + 1, passes.size(), pass.label.c_str());
        pid_t pid = fork();
        if (pid < 0) { std::perror("pcomm: fork"); return 127; }
        if (pid == 0) {
            set_pass_env(pass);
            execvp(target[0], target.data());
            std::perror("pcomm: execvp");
            _exit(127);
        }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        int code = WIFEXITED(status) ? WEXITSTATUS(status)
                                     : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        std::fprintf(stderr, "pcomm: pass [%s] exit=%d\n", pass.label.c_str(), code);
        if (code != 0) worst = code;
    }
    std::fprintf(stderr,
                 "\npcomm: %zu passes complete → %s\n"
                 "       read merged: pcomm-read %s\n",
                 passes.size(), output_dir.c_str(), output_dir.c_str());
    return worst;
}
