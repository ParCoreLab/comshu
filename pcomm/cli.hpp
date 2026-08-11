#pragma once
// The one command's subcommands.
//
// pcomm used to ship three binaries — `pcomm` (launch), `pcomm-read` (decode,
// export, query) and `pcomm-ui` (serve) — which is three tools to learn, three
// help texts that drifted, and an argv[1] that meant a *capture* directory in
// some invocations and an *export* directory in others. They are one binary and
// one verb table now. Nothing is kept as an alias: this tool has no installed
// base to protect, and a compatibility shim written before anyone depends on it
// is a shim nobody ever deletes.
//
// Each verb keeps its own parsing, in the file that implements it. The
// dispatcher's only job is picking one — it does not learn the flags of things
// it dispatches to, which is what stops it from slowly becoming a fourth place
// where the command line is defined.
//
// Every entry point takes argv with the VERB as argv[0] (dispatch passes
// `argc - 1, argv + 1`), so a verb's own usage text can print argv[0] and be
// right without knowing how it was reached.

// Defined by the build when the feature is present; defaulted here so every
// file can test them with #if and plain `if` rather than #ifdef, which is how a
// verb quietly disappears from a help text without anyone noticing.
#ifndef PCOMM_HAVE_DUCKDB
#define PCOMM_HAVE_DUCKDB 0
#endif
#ifndef PCOMM_HAVE_UI
#define PCOMM_HAVE_UI 0
#endif

namespace pcomm::cli {

// pcomm run / pcomm list — launch a workload under the selected providers, or
// describe what can be selected. See pcomm/run.cpp.
int run_main(int argc, char** argv);

// pcomm read / dump / export / link / sql / types — everything that reads a
// finished capture. See pcomm-read/main.cpp.
int read_main(int argc, char** argv);

#if PCOMM_HAVE_UI
// pcomm ui — serve a capture as a queryable page. See pcomm-ui/main.cpp.
// Only compiled into a DuckDB build: serving a capture is querying it.
int ui_main(int argc, char** argv);
#endif

}  // namespace pcomm::cli
