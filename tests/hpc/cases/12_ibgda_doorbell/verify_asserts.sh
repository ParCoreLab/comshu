#!/bin/bash
# verify_asserts.sh — test the tests. Proves each of case 12's data-plane
# assertions (a) stays silent on a healthy hijacked capture and (b) fires on the
# specific defect it claims to catch, against a synthetic sqlite fixture.
#
#   tests/hpc/cases/12_ibgda_doorbell/verify_asserts.sh
#
# Needs nothing but sqlite3 — no GPU, no InfiniBand, no A100 — so the assertions
# that guard a code path only du04 can execute are still exercised everywhere.
set -u
A="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/asserts}"
DB="$(mktemp -t pcomm-verify12-XXXXXX.db)"
trap 'rm -f "$DB"' EXIT

# A healthy capture: 2 ranks, one device-driven QP each, doorbells and sends on
# those QPs, every RDMA write a whole number of 4-byte elements.
mk() {  # mk <extra-sql>
  rm -f "$DB"
  sqlite3 "$DB" <<SQL
CREATE TABLE params (ranks INTEGER, elem_bytes INTEGER);
INSERT INTO params VALUES (2, 4);

CREATE TABLE mlx5_devx_qp (rank INT, qp_num INT, gpu_driven INT);
INSERT INTO mlx5_devx_qp VALUES (0, 100, 1), (1, 101, 1);

CREATE TABLE mlx5_doorbell (rank INT, uar INT, qp_num INT, prod_idx INT);
INSERT INTO mlx5_doorbell VALUES (0, 1, 100, 1), (0, 1, 100, 4), (1, 2, 101, 1), (1, 2, 101, 4);

CREATE TABLE mlx5_device_send
  (rank INT, qp_num INT, wqe_idx INT, opcode TEXT, opcode_name TEXT,
   laddr TEXT, lkey TEXT, raddr TEXT, rkey TEXT, length INT);
INSERT INTO mlx5_device_send VALUES
  (0, 100, 4, '0x08', 'rdma_write', '0x6208a0200', '0xb100', '0x6208a0200', '0xd000', 128),
  (0, 100, 1, '0x14', 'atomic_masked_cs', '0x0', '0x0', '0x620000200', '0xd000', 0),
  (1, 101, 4, '0x08', 'rdma_write', '0x62144c200', '0xb100', '0x62144c200', '0xd000', 128),
  (1, 101, 1, '0x14', 'atomic_masked_cs', '0x0', '0x0', '0x621374200', '0xd000', 0);
${1:-}
SQL
}

fails=0
# run <assert> <expect: pass|fail> <label>
run() {
  local q="$A/$1.sql" want="$2" label="$3"
  local out; out="$(sqlite3 -csv "$DB" < "$q" 2>&1)"
  local got="pass"; [ -n "$out" ] && got="fail"
  if [ "$got" = "$want" ]; then
    echo "  [ok ] $1: $label -> $got"
  else
    echo "  [XX ] $1: $label -> got '$got', want '$want'"
    [ -n "$out" ] && printf '%s\n' "$out" | head -4 | sed 's/^/        /'
    fails=1
  fi
}

echo "-- healthy capture: every assertion silent"
mk ""
run doorbell_engaged   pass "2 ranks, doorbells on created qps"
run doorbell_names_qp  pass "every doorbell names a devx_qp"
run device_send_named  pass "every send names a qp with a doorbell"
run sends_are_sized    pass "rdma_write length 128 = 32 elems"

echo "-- doorbell_engaged fires when a rank rang no doorbell"
mk "DELETE FROM mlx5_doorbell WHERE rank = 1;"
run doorbell_engaged   fail "rank 1 has no doorbells (ranks_seen 1 != 2)"

echo "-- doorbell_names_qp fires on a doorbell for an uncreated qp"
mk "INSERT INTO mlx5_doorbell VALUES (0, 1, 999, 7);"
run doorbell_names_qp  fail "qp_num 999 was never created"

echo "-- device_send_named fires on a send for an uncreated qp"
mk "INSERT INTO mlx5_device_send VALUES (0, 999, 9, '0x08', 'rdma_write', '0x1', '0x1', '0x2', '0x2', 128);"
run device_send_named  fail "send on qp_num 999 (no devx_qp, no doorbell)"

echo "-- sends_are_sized fires on a length that is not a whole element count"
mk "INSERT INTO mlx5_device_send VALUES (0, 100, 9, '0x08', 'rdma_write', '0x1', '0x1', '0x2', '0x2', 130);"
run sends_are_sized    fail "length 130 is not a multiple of elem_bytes 4"

echo "-- sends_are_sized fires on a zero-length data send"
mk "INSERT INTO mlx5_device_send VALUES (0, 100, 9, '0x08', 'rdma_write', '0x1', '0x1', '0x2', '0x2', 0);"
run sends_are_sized    fail "rdma_write with length 0 is a half-decoded WQE"

echo
if [ "$fails" -eq 0 ]; then echo "verify_asserts (case 12): PASS"; else echo "verify_asserts (case 12): FAIL"; fi
exit $fails
