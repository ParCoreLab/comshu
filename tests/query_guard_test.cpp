// comshu-ui's read-only guard: leading comments are skipped, and skipping them
// opens no way around the check. See comshu-ui/query_guard.hpp.
#include <cassert>
#include <cstdio>
#include <string>

#include "query_guard.hpp"

using comshu::ui::looks_like_select;

int main() {
    // The reads, including every shape a commented catalog example takes.
    assert(looks_like_select("SELECT 1"));
    assert(looks_like_select("  with x AS (SELECT 1) SELECT * FROM x"));
    assert(looks_like_select("-- the heaviest pairs\nSELECT src_rank FROM comm_edges"));
    assert(looks_like_select("-- one\n-- two\n\n  SELECT 1"));
    assert(looks_like_select("/* block */ SELECT 1"));
    assert(looks_like_select("/* a */ -- b\n/* c\n spans */WITH t AS (SELECT 1) SELECT * FROM t"));

    // The writes, with and without comments in front.
    assert(!looks_like_select("DELETE FROM comm_edges"));
    assert(!looks_like_select("-- looks harmless\nDROP TABLE comm_edges"));
    assert(!looks_like_select("/* SELECT */ COPY comm_edges TO 'x.csv'"));
    assert(!looks_like_select("-- SELECT"));  // nothing after the comment

    // Nothing hides behind a comment that never closes.
    assert(!looks_like_select("/* SELECT 1"));
    assert(!looks_like_select("/* unterminated"));

    assert(!looks_like_select(""));
    assert(!looks_like_select("   \n\t"));

    // Trailing semicolon and quoted special characters stay allowed.
    assert(looks_like_select("SELECT 1;"));
    assert(looks_like_select("SELECT 1; -- trailing comment"));
    assert(looks_like_select("SELECT 'a;b)c' AS s"));
    assert(comshu::ui::strip_trailing_semicolon("SELECT 1;") == "SELECT 1");
    assert(comshu::ui::strip_trailing_semicolon("SELECT 'a;b';") == "SELECT 'a;b'");

    // Multi-statement and subquery-breakout injections must be refused (CR-01).
    assert(!looks_like_select("SELECT 1; DROP TABLE comm_edges"));
    assert(!looks_like_select("SELECT 1); DROP TABLE gpu_access; --"));
    assert(!looks_like_select("SELECT 1) UNION ALL SELECT * FROM comm_edges; --"));
    assert(!looks_like_select("SELECT 1; /* unterminated"));
    assert(!looks_like_select("SELECTIVE 1"));

    // A closed comment after the semicolon is fine; one that never closes is
    // not, whatever comes before it (the audit's two directions of CR-01).
    assert(looks_like_select("SELECT 1; /* done */"));
    assert(looks_like_select("SELECT 1; /* a */ -- b\n  /* c */\n"));
    assert(!looks_like_select("SELECT 1; -- ok\n/* unterminated"));
    assert(!looks_like_select("SELECT 1; /* a */ DROP TABLE comm_edges"));

    std::puts("ok: query_guard skips leading comments and still refuses writes");
    return 0;
}
