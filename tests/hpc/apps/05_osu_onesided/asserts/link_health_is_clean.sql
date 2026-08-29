-- requires: link_health
-- The derived layer's own bill of health, which the brief says must be empty.
--
-- Every row here is a defect in the LINK, not in the workload: an operation
-- whose transport could not be named, a send whose peer could not be resolved,
-- an interface pointer claimed by two transports. This study is the first time
-- a third-party one-sided workload has been put through `pcomm link` at all,
-- and the round-2 brief is explicit that a non-empty link_health is a finding.
--
-- WHY IT BELONGS HERE RATHER THAN ONLY IN THE SUITE. The RERUN of 2026-08-27
-- found QuantumESPRESSO reporting a clean board over a 91% hole because the
-- finding that would have named it — unresolved_ucx — did not exist yet. A
-- one-sided workload has a structurally harder peer-resolution problem than a
-- tagged one: the peer is named through a remote KEY and an endpoint rather
-- than by the send itself, so if any workload is going to put a row in this
-- table it is this one, and asserting it is how the hole becomes visible
-- instead of being reported as a pass.
--
-- Prints the note as well as the count, because the note is what makes the
-- finding actionable and reading it should not require a second query.
SELECT finding, n, note
FROM link_health
WHERE CAST(n AS INTEGER) > 0;
