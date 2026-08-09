// pcomm ui — serve an exported capture as a queryable page.
//
//   pcomm-ui <export-dir> [--port N] [--dir <static-dir>]
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

#include "duck_mount.hpp"  // shared with pcomm-read: the empty-table policy
#include "ui_assets.hpp"   // generated: the built page, embedded

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
bool looks_like_select(const std::string& sql) {
    const std::size_t i = sql.find_first_not_of(" \t\r\n");
    if (i == std::string::npos)
        return false;
    const std::string head = sql.substr(i);
    return strncasecmp(head.c_str(), "SELECT", 6) == 0 ||
           strncasecmp(head.c_str(), "WITH", 4) == 0;
}

std::string json_escape(const char* s) {
    std::string out;
    for (const char* p = s; p && *p; ++p) {
        switch (*p) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += *p;
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: pcomm-ui <export-dir> [--port N] [--dir <static-dir>]\n"
                   "  <export-dir> is a `pcomm-read --parquet` output, ideally --link'ed\n",
                   stderr);
        return 1;
    }
    fs::path    dir = argv[1];
    int         port = 8777;
    std::string static_dir;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--port") == 0)
            port = std::atoi(argv[i + 1]);
        else if (std::strcmp(argv[i], "--dir") == 0)
            static_dir = argv[i + 1];
    }
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "pcomm-ui: not a directory: %s\n", dir.c_str());
        return 1;
    }

    duckdb_database   db{};
    duckdb_connection con{};
    if (duckdb_open(nullptr, &db) != DuckDBSuccess ||
        duckdb_connect(db, &con) != DuckDBSuccess) {
        std::fprintf(stderr, "pcomm-ui: cannot open DuckDB\n");
        return 1;
    }

    // Raw record tables first, through the SAME policy `--link` uses: a stream
    // this capture lacks becomes an EMPTY table with the right columns, not a
    // missing one. Without that, a panel querying `mlx5_device_send` fails on
    // every verbs-only capture, and every panel would have to branch on which
    // provider ran — the coupling the derived layer exists to remove.
    std::size_t tables = pcomm::duck::mount_raw_tables(con, dir);

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
        std::fprintf(stderr,
                     "pcomm-ui: no .parquet tables in %s\n"
                     "  pcomm-read <trace-dir> --parquet %s && pcomm-read --link %s\n",
                     dir.c_str(), dir.c_str(), dir.c_str());
        return 1;
    }

    httplib::Server srv;

    srv.Post("/query", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string& sql = req.body;
        if (!looks_like_select(sql)) {
            res.status = 400;
            res.set_content(std::string("{\"error\":\"") + kReadOnlyErr + "\"}", "application/json");
            return;
        }
        // LIMIT is applied by wrapping, not by trusting the caller to include
        // one: an accidental unbounded scan should be slow, not fatal to the
        // browser tab.
        const auto capped =
            "SELECT * FROM (" + sql + ") LIMIT " + std::to_string(kRowCap);

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

    // A built frontend if one was given, the embedded page otherwise. The seam
    // exists so the Svelte app can replace the built-in page without the server
    // learning anything about it.
    if (!static_dir.empty())
        srv.set_mount_point("/", static_dir);
    else
        srv.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(pcomm::ui::kIndexHtml, "text/html");
        });

    std::printf("pcomm-ui: %zu tables from %s\n", tables, dir.c_str());
    if (static_dir.empty())
        std::printf("pcomm-ui: http://127.0.0.1:%d/  (built-in page)\n", port);
    else
        std::printf("pcomm-ui: http://127.0.0.1:%d/  (serving %s)\n", port, static_dir.c_str());
    std::fflush(stdout);

    if (!srv.listen("127.0.0.1", port)) {
        std::fprintf(stderr, "pcomm-ui: cannot bind 127.0.0.1:%d\n", port);
        return 1;
    }
    duckdb_disconnect(&con);
    duckdb_close(&db);
    return 0;
}
