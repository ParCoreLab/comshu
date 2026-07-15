// pcomm — the launcher (successor of SigmaTrace's Profiler in spirit): a thin
// process wrapper that resolves provider .so paths, assembles the LD_PRELOAD
// chain and PCOMM_* env, and execs the target.
//
//   pcomm [--load-gpumem] [--load-ibtrace] [-o DIR] [-v] -- ./app args...

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct Args {
    std::vector<std::string> providers;
    std::string output_dir = "pcomm-out";
    bool verbose = false;
    std::vector<char*> target;
};

void usage() {
    std::fputs(
        "usage: pcomm [options] -- <app> [app args...]\n"
        "  --load-gpumem     GPU memory-access provider (Snoopie engine)\n"
        "  --load-ibtrace    ibverbs provider\n"
        "  -o, --output DIR  trace output directory (default: pcomm-out)\n"
        "  -v, --verbose     verbose provider logging\n",
        stderr);
}

// Provider .so files are searched next to the launcher binary, then in
// PCOMM_PROVIDER_PATH.
std::string find_provider(const std::string& name) {
    namespace fs = std::filesystem;
    std::string so = "lib" + name + "_trace.so";
    std::vector<fs::path> roots;
    if (const char* p = std::getenv("PCOMM_PROVIDER_PATH")) roots.emplace_back(p);
    roots.emplace_back(fs::read_symlink("/proc/self/exe").parent_path());
    for (const auto& root : roots) {
        if (fs::exists(root / so)) return (root / so).string();
    }
    std::fprintf(stderr, "pcomm: provider %s not found (%s)\n", name.c_str(), so.c_str());
    std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    int i = 1;
    for (; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--") { ++i; break; }
        else if (a == "--load-gpumem") args.providers.emplace_back("gpumem");
        else if (a == "--load-ibtrace") args.providers.emplace_back("ibverbs");
        else if ((a == "-o" || a == "--output") && i + 1 < argc) args.output_dir = argv[++i];
        else if (a == "-v" || a == "--verbose") args.verbose = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else break;  // first non-flag = start of target command
    }
    for (; i < argc; ++i) args.target.push_back(argv[i]);
    args.target.push_back(nullptr);

    if (args.target.size() < 2) { usage(); return 1; }
    if (args.providers.empty()) {
        std::fputs("pcomm: no providers selected (see --help)\n", stderr);
        return 1;
    }

    std::string preload;
    for (const auto& p : args.providers) {
        if (!preload.empty()) preload += ':';
        preload += find_provider(p);
    }
    if (const char* prev = std::getenv("LD_PRELOAD"); prev && *prev) {
        preload += ':';
        preload += prev;
    }

    setenv("LD_PRELOAD", preload.c_str(), 1);
    setenv("PCOMM_OUTPUT_DIR", args.output_dir.c_str(), 1);
    setenv("PCOMM_VERBOSE", args.verbose ? "1" : "0", 1);
    // PCOMM_RANK is detected in-process (env guess: MPI/SLURM vars) — the
    // RankDetector port from SigmaTrace lands with the first provider.

    execvp(args.target[0], args.target.data());
    std::perror("pcomm: execvp");
    return 127;
}
