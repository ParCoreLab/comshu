-- requires: ucx_rkey_pack
-- An exported key is a buffer of bytes. It must have a length.
--
-- UcxRkeyPack is the ONE record in the UCX family that can hash its whole
-- payload, and the schema says why: ucp_rkey_pack returns the size, where the
-- unpack door takes no length at all and so a fixed prefix hash there would be
-- an over-read that never matches. That makes buffer_len the record's load-
-- bearing field — it is the reason the hash is trustworthy — and nothing has
-- ever checked that it is populated.
--
-- WHAT A ZERO WOULD MEAN. Either the length was read from the wrong place (the
-- size is an out-parameter of ucp_rkey_pack, so a hook that records before the
-- call returns gets whatever was on the stack), or the pack succeeded and
-- exported nothing. The first is a provider defect and the second is an MPI
-- that cannot possibly work. Both are invisible to every other check here: a
-- zero-length pack row still joins to its mem_map, still counts towards
-- n_windows, and still looks like a witnessed export.
--
-- A packed rkey on this stack is a handful of bytes — the schema comment says
-- nine is possible — so this asserts only that it is not empty and not absurd,
-- and names no size of its own. 4096 is a page, and a packed remote key that
-- exceeded one would mean the length is not a length.
SELECT 'a packed rkey has an impossible buffer length' AS problem,
       rank,
       memh,
       CAST(buffer_len AS INTEGER) AS buffer_len
FROM ucx_rkey_pack
WHERE CAST(buffer_len AS INTEGER) <= 0
   OR CAST(buffer_len AS INTEGER) > 4096;
