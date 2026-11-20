-- requires: link_health
-- No peer access was left unplaced. lsa_stores_land_in_the_window.sql proves
-- the right bytes went to the right place; this proves no others went nowhere —
-- link_health counts every gpu_access object_access could not place.
SELECT finding, n FROM link_health
WHERE finding = 'unattributed_p2p_accesses' AND CAST(n AS INTEGER) != 0;
