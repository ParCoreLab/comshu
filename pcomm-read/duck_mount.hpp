// Mounting an export into DuckDB — shared by `pcomm-read --link` and `pcomm-ui`.
//
// Shared rather than copied because the interesting part is a *policy*, and two
// copies of a policy is one copy and one bug: a missing stream must become an
// EMPTY table with the right columns, not a missing one.
//
// Why that policy exists. A capture legitimately lacks whole families of
// streams — a gpumem-only run has no `ib_modify_qp`, and under IBGDA the entire
// `ib_*` family is absent while `mlx5_*` carries everything. Queries above this
// layer union both transports, so they name tables that no single-provider
// capture has. Without the empty stand-in, every panel and every derived query
// would have to branch on which provider produced the capture, which is the
// coupling the whole derived layer exists to remove.
//
// The distinction between "absent" and "empty" is preserved where it matters:
// the exporter still writes no file for a stream that does not exist. It is
// erased only here, at the point of query.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>

extern "C" {
#include "duckdb.h"
}
#include "raw_columns.hpp"

namespace pcomm::duck {

namespace fs = std::filesystem;

/** One SQL statement, with the error text surfaced rather than dropped. */
inline bool exec(duckdb_connection con, const std::string& sql, const char* what) {
    duckdb_result res;
    const bool    ok = duckdb_query(con, sql.c_str(), &res) == DuckDBSuccess;
    if (!ok) {
        const char* err = duckdb_result_error(&res);
        std::fprintf(stderr, "pcomm: %s failed: %s\n", what, err ? err : "(no message)");
    }
    duckdb_destroy_result(&res);
    return ok;
}

namespace detail {
template<typename Tuple, typename F, std::size_t... Is>
void for_each_type_impl(F&& f, std::index_sequence<Is...>) {
    (f.template operator()<std::tuple_element_t<Is, Tuple>>(), ...);
}
}  // namespace detail

template<typename Tuple, typename F> void for_each_type(F&& f) {
    detail::for_each_type_impl<Tuple>(std::forward<F>(f),
                                      std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

/**
 * Make every raw table reachable by name. Returns how many were actually
 * present; the rest are empty tables with the correct columns.
 *
 * Parquet is preferred and CSV accepted, so a `--csv` export is queryable too.
 * CSV is loaded through the generated DDL and COPY FROM rather than read_csv's
 * auto-detect: the types are declared by the record struct, and inferring them
 * from text is what once made `mem_object.base` a BIGINT on one capture and a
 * VARCHAR on another.
 */
inline std::size_t mount_raw_tables(duckdb_connection con, const fs::path& dir) {
    std::size_t present = 0;
    for_each_type<raw::RawRecords>([&]<typename R>() {
        using Raw = raw::RawTable<R>;
        const auto name = std::string(Raw::table);
        const auto parquet = dir / (name + ".parquet");
        const auto csv = dir / (name + ".csv");
        if (fs::exists(parquet)) {
            exec(con,
                 "CREATE OR REPLACE VIEW \"" + name + "\" AS SELECT * FROM read_parquet('" +
                     parquet.string() + "')",
                 Raw::table);
            present++;
        } else if (fs::exists(csv)) {
            if (exec(con, Raw::ddl, Raw::table) &&
                exec(con,
                     "COPY \"" + name + "\" FROM '" + csv.string() + "' (FORMAT CSV, HEADER)",
                     Raw::table))
                present++;
        } else {
            exec(con, Raw::ddl, Raw::table);
        }
    });
    return present;
}

}  // namespace pcomm::duck
