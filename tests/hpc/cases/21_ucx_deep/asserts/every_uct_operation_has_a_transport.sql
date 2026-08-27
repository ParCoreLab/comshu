-- requires: link_health uct_send
-- Every recorded UCT operation must name the transport that carried it.
--
-- The whole point of the 800s is that `uct_iface` says whether a message went
-- over rc_mlx5, over shared memory or over a CUDA IPC handle. An operation whose
-- interface cannot be named has the timing and the byte count and not the one
-- fact it was captured for.
--
-- Three different causes land in this one count — no uct_iface row carries that
-- iface pointer, the pointer was reused for a second transport, or its tl_name
-- intern handle is claimed by two strings — and they mean the same thing to a
-- consumer, which is why the derived layer counts them together and why this
-- asserts the count rather than the cause. A non-zero value here is a defect in
-- the provider's entity tracking, not in the workload.
SELECT finding, n, note
FROM link_health
WHERE finding = 'unnamed_uct_transport' AND CAST(n AS INTEGER) > 0;
