#pragma once
// The first-token check that keeps `POST /query` read-only.
//
// Cheap, and it is the ONLY thing stopping a write: an in-memory DuckDB cannot
// be opened in READ_ONLY access mode, so there is no engine-level guard behind
// this one. Stated plainly because a comment claiming a safety property the
// code does not have is worse than no comment.
//
// What actually bounds the risk is the trust boundary: the server binds
// loopback and serves one local capture to the person who started it, so a
// caller who can POST here can already run comshu itself. The same reasoning
// covers the larger hole: DuckDB's own table functions let a SELECT read any
// file the user can (`read_csv('/etc/passwd')`). That is why binding anywhere
// other than loopback (`--bind`) turns external access OFF and refuses to serve
// a Parquet export (bind_is_loopback / harden_for_network in main.cpp).
//
// LEADING COMMENTS ARE SKIPPED, then the first token decides. Without this, any
// commented query pasted into the Ask panel came back 400 with a message about
// writes, and every worked example in agent/catalog.md (each starts with a
// comment) failed. Skipping cannot open a bypass: what follows the comments is
// still held to SELECT/WITH, and an unterminated /* is refused, so nothing can
// hide behind a comment the parser closes differently.
//
// Header-only and free of DuckDB and httplib so tests/query_guard_test.cpp can
// exercise it without the server.

#include <cstddef>
#include <string>
#include <strings.h>

namespace comshu::ui {

// Index of the first character that is neither whitespace nor inside a leading
// `--` line comment or `/* */` block comment; npos if there is none, or if a
// block comment never closes.
inline std::size_t skip_leading_comments(const std::string& sql) {
    std::size_t i = 0;
    for (;;) {
        i = sql.find_first_not_of(" \t\r\n", i);
        if (i == std::string::npos)
            return i;
        if (sql.compare(i, 2, "--") == 0) {
            i = sql.find('\n', i);
            if (i == std::string::npos)
                return i;
            continue;
        }
        if (sql.compare(i, 2, "/*") == 0) {
            const std::size_t end = sql.find("*/", i + 2);
            if (end == std::string::npos)
                return std::string::npos;
            i = end + 2;
            continue;
        }
        return i;
    }
}

inline bool looks_like_select(const std::string& sql) {
    const std::size_t i = skip_leading_comments(sql);
    if (i == std::string::npos)
        return false;
    const char* head = sql.c_str() + i;
    return strncasecmp(head, "SELECT", 6) == 0 || strncasecmp(head, "WITH", 4) == 0;
}

}  // namespace comshu::ui
