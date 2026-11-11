#!/bin/bash
# verify_asserts.sh — test the tests.
#
#   tests/hpc/cases/09_ibgda_putget/verify_asserts.sh
#
# Proves each of this case's assertions (a) stays silent on a healthy capture
# and (b) fires on the specific defect it claims to catch, against a synthetic
# fixture. An assertion nobody has ever seen fail is not known to work — and
# these guard a code path that only one machine in the lab can execute, so
# without this they would go unexercised on every other host.
#
# Needs nothing but sqlite3: no GPU, no InfiniBand, no A100. That is the point.
#
# The fixture models the real topology deliberately, including the part that is
# easy to get wrong: both PEs sit behind ONE HCA, so src_lid == dest_lid on
# every connection. The last check asserts that a same-LID capture is NOT
# flagged — case 02 treats a self-edge as a bug, and copying that assertion here
# would have failed a perfectly correct loopback run.
set -u
A="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/asserts}"
DB="$(mktemp -t comshu-verify-XXXXXX.db)"
trap 'rm -f "$DB"' EXIT

mk() {  # mk <db> <extra-sql>
  local db="$1"; shift
  rm -f "$db"
  sqlite3 "$db" <<SQL
CREATE TABLE params (ranks INTEGER, umem_binding INTEGER);
INSERT INTO params VALUES (2, 1);
CREATE TABLE mlx5_umem (rank INT, umem_id INT, addr TEXT, binding TEXT, dmabuf_fd INT);
CREATE TABLE mlx5_devx_qp (rank INT, qp_num INT, wq_umem_id INT, sq_base TEXT, gpu_driven INT);
CREATE TABLE mlx5_devx_qp_connect (rank INT, qp_num INT, dest_qp_num INT, dest_lid INT, src_lid INT, port_num INT);
CREATE TABLE mlx5_devx_cmd (rank INT, obj INT, opcode TEXT, decoded INT);
-- healthy 2-rank loopback capture: same LID both sides (one HCA), symmetric.
INSERT INTO mlx5_umem VALUES (0,1,'0x0','by_dmabuf',85),(0,2,'0x0','by_dmabuf',85),
                             (1,1,'0x0','by_dmabuf',86),(1,2,'0x0','by_dmabuf',86);
INSERT INTO mlx5_devx_qp VALUES (0,100,1,'0xAAA0',1),(1,200,1,'0xBBB0',1);
INSERT INTO mlx5_devx_qp_connect VALUES (0,100,200,7,7,1),(1,200,100,7,7,1);
INSERT INTO mlx5_devx_cmd VALUES (0,1,'0x500',1),(1,1,'0x500',1),
                                 (0,2,'0x503',1),(1,2,'0x503',1);
$*
SQL
}

run() {
  local out; out=$(sqlite3 "$1" ".read $2" 2>&1)
  case "$out" in *"no such column"*|*"no such table"*|*"syntax error"*)
    echo "QUERY-ERROR: $out" >&2; echo 9999; return ;;
  esac
  printf '%s' "$out" | grep -c . 
}

pass=0; fail=0
check() { # check <label> <expect: 0|nonzero> <assert-file> <mutation-sql>
  local label="$1" expect="$2" f="$3"; shift 3
  mk "$DB" "$*"
  local n; n=$(run "$DB" "$A/$f")
  if [ "$expect" = "0" ] && [ "$n" = "0" ]; then echo "  ok   $label"; pass=$((pass+1))
  elif [ "$expect" = "n" ] && [ "$n" != "0" ]; then echo "  ok   $label (fired, $n row(s))"; pass=$((pass+1))
  else echo "  FAIL $label (got $n rows, wanted $([ "$expect" = 0 ] && echo none || echo some))"; fail=$((fail+1)); fi
}

echo "healthy capture — every assertion must be silent:"
for f in "$A"/*.sql; do check "$(basename "$f")" 0 "$(basename "$f")" ""; done

echo
echo "injected defects — the named assertion must fire:"
check "ibgda_engaged: no GPU-driven QP"      n ibgda_engaged.sql \
      "UPDATE mlx5_devx_qp SET gpu_driven=0;"
check "ibgda_engaged: a rank missing"        n ibgda_engaged.sql \
      "DELETE FROM mlx5_devx_qp WHERE rank=1;"
check "symmetry: one rank short"             n devx_setup_symmetric.sql \
      "DELETE FROM mlx5_umem WHERE rank=1 AND umem_id=2;"
check "decoder: create_qp with no qp row"    n decoder_lost_nothing.sql \
      "DELETE FROM mlx5_devx_qp WHERE rank=0;"
check "decoder: decoded=1 on unknown opcode" n decoder_lost_nothing.sql \
      "INSERT INTO mlx5_devx_cmd VALUES (0,9,'0x999',1);"
check "topology: dangling dest_qp_num"       n topology_closed.sql \
      "UPDATE mlx5_devx_qp_connect SET dest_qp_num=999 WHERE rank=0;"
check "topology: connect on uncreated qp"    n topology_closed.sql \
      "INSERT INTO mlx5_devx_qp_connect VALUES (0,777,200,7,7,1);"
check "binding: wrong binding for config"    n umem_binding_as_configured.sql \
      "UPDATE mlx5_umem SET binding='by_addr', dmabuf_fd=-1;"
check "binding: dmabuf with nonzero addr"    n umem_binding_as_configured.sql \
      "UPDATE mlx5_umem SET addr='0xDEAD' WHERE rank=0;"
check "referential: qp -> unknown umem"      n umem_referential.sql \
      "UPDATE mlx5_devx_qp SET wq_umem_id=42;"
# sq_base=0 is only a defect under by_addr — under by_dmabuf it is the first
# slot in the descriptor, which a real capture showed (three queue pairs at 0x0,
# 0x10000, 0x20000 in one buffer). So the fixture has to become a by_addr
# capture for this to be a defect at all.
check "referential: known by_addr umem, sq_base 0" n umem_referential.sql \
      "UPDATE mlx5_umem SET binding='by_addr', addr='0xF000', dmabuf_fd=-1;
       UPDATE mlx5_devx_qp SET sq_base='0x0';
       UPDATE params SET umem_binding=0;"
check "referential: by_dmabuf sq_base 0 is NOT a defect" 0 umem_referential.sql \
      "UPDATE mlx5_devx_qp SET sq_base='0x0';"

# The uniqueness invariant: a connected queue pair must know its own port's LID.
check "nameable: connected qp with src_lid 0"  n qp_is_nameable.sql \
      "UPDATE mlx5_devx_qp_connect SET src_lid=0;"
check "nameable: a DCI naming no remote is fine" 0 qp_is_nameable.sql \
      "UPDATE mlx5_devx_qp_connect SET dest_lid=0, dest_qp_num=0, src_lid=0;"

echo
echo "self-check: a loopback capture (src_lid == dest_lid) must NOT be flagged"
check "topology tolerates same-LID edges"    0 topology_closed.sql \
      "UPDATE mlx5_devx_qp_connect SET src_lid=7, dest_lid=7;"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
