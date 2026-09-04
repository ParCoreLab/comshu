-- requires: link_health
-- One process drives all four GPUs, so every device on this node has an owner
-- and no peer copy should fail to resolve. A non-zero here means the ownership
-- chain (_node -> _device_key -> _device_owner) did not close, which would put
-- every byte of this workload's traffic on an edge to rank -1 while comm_edges
-- still looked populated.
SELECT finding, n FROM link_health
WHERE finding = 'unresolved_device_copy' AND CAST(n AS INTEGER) != 0;
