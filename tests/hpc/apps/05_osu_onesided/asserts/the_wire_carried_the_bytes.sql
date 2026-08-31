-- requires: transport_mix params.total_bytes
-- The two layers must agree about how much left the node.
--
-- This is the cross-layer claim the brief says has never been pointed at an
-- application we did not write: the UCP layer records what the application
-- ASKED FOR and the UCT layer records what UCX ISSUED, and 3,174,400 B is a
-- number from the command line rather than from either of them.
--
-- A FLOOR AND NOT AN EQUALITY, for two reasons that both come from the
-- transport rather than from the tracer:
--   * UCP fragments a bcopy put at the transport's segment size, so N MPI_Puts
--     can legitimately be more than N UCT operations' worth of rows while the
--     bytes are conserved;
--   * OpenMPI's own window synchronisation moves bytes over the same doors,
--     so the wire total is the workload's plus a little.
-- Asserting equality would therefore be asserting the HCA's segment size, which
-- is not this project's claim. Asserting a floor still catches the failure that
-- matters: a transport layer that recorded the operations and lost the payload,
-- or an `op` family whose payload field means something other than bytes sent.
--
-- WHY BOTH DIRECTIONS ARE SUMMED. One meta.json serves the put arm and the get
-- arm, and a get's payload arrives rather than leaves. Summing out and in is
-- what makes the same assertion true of both without naming the direction;
-- advertised_bytes is deliberately NOT summed, because those are bytes offered
-- for a peer to pull and counting them here would bill the same buffer twice.
SELECT 'the transport layer carried fewer bytes than the command line asked for' AS problem,
       (SELECT coalesce(sum(CAST(out_bytes AS INTEGER) + CAST(in_bytes AS INTEGER)), 0)
        FROM transport_mix
        WHERE CAST(op AS VARCHAR) LIKE 'put%'
           OR CAST(op AS VARCHAR) LIKE 'get%') AS wire_bytes,
       (SELECT total_bytes FROM params)        AS requested_bytes
WHERE (SELECT coalesce(sum(CAST(out_bytes AS INTEGER) + CAST(in_bytes AS INTEGER)), 0)
       FROM transport_mix
       WHERE CAST(op AS VARCHAR) LIKE 'put%'
          OR CAST(op AS VARCHAR) LIKE 'get%')
      < (SELECT total_bytes FROM params);
