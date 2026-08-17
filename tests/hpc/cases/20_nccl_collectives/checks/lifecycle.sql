-- GOLDEN — communicator lifecycle: one open and one close per rank.
--
-- The close half is what entity_close_accounted checks structurally; this pins
-- the count. A provider that stopped emitting the EntityClose at
-- ncclCommDestroy would leave the assertion quiet — an unclosed entity is
-- exempt for mem_object and ib_device, and it would be easy to add nccl_comm to
-- that list to make a red board green. This golden makes the silence visible.
SELECT
  (SELECT count(*) FROM nccl_comm)                        AS comms_opened,
  (SELECT count(*) FROM entity_close WHERE opener = 900)  AS comms_closed,
  (SELECT count(DISTINCT edge) FROM nccl_group)           AS group_edge_kinds,
  (SELECT max(CAST(depth AS INTEGER)) FROM nccl_group)    AS max_group_depth;
