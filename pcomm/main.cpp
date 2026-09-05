// pcomm — the one entry point. Picks a verb and gets out of the way.
//
// This file knows the verb NAMES and nothing else about them: no flags, no
// directory shapes, no output formats. That is deliberate. A dispatcher that
// starts validating its subcommands' arguments becomes a second place where the
// command line is defined, and the two drift.
#include <cstdio>
#include <cstring>
#include <string_view>

#include "baked.hpp"
#include "cli.hpp"
#include "pcomm/engine_abi.h"
#include "pcomm/rec/stream_header.hpp"
// Generated per build by cmake/write_build_id.cmake — see pcomm/CMakeLists.txt.
#include "pcomm_build_id.h"

namespace {

struct Verb {
    const char* name;
    const char* args;
    const char* what;
    bool        available;  // false = this build cannot do it, and says why below
};

// Order is the order they print, which is the order you use them in: launch a
// run, look at what it captured, export it, query it, serve it.
constexpr Verb kVerbs[] = {
    {"run", "-p <list> [-o DIR] -- <app> [args]", "run a workload under the selected providers",
     true},
    {"list", "", "the providers this build can select, and what each captures", true},
    {"read", "<capture>", "stream inventory: what is in a capture directory", true},
    {"dump", "<capture> <type-id>", "decode one raw stream to stdout", true},
    {"export", "<capture> --<format> <out>",
     "write the tables out: --csv, --parquet, --perfetto, --pftrace", true},
    {"link", "<export>", "derive the rank-space tables over an export", PCOMM_HAVE_DUCKDB},
    {"sql", "<export> \"<query>\"", "query an export directly", PCOMM_HAVE_DUCKDB},
    {"types", "", "type-id -> table name, for this build", true},
    {"bake", "-o <new binary> <binary> <lib.so>...", "carry the providers inside the launcher",
     true},
    {"ui", "<capture|export> [--port N]", "serve the capture as a page (127.0.0.1)", PCOMM_HAVE_UI},
};

void usage() {
    std::fputs(
        "pcomm — capture and read GPU/network communication\n\nusage: pcomm <command> [args]\n\n",
        stderr);
    for (const auto& v : kVerbs) {
        std::fprintf(stderr, "  %-7s %-34s %s%s\n", v.name, v.args, v.what,
                     v.available ? "" : "   [not in this build]");
    }
    std::fputs("\n  pcomm <command> with no arguments prints that command's own usage.\n"
               "  pcomm --version prints the build id a capture will report.\n",
               stderr);
#if !PCOMM_HAVE_DUCKDB
    std::fputs("\n  This build has no DuckDB, so link/sql/ui are absent. To get them:\n"
               "    tools/get_duckdb.sh && cmake -B build -DPCOMM_DUCKDB=ON\n"
               "  (needs network; MN5 has none — `export --csv` is the path there.)\n",
               stderr);
#endif
}

// The old command line was `pcomm -p ib -- ./app`, with no verb. Recognising it
// costs three lines and turns "unknown command '-p'" — which reads like a
// broken flag — into the one sentence that fixes it.
bool looks_like_the_old_launcher(std::string_view a) {
    return a == "-p" || a == "--providers" || a == "-o" || a == "--output" || a == "--" ||
           a == "--list" || a == "--dry-run";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage();
        return 0;
    }

    // WORD FOR WORD THE STRING `Invocation.build` CARRIES (engine.cpp), because
    // the point is to compare them: a capture says which build produced it, and
    // until now nothing could ask a binary the same question without running a
    // workload first. On stdout, single line, so a script can read it.
    if (cmd == "--version" || cmd == "version") {
        std::printf("commit=%s type=%s abi=%u schema=0x%016llX\n", PCOMM_BUILD_COMMIT,
                    PCOMM_BUILD_TYPE, static_cast<unsigned>(PCOMM_ENGINE_ABI_VERSION),
                    static_cast<unsigned long long>(pcomm::rec::kSchemaHash));
        return 0;
    }

    // Verbs are dispatched with the verb itself as argv[0], so each one's usage
    // text names the way it was actually invoked.
    const int    sub_argc = argc - 1;
    char** const sub_argv = argv + 1;

    if (cmd == "run" || cmd == "list")
        return pcomm::cli::run_main(sub_argc, sub_argv);
    if (cmd == "bake")
        return pcomm::baked::bake_main(sub_argc, sub_argv);
    if (cmd == "read" || cmd == "dump" || cmd == "export" || cmd == "link" || cmd == "sql" ||
        cmd == "types")
        return pcomm::cli::read_main(sub_argc, sub_argv);
    if (cmd == "ui") {
#if PCOMM_HAVE_UI
        return pcomm::cli::ui_main(sub_argc, sub_argv);
#else
        std::fputs("pcomm: this build has no `ui` — it needs DuckDB and a built frontend.\n"
                   "  tools/get_duckdb.sh && pnpm -C ui install && pnpm -C ui build\n"
                   "  cmake -B build -DPCOMM_DUCKDB=ON\n",
                   stderr);
        return 1;
#endif
    }

    if (looks_like_the_old_launcher(cmd)) {
        std::fprintf(stderr,
                     "pcomm: `%s` is a flag of `pcomm run`, not a command.\n"
                     "  did you mean:  pcomm run %s ...\n",
                     argv[1], argv[1]);
        return 2;
    }
    std::fprintf(stderr, "pcomm: unknown command '%s'\n\n", argv[1]);
    usage();
    return 2;
}
