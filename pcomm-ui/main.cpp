// pcomm ui — serve an exported capture as a queryable page.
//
//   pcomm-ui <capture-or-export-dir> [--port N] [--bind ADDR] [--dir <static-dir>]
//
// The export directory is one produced by `pcomm-read --parquet` and linked
// with `--link`. This process never opens a `.pcs`: stream headers, schema
// hashes, stride checks, version mapping and intern resolution all live in
// pcomm-read, and duplicating any of it here is how two readers start
// disagreeing about what a capture says.
//
// **One data path.** Everything the page knows arrives through POST /query.
// There is no second endpoint that computes something server-side, and there
// will not be one: the moment a panel gets its own bespoke endpoint, the
// derived layer forks and two panels disagree about total bytes. It is also
// what keeps the static/DuckDB-WASM mode reachable — swapping the transport
// under one function is a different job from finding every fetch in a UI.
//
// Bound to loopback. This serves a local capture to the person who ran it,
// reached from a cluster through an SSH port-forward; it is not a service, has
// no authentication, and must not acquire either by accident.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

extern "C" {
#include "duckdb.h"
}
#include "httplib.h"

#include "duck_mount.hpp"    // shared with pcomm-read: the empty-table policy
#include "capture_load.hpp"  // ... and reading a raw capture straight in
#include "link_sql.hpp"      // ... and the derived layer, as views
#include "ui_assets.hpp"     // generated from ui/dist: the built app, embedded

namespace fs = std::filesystem;

