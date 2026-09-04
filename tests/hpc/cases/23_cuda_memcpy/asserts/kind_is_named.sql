-- No copy came back MemcpyKind::Unknown.
--
-- A translation gap in cupti_backend.cpp's copy_kind() would show up HERE and
-- nowhere else: every volume assertion in this case filters on a kind or on a
-- device pair, so an unrecognised copyKind would simply be absent from all of
-- them and each would still pass.
--
-- Empty means pass here, which is only safe because total_copies and
-- expect_tables both fail on an empty table. Stated rather than assumed, per
-- the vacuous-pass rule.
SELECT kind, count(*) AS n
FROM cuda_memcpy WHERE CAST(kind AS INTEGER) = 0 GROUP BY kind;
