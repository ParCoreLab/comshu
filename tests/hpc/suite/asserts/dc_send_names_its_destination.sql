-- requires: mlx5_device_send
-- A send that carries an address vector must say who it is addressed to.
--
-- On dynamically-connected transport the queue pair is not bound to a peer;
-- the WQE is. The destination DCT number lives in the low 24 bits of dqp_dct,
-- at offset 8 of the address vector, and it is the ONLY place a DC capture
-- records who a send went to. Without it the whole transport records traffic
-- with no peer — which is what this provider did until the AV rewrite, and why
-- dest_dct exists.
--
-- av_ds > 0 means the decoder found an address vector and read that word. A
-- zero DCT read out of a word the decoder just used to select the AV width is
-- not a peerless send — the fabric does not assign DCT 0 to a target — it is a
-- read from the wrong offset. The two facts come from the same 32 bits, so
-- this catches the case where the top bit happened to land somewhere plausible
-- and the low 24 did not.
--
-- Hex, not integer: the CSV renders dest_dct as 0x%06x, so the zero to compare
-- against is the string '0x000000'. Comparing against 0 here would be the
-- sqlite TEXT-affinity trap in its other direction — every row would look
-- nonzero and the assertion would pass on everything forever.
SELECT s.rank,
       s.qp_num,
       s.opcode,
       count(*) AS n_sends
FROM mlx5_device_send s
WHERE CAST(s.av_ds AS INTEGER) > 0
  AND s.dest_dct = '0x000000'
GROUP BY s.rank, s.qp_num, s.opcode
