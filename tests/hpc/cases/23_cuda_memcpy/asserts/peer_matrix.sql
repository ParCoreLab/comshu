-- Every ordered pair of distinct devices carries exactly `iters` copies.
--
-- This is what peer_volume cannot see: the totals are right if all 48 copies
-- landed on one pair, and a src/dst transposition in the backend or in the
-- record would keep every total identical while reversing every direction. The
-- pairs come from the capture and the count each must have comes from params,
-- so no ordinal is named here.
SELECT src_device, dst_device, count(*) AS n
FROM cuda_memcpy
WHERE CAST(src_device AS INTEGER) != 65535
  AND CAST(dst_device AS INTEGER) != 65535
  AND CAST(src_device AS INTEGER) != CAST(dst_device AS INTEGER)
  AND CAST(bytes AS INTEGER) = (SELECT msg_bytes FROM params)
GROUP BY src_device, dst_device
HAVING n != (SELECT iters FROM params);
