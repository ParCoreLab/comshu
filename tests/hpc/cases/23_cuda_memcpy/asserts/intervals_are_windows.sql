-- [ticks, end_ticks] is the COPY ENGINE's window, which is the whole claim the
-- record's `source` column makes. A 4 MiB transfer cannot take zero time, so an
-- end that does not follow its start means the interval is not what the record
-- says it is — a submission stamp, a dropped field, or a domain mix-up.
--
-- Empty means pass; see kind_is_named for why that is safe in this case.
SELECT rank, correlation, ticks, end_ticks
FROM cuda_memcpy
WHERE CAST(end_ticks AS INTEGER) <= CAST(ticks AS INTEGER);
