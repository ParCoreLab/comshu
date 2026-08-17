-- requires: object_access params.ranks
-- The hub's recv buffer must be ONE object in object_access with every spoke as
-- an actor. This is hub_buffer_shared.sql's question asked of the DERIVED layer.
--
-- WHY BOTH. hub_buffer_shared.sql counts distinct run_dev straight off
-- gpu_access, so it tests the CAPTURE. It passed for the whole life of a bug in
-- the VIEW: object_access's p2p branch set owner_rank to the writing rank rather
-- than the owning one, so the hub's single 64 KiB buffer came out as three
-- objects of one actor each — the object id is symmetric-heap-shared and
-- mem_object has a row for it on every rank, so the panel's owner_rank = m.rank
-- join landed on the wrong one. Raw data correct, every view of it wrong, and
-- nothing in the suite could see the difference because no assertion could reach
-- a derived table at all.
--
-- Threshold-free and rank-free: it asks whether SOME object has ranks-1 distinct
-- actors, never which one. Stated as "the best we found is short of expected",
-- so the failure prints the number it did see.
WITH best AS (
    SELECT coalesce(max(w), 0) AS w
    FROM (SELECT count(DISTINCT actor_rank) AS w
          FROM object_access GROUP BY owner_rank, object)
)
SELECT 'no single object is written by every spoke' AS problem,
       (SELECT w FROM best)            AS most_actors_on_one_object,
       (SELECT ranks - 1 FROM params)  AS expected
WHERE (SELECT w FROM best) < (SELECT ranks - 1 FROM params);