namespace {

// Read-only, row-capped, and that is the contract the chat panel will later
// depend on. Enforced here rather than trusted from the caller: the page is not
// the only thing that can POST to this port.
constexpr int         kRowCap = 100000;
constexpr const char* kReadOnlyErr =
    "this endpoint answers SELECT only; use pcomm-read to write anything";

// Cheap first-token check, and it is the ONLY thing stopping a write — an
// in-memory DuckDB cannot be opened in READ_ONLY access mode, so there is no
// engine-level guard behind this one. Stated plainly because a comment claiming
// a safety property the code does not have is worse than no comment.
//
// What actually bounds the risk is the trust boundary: the server binds
// loopback and serves one local capture to the person who started it, so a
// caller who can POST here can already run pcomm-read. Note the same reasoning
// covers the larger hole — DuckDB's own table functions mean a SELECT can read
// any file the user can (`read_csv('/etc/passwd')`). That is acceptable at
// loopback and would NOT be if this ever grew a bind address, which is the
// moment to add `SET enable_external_access=false` and a real parser.
//
// It has now grown one (`--bind`), so that condition is honoured below: binding
// anywhere other than loopback turns external access OFF and refuses to serve a
// Parquet export at all. See bind_is_loopback / harden_for_network.
bool looks_like_select(const std::string& sql) {
    const std::size_t i = sql.find_first_not_of(" \t\r\n");
    if (i == std::string::npos)
        return false;
    const std::string head = sql.substr(i);
    return strncasecmp(head.c_str(), "SELECT", 6) == 0 || strncasecmp(head.c_str(), "WITH", 4) == 0;
}

std::string json_escape(const char* s) {
    std::string out;
    for (const char* p = s; p && *p; ++p) {
        switch (*p) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += *p;
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: pcomm-ui <dir> [--port N] [--bind ADDR] [--dir <static-dir>]\n"
                   "  <dir> is either a capture (one containing rank-* dirs) or a\n"
                   "  `pcomm-read --parquet` export, ideally --link'ed\n"
                   "  the frontend is built in; --dir serves one from disk instead,\n"
                   "  for iterating on ui/ without rebuilding this binary\n",
                   stderr);
        return 1;
    }
    fs::path    dir = argv[1];
    int         port = 8777;
    std::string static_dir;
    std::string bind_addr = "127.0.0.1";
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--port") == 0)
            port = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--dir") == 0)
            static_dir = argv[i + 1];
        else if (std::strcmp(argv[i], "--bind") == 0)
            bind_addr = argv[i + 1];
    }
    const bool loopback =
        bind_addr == "127.0.0.1" || bind_addr == "localhost" || bind_addr == "::1";
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "pcomm-ui: not a directory: %s\n", dir.c_str());
        return 1;
    }

    duckdb_database   db{};
    duckdb_connection con{};
    if (duckdb_open(nullptr, &db) != DuckDBSuccess || duckdb_connect(db, &con) != DuckDBSuccess) {
        std::fprintf(stderr, "pcomm-ui: cannot open DuckDB\n");
        return 1;
    }

    // Raw record tables first, through the SAME policy `--link` uses: a stream
    // this capture lacks becomes an EMPTY table with the right columns, not a
    // missing one. Without that, a panel querying `mlx5_device_send` fails on
    // every verbs-only capture, and every panel would have to branch on which
    // provider ran — the coupling the derived layer exists to remove.
    // Two kinds of directory, told apart by whether it holds rank-* dirs.
    //
    // A capture is loaded in full and the derived layer defined as views, so
    // `pcomm-ui <capture>` works with no export step. That is the common case —
    // needing `--parquet` and then `--link` before you could look at a run made
    // the quick question ("what does this capture look like?") the slow one.
    //
    // An export is still mounted as before: it is what the suite produces, and
    // re-decoding streams that have already been decoded would be slower and
    // would give a second answer to a question that already has one.
    std::size_t tables = 0;
    const bool  from_capture = pcomm::capture::looks_like_capture(dir);
    if (from_capture) {
        const auto ranks = pcomm::capture::scan_ranks(dir);
        if (ranks.empty()) {
            std::fprintf(stderr, "pcomm-ui: %s holds rank dirs but none could be read\n",
                         dir.c_str());
            return 1;
        }
        const auto st = pcomm::capture::load_raw_tables(con, ranks);
        pcomm::capture::fill_missing_raw_tables(con);
        const auto bad = pcomm::link::define(con);
        tables = st.tables;
        std::printf("pcomm-ui: capture %s — %zu ranks, %zu tables, %zu rows%s\n", dir.c_str(),
                    ranks.size(), st.tables, st.rows,
                    (st.failed || bad) ? " (SOME FAILED, see above)" : "");
    } else {
        tables = pcomm::duck::mount_raw_tables(con, dir);
    }

    // Binding off loopback changes the trust boundary, so it changes the rules.
    // The comment above looks_like_select names the two holes that loopback was
    // covering: the first-token SELECT check is not a parser, and DuckDB's table
    // functions let any SELECT read any file this process can. Both are
    // acceptable when the only possible caller already has a shell here. Neither
    // is acceptable to a network, however small.
    if (!loopback) {
        // A Parquet export is served as views over read_parquet, so it needs
        // file access at QUERY time and cannot be hardened. A capture is loaded
        // into memory up front, so it can. Refuse rather than quietly leave
        // external access on — the whole point is that this path is the one
        // reachable by someone else.
        if (!from_capture) {
            std::fprintf(stderr,
                         "pcomm-ui: --bind %s refuses a Parquet export: its tables are views\n"
                         "  over files, so queries need file access that must stay off here.\n"
                         "  Point it at the capture directory instead — that loads into memory.\n",
                         bind_addr.c_str());
            return 1;
        }
        if (!pcomm::duck::exec(con, "SET enable_external_access=false",
                               "disable external access")) {
            std::fprintf(stderr, "pcomm-ui: refusing to bind %s without external access off\n",
                         bind_addr.c_str());
            return 1;
        }
        std::fprintf(stderr,
                     "pcomm-ui: bound to %s — reachable by anyone who can route to it.\n"
                     "  External file access is OFF; the query endpoint is still SELECT-only\n"
                     "  by a first-token check, not a parser. Serve captures you would show\n"
                     "  that audience anyway.\n",
                     bind_addr.c_str());
    }

    // Then everything else in the directory — the derived tables from --link,
    // and anything a future pass adds. Raw and derived share one namespace on
    // purpose: a query should not have to know which pass produced a table.
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".parquet")
            continue;
        const auto    name = e.path().stem().string();
        duckdb_result r;
        const auto    probe = "SELECT 1 FROM \"" + name + "\" LIMIT 0";
        const bool    known = duckdb_query(con, probe.c_str(), &r) == DuckDBSuccess;
        duckdb_destroy_result(&r);
        if (known)
            continue;  // already mounted above
        const auto sql = "CREATE OR REPLACE VIEW \"" + name + "\" AS SELECT * FROM read_parquet('" +
                         e.path().string() + "')";
        duckdb_result r2;
        if (duckdb_query(con, sql.c_str(), &r2) == DuckDBSuccess)
            tables++;
        duckdb_destroy_result(&r2);
    }
    if (!tables) {
        // Two different failures, and telling someone to run --parquet on a
        // directory that already IS a capture is how a wrong error message
        // costs more time than no error message.
        if (from_capture)
            std::fprintf(stderr,
                         "pcomm-ui: %s has rank dirs but no decodable streams\n"
                         "  check it with: pcomm-read %s\n",
                         dir.c_str(), dir.c_str());
        else
            std::fprintf(stderr,
                         "pcomm-ui: no .parquet tables in %s, and no rank-* dirs either\n"
                         "  pcomm-read <capture> --parquet %s && pcomm-read --link %s\n"
                         "  (or just: pcomm-ui <capture>)\n",
                         dir.c_str(), dir.c_str(), dir.c_str());
        return 1;
    }

    httplib::Server srv;

    srv.Post("/query", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string& sql = req.body;
        if (!looks_like_select(sql)) {
            res.status = 400;
            res.set_content(std::string("{\"error\":\"") + kReadOnlyErr + "\"}",
                            "application/json");
            return;
        }
        // LIMIT is applied by wrapping, not by trusting the caller to include
        // one: an accidental unbounded scan should be slow, not fatal to the
        // browser tab.
        const auto capped = "SELECT * FROM (" + sql + ") LIMIT " + std::to_string(kRowCap);

        duckdb_result r;
        if (duckdb_query(con, capped.c_str(), &r) != DuckDBSuccess) {
            const char* err = duckdb_result_error(&r);
            res.status = 400;
            res.set_content("{\"error\":\"" + json_escape(err ? err : "query failed") + "\"}",
                            "application/json");
            duckdb_destroy_result(&r);
            return;
        }

        // {columns:[...], rows:[[...]]} — column-named, row-oriented. Values are
        // strings; the page casts what it needs. Deliberate: a JSON number
        // cannot hold a UBIGINT address without losing the low bits, and those
        // addresses are the join keys.
        const idx_t ncol = duckdb_column_count(&r);
        const idx_t nrow = duckdb_row_count(&r);
        std::string out = "{\"columns\":[";
        for (idx_t c = 0; c < ncol; ++c) {
            if (c)
                out += ',';
            out += '"' + json_escape(duckdb_column_name(&r, c)) + '"';
        }
        out += "],\"rows\":[";
        for (idx_t i = 0; i < nrow; ++i) {
            if (i)
                out += ',';
            out += '[';
            for (idx_t c = 0; c < ncol; ++c) {
                if (c)
                    out += ',';
                char* v = duckdb_value_varchar(&r, c, i);
                if (v)
                    out += '"' + json_escape(v) + '"';
                else
                    out += "null";
                duckdb_free(v);
            }
            out += ']';
        }
        out += "]}";
        duckdb_destroy_result(&r);
        res.set_content(out, "application/json");
    });

    // The embedded app, unless --dir points at a directory to serve instead.
    // That override exists for iterating on the frontend without a rebuild of
    // this binary; everything else — including every copy that ends up on a
    // login node — serves what is compiled in.
    if (!static_dir.empty())
        srv.set_mount_point("/", static_dir);
    else
        srv.Get(".*", [](const httplib::Request& req, httplib::Response& res) {
            // "/" means index.html; anything else is looked up literally. No
            // SPA catch-all rewrite: the app puts its view in the URL FRAGMENT
            // (#flow), which never reaches the server, so a path we do not have
            // is a genuine 404 and should say so rather than returning the page
            // with status 200 and letting it fail in the browser.
            const std::string want = req.path == "/" ? "/index.html" : req.path;
            for (const auto& a : pcomm::ui::kAssets)
                if (want == a.path) {
                    res.set_content(reinterpret_cast<const char*>(a.data), a.size, a.mime);
                    return;
                }
            res.status = 404;
            res.set_content("no such asset: " + req.path + "\n", "text/plain");
        });

    std::printf("pcomm-ui: %zu tables from %s\n", tables, dir.c_str());
    if (static_dir.empty())
        std::printf("pcomm-ui: http://%s:%d/  (%zu embedded assets)\n", bind_addr.c_str(), port,
                    pcomm::ui::kAssetCount);
    else
        std::printf("pcomm-ui: http://%s:%d/  (serving %s)\n", bind_addr.c_str(), port,
                    static_dir.c_str());
    std::fflush(stdout);

    if (!srv.listen(bind_addr.c_str(), port)) {
        std::fprintf(stderr, "pcomm-ui: cannot bind %s:%d\n", bind_addr.c_str(), port);
        return 1;
    }
    duckdb_disconnect(&con);
    duckdb_close(&db);
    return 0;
}
