-- requires: uct_send
-- `tag` and `remote_addr` belong to specific doors, and to no others.
--
-- UctSend's declaration makes two claims that nothing has ever checked against
-- hardware:
--   * `tag` is set only by the TagEager*/TagRndv* doors — the ConnectX offload —
--     and is 0 (not "unknown") on every other door, because those doors carry no
--     tag at all: without offload UCP does its own matching and the tag travels
--     inside an AM payload this record does not parse.
--   * `remote_addr` is set only by Put*/Get*.
-- A non-zero value outside its family means the provider read a field from the
-- wrong argument slot, which is the characteristic failure of patching a
-- fourteen-entry dispatch table: every slot has a different signature and a
-- one-slot misalignment produces plausible garbage rather than a crash.
--
-- WRITTEN TO RUN ON BOTH ENGINES, and that is not cosmetic. The CSV export
-- DECODES — `op` is 'am_short' and `tag` is '0x1a2b' — while the Parquet export
-- deliberately does not, so the same columns are a UTINYINT and a UBIGINT there.
-- sql_check.sh prefers Parquet and falls back to CSV, so an assertion written
-- for one silently matches no row on the other and passes vacuously. Both
-- representations are therefore named: the op families as numbers AND names,
-- and "is it zero" as a string test after stripping the 0x prefix, which
-- reduces '0x0' and 0 to the same '0'.
SELECT 'a non-tag door carried a tag' AS problem, op, count(*) AS n
FROM uct_send
WHERE CAST(op AS VARCHAR) NOT IN ('10', '11', '12', '13',
                                  'tag_eager_short', 'tag_eager_bcopy',
                                  'tag_eager_zcopy', 'tag_rndv_zcopy')
  AND replace(CAST(tag AS VARCHAR), '0x', '') <> '0'
GROUP BY op
UNION ALL
SELECT 'a non-RMA door carried a remote address', op, count(*)
FROM uct_send
WHERE CAST(op AS VARCHAR) NOT IN ('4', '5', '6', '7', '8', '9',
                                  'put_short', 'put_bcopy', 'put_zcopy',
                                  'get_short', 'get_bcopy', 'get_zcopy')
  AND replace(CAST(remote_addr AS VARCHAR), '0x', '') <> '0'
GROUP BY op;
