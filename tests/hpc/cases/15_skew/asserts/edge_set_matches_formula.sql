-- requires: gpu_access
-- The observed edge set must equal the one the workload computes.
--
-- The workload picks edges from a closed form rather than an RNG precisely so
-- this assertion can exist:
--
--     h = (from*31 + to*17) % 11 ;  edge iff from != to and h >= 5
--
-- so the expected set is derivable here, from the same arithmetic, without
-- recording anything. That is the difference between this and a golden: a
-- golden would agree with whatever was captured, including a capture that
-- dropped half the edges or invented one.
--
-- Two directions of failure, reported together and labelled, because they mean
-- opposite things: a missing edge is lost data, an unexpected edge is
-- misattribution. Reporting only a count would conflate them.
WITH observed AS (
    SELECT DISTINCT CAST(run_dev AS INTEGER) AS f, CAST(mem_dev AS INTEGER) AS t
    FROM gpu_access
),
devs AS (SELECT DISTINCT f AS d FROM observed
         UNION SELECT DISTINCT t FROM observed),
expected AS (
    SELECT a.d AS f, b.d AS t
    FROM devs a, devs b
    WHERE a.d != b.d AND ((a.d * 31 + b.d * 17) % 11) >= 5
)
SELECT 'edge present in capture but not in the workload''s formula' AS problem, f, t
FROM observed WHERE (f, t) NOT IN (SELECT f, t FROM expected)
UNION ALL
SELECT 'edge in the workload''s formula but missing from the capture', f, t
FROM expected WHERE (f, t) NOT IN (SELECT f, t FROM observed)
ORDER BY f, t;
