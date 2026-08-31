-- requires: ucx_rma ucx_rkey
-- Every one-sided row names a remote key, and that key must resolve to a row
-- that recorded it being created.
--
-- THIS IS THE STUDY'S HEADLINE and the reason it exists: `ucx_rkey` (626) is
-- new, and until now the only thing that ever drove it was a workload the
-- pcomm authors wrote themselves. OSU is third-party code that has never heard
-- of this project, run with arguments chosen for their arithmetic rather than
-- for the doors they open, and it must produce the same join.
--
-- WHAT AN UNRESOLVED KEY MEANS. The RMA row would say a transfer happened and
-- be unable to say which peer region it targeted. `remote_addr` alone is a bare
-- virtual address with no owner; the rkey is what ties it to an endpoint, and
-- the endpoint to a peer. A capture of 1,000 puts whose keys resolve to nothing
-- is 1,000 rows of "something moved, somewhere".
--
-- SCOPED TO KEYS ACTUALLY NAMED by a recorded operation, not asserted as a
-- count. How many distinct keys an MPI unpacks for one window is the MPI
-- implementation's decision — OpenMPI keeps a state window beside each data
-- window and may unpack lazily per target — and pinning that number would be
-- freezing OpenMPI's internals into this project's test suite.
--
-- rkey = 0 is excluded because a null handle is the record's own "no key here",
-- not a key that failed to resolve.
SELECT 'an RMA row names an rkey that no ucx_rkey row created' AS problem,
       r.rank                                                  AS rank,
       r.rkey                                                  AS orphan_rkey,
       count(*)                                                AS rma_rows
FROM ucx_rma r
WHERE r.rkey <> 0
  AND NOT EXISTS (SELECT 1 FROM ucx_rkey k
                  WHERE k.rank = r.rank AND k.rkey = r.rkey)
GROUP BY r.rank, r.rkey;
