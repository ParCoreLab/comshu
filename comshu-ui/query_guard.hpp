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

#include <cctype>
#include <cstddef>
#include <string>
#include <strings.h>

namespace comshu::ui {

inline bool is_ident_char(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_';
}

// Index of the first character that is neither whitespace nor inside a leading
// `--` line comment or `/* */` block comment; npos if there is none, or if a
// block comment never closes.
inline std::size_t skip_leading_comments(const std::string& sql, std::size_t start = 0) {
    std::size_t i = start;
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

// Returns true iff `sql` is a single SELECT / WITH statement with balanced
// parentheses and no trailing statement after a semicolon (CR-01).
// comshu-ui/main.cpp also verifies the statement with DuckDB's own parser
// (`duckdb_extract_statements` + `duckdb_prepared_statement_type`).
inline bool looks_like_select(const std::string& sql) {
    const std::size_t i = skip_leading_comments(sql);
    if (i == std::string::npos)
        return false;
    const char* head = sql.c_str() + i;
    const auto  rem = sql.size() - i;
    const bool  is_select =
        rem >= 6 && strncasecmp(head, "SELECT", 6) == 0 && (rem == 6 || !is_ident_char(head[6]));
    const bool is_with =
        rem >= 4 && strncasecmp(head, "WITH", 4) == 0 && (rem == 4 || !is_ident_char(head[4]));
    if (!is_select && !is_with)
        return false;

    int  depth = 0;
    bool saw_semi = false;
    for (std::size_t p = i; p < sql.size();) {
        const char c = sql[p];
        if (saw_semi) {
            // Only whitespace and comments may follow a trailing semicolon.
            const std::size_t next = skip_leading_comments(sql, p);
            if (next == std::string::npos) {
                // Either all trailing whitespace/comments, or an unterminated /*.
                // Check if an unterminated /* caused npos.
                const std::size_t ws = sql.find_first_not_of(" \t\r\n", p);
                if (ws != std::string::npos && sql.compare(ws, 2, "/*") == 0)
                    return false;
                break;
            }
            return false;
        }
        if (c == '\'' || c == '"') {
            const char q = c;
            ++p;
            while (p < sql.size()) {
                if (sql[p] == q) {
                    ++p;
                    if (p < sql.size() && sql[p] == q) {
                        ++p;  // escaped quote '' or ""
                        continue;
                    }
                    break;
                }
                ++p;
            }
            continue;
        }
        if (c == '-' && p + 1 < sql.size() && sql[p + 1] == '-') {
            const std::size_t nl = sql.find('\n', p + 2);
            if (nl == std::string::npos)
                break;
            p = nl + 1;
            continue;
        }
        if (c == '/' && p + 1 < sql.size() && sql[p + 1] == '*') {
            const std::size_t end = sql.find("*/", p + 2);
            if (end == std::string::npos)
                return false;
            p = end + 2;
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (--depth < 0)
                return false;
        } else if (c == ';') {
            saw_semi = true;
        }
        ++p;
    }
    return depth == 0;
}

// Removes a single trailing semicolon (outside quotes/comments) so a valid
// statement like `SELECT 1;` can be wrapped in `SELECT * FROM (... \n) LIMIT N`.
inline std::string strip_trailing_semicolon(const std::string& sql) {
    std::size_t last_semi = std::string::npos;
    for (std::size_t p = 0; p < sql.size();) {
        const char c = sql[p];
        if (c == '\'' || c == '"') {
            const char q = c;
            ++p;
            while (p < sql.size()) {
                if (sql[p] == q) {
                    ++p;
                    if (p < sql.size() && sql[p] == q) {
                        ++p;
                        continue;
                    }
                    break;
                }
                ++p;
            }
            continue;
        }
        if (c == '-' && p + 1 < sql.size() && sql[p + 1] == '-') {
            const std::size_t nl = sql.find('\n', p + 2);
            if (nl == std::string::npos)
                break;
            p = nl + 1;
            continue;
        }
        if (c == '/' && p + 1 < sql.size() && sql[p + 1] == '*') {
            const std::size_t end = sql.find("*/", p + 2);
            if (end == std::string::npos)
                break;
            p = end + 2;
            continue;
        }
        if (c == ';')
            last_semi = p;
        ++p;
    }
    if (last_semi == std::string::npos)
        return sql;
    return sql.substr(0, last_semi) + sql.substr(last_semi + 1);
}

}  // namespace comshu::ui
