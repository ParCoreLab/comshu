-- requires: ucx_context uct_iface params.ranks
-- Every rank must appear in BOTH provider ranges, or the capture is partial in
-- a way that every downstream aggregate hides.
--
-- Two distinct failures land here and they are not the same defect:
--
--  * INTERPOSITION DID NOT ENGAGE ON A RANK. This is 19_ucx_mpi's floor
--    assertion generalised across the two ranges. A provider that engages on
--    seven ranks of eight produces a capture that looks entirely healthy: every
--    table has rows, every join closes, and every per-rank aggregate is simply
--    absent for one rank. Stated as "fewer than params.ranks", because MORE
--    would mean the workload changed while FEWER is the failure that matters,
--    and an "at least one row" check cannot see it.
--
--  * A RANK DIED BEFORE FLUSHING ITS LOW-VOLUME STREAMS. Round 1's finding 2,
--    which is the reason this assertion is here rather than in the suite:
--    pw.x reaches JOB DONE and then takes SIGSEGV in teardown (attributed with
--    an unpreloaded control run -- it is the application on this stack, not
--    pcomm), and a rank that dies by signal never writes its entity streams.
--    The symptom is inverted and therefore dangerous: the high-volume tables are
--    FULL, so the capture looks bigger and healthier than a clean one, and the
--    loss shows up only as unattributed rows downstream. 15.68 M of 18.70 M
--    sends landed that way in job 45088258.
--
-- uct_iface and not uct_send: an interface is opened once per (transport,
-- device) and is the cheapest per-rank witness that the 800-range engaged at
-- all. If uct_binding says accepted = 0 this will fire too, which is correct --
-- but uct_binding_accepted-r2.sql is the assertion that says WHY, and it should
-- be read first.
--
-- Derived from params.ranks; names no rank.
SELECT 'not every rank opened a UCP context' AS problem,
       (SELECT count(DISTINCT rank) FROM ucx_context) AS ranks_present,
       (SELECT ranks FROM params)                     AS expected
WHERE (SELECT count(DISTINCT rank) FROM ucx_context) < (SELECT ranks FROM params)

UNION ALL

SELECT 'not every rank opened a UCT interface',
       (SELECT count(DISTINCT rank) FROM uct_iface),
       (SELECT ranks FROM params)
WHERE (SELECT count(DISTINCT rank) FROM uct_iface) < (SELECT ranks FROM params)

UNION ALL

SELECT 'a rank recorded wire operations but never recorded opening an interface',
       (SELECT count(DISTINCT rank) FROM uct_send),
       (SELECT count(DISTINCT rank) FROM uct_iface)
WHERE (SELECT count(DISTINCT rank) FROM uct_send)
      > (SELECT count(DISTINCT rank) FROM uct_iface);
