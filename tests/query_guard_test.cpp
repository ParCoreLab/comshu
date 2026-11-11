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

    std::puts("ok: query_guard skips leading comments and still refuses writes");
    return 0;
}
