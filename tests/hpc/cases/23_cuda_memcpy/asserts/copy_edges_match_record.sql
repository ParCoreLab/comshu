-- requires: comm_edges edge_sizes
-- The derived lane against the raw record it is built from: every byte that
-- crossed a device boundary appears in comm_edges' cudamemcpy lane, and nothing
-- else does. And edge_sizes, split one group deeper, rolls up to the same
-- number.
--
-- An INVARIANT, not an oracle — it names no rank, no ordinal and no size — and
-- it catches the one class of error params cannot see. Staging bytes are real
-- bytes, so dropping the host-end filter from _memcpy_edges would inflate the
-- lane by 6710886400 B on this workload while every params-derived assertion
-- beside it stayed green.
--
-- `edge_sizes` IS IN THE REQUIRES LINE FOR A MECHANICAL REASON worth stating,
-- because it is not obvious and it silently disabled this check once. sql_check
-- re-runs an assertion against the Parquet/DuckDB layer only when a table it
-- requires is ABSENT from the sqlite side and present in Parquet. `comm_edges`
-- is not absent there: the CSV export writes its own comm_edges.csv from
-- CsvAgg, header-only on a capture with no verbs traffic, which sqlite imports
-- as an EMPTY TABLE. So requiring comm_edges alone got this assertion skipped
-- on both engines and reported as `[skip]`, not as a failure. edge_sizes exists
-- only in the derived layer, so naming it is what makes the re-run happen — and
-- the query uses it, rather than naming it as a token.
SELECT 'copy lane disagrees with the record it is built from' AS problem,
       (SELECT coalesce(sum(bytes), 0) FROM comm_edges  WHERE transport = 'cudamemcpy') AS lane,
       (SELECT coalesce(sum(bytes), 0) FROM edge_sizes  WHERE transport = 'cudamemcpy') AS sized,
       (SELECT coalesce(sum(bytes), 0) FROM cuda_memcpy
         WHERE src_device != 65535 AND dst_device != 65535 AND src_device != dst_device) AS crossed
WHERE lane != crossed OR sized != crossed;
