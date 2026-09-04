-- requires: link_health
-- Nothing in this workload's traffic went unresolved. Every rank owns a GPU and
-- every peer copy names two of them, so a non-zero here means the ownership
-- chain did not close — which on this case is not a degradation but the
-- difference between a communication matrix and one unresolved row.
SELECT finding, n FROM link_health
WHERE finding = 'unresolved_device_copy' AND CAST(n AS INTEGER) != 0;
