-- requires: mem_object
-- No memory object records a FAILED device attribution, on any case.
--
-- AttrStatus (vocab.hpp) separates the answers that are healthy — Device, a
-- host answer, or never asked because CUDA was not loaded — from the two that
-- are a loss: OrdinalFailed (device memory whose GPU could not be read) and
-- NullAddr (an emitter asked about a null pointer). Either is a defect in the
-- capture, and link_health reports them as attribution_failed; this holds the
-- whole board to zero.
SELECT CAST(rank AS INTEGER) AS rank, attr_status, count(*) AS objects
FROM mem_object
WHERE attr_status IN ('OrdinalFailed', 'NullAddr')
GROUP BY rank, attr_status;
