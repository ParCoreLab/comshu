#!/bin/bash
# Case 25 — av_widths: the WQE decoder's address vector, at every width it has.
#
# The decoder derives the AV width PER WQE from MLX5_EXTENDED_UD_AV, and there
# are three outcomes: no AV at all on RC (av_ds=0), the 16-byte compact form on
# a DC queue pair whose port allows it (av_ds=1), and the 48-byte full form
# where it does not (av_ds=3). Every field after the AV — raddr, rkey, lkey,
# laddr, length — is read at an offset computed from that width, so getting it
# wrong does not produce an error. It produces a row that is confidently wrong,
# which is what the decoder did before the width was derived rather than assumed.
#
# TWO OF THE THREE WIDTHS RAN ON EVERY BOARD. The third never had. `av_ds=3`
# was written up as needing the paper's P-RoCE hardware and stayed unexercised
# for weeks, until job 45580050 produced it on MN5's own service NIC — an
# Ethernet mlx5 port whose lid == 0 clears support_half_av_seg. That is the
# whole reason this case exists: a branch proven once by a probe is a branch
# that can go quiet again with nothing to notice, and `av_ds` had already been
# added to the schema precisely so nobody would have to infer it from WQE index
# strides a second time.
#
# THE CENTRAL CHECK IS CROSS-LEG, and it is why this runs three legs instead of
# asserting the RoCE one alone. `length` sits AFTER the address vector. If a
# width is mis-derived, length shifts into an operand, a key, or half an
# address — all plausible integers. The workload is deterministic, so the three
# legs must agree exactly on (length, count) for their RDMA writes. Three
# different segment layouts arriving at the same byte counts is the statement
# that the offsets are right; each leg alone can only say it is self-consistent.
#
#   rc        default RC queue pairs, IB HCA         -> av_ds=0, dest_dct = 0
#   dc        NUM_RC_PER_PE=0, IB HCA                -> av_ds=1, dest_dct set
#   roce_dc   NUM_RC_PER_PE=0, Ethernet RoCE v2 port -> av_ds=3, dest_dct set
#
# SKIPs where there is no RoCE port, which is honest rather than convenient: on
# du04 (one InfiniBand HCA) the third width is genuinely unreachable and a case
# that quietly asserted only the first two would report coverage it does not
# have. The skip reason names the missing hardware.
#
#SBATCH --job-name=pcomm-25-av-widths
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --gres=gpu:1
#SBATCH --time=00:20:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
CASE_DIR="$PCOMM_ROOT/tests/hpc/cases/25_av_widths"
OUT=$(pcomm_out av_widths)            # the roce_dc capture, the one asserted
mkdir -p "$(dirname "$OUT")"

skip() {
  echo "=== 25_av_widths : SKIP ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"25_av_widths","ok":true,"skipped":true,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 0
}
fail() {
  echo "=== 25_av_widths : FAIL ($1) ==="
  mkdir -p "$OUT.csv"
  printf '{"case":"25_av_widths","ok":false,"reason":"%s","checks":0}\n' \
    "$1" > "$OUT.csv/result.json"
  exit 1
}

# ---- what hardware is here? -----------------------------------------------
[ -d /sys/class/infiniband ] || skip "no InfiniBand device on this host"

# DISCOVERED, NOT HARDCODED. The probe that first produced av_ds=3 named
# mlx5_2 and GID index 3 because that is what MN5's acc nodes happen to expose;
# neither is a property of RoCE, and a case that hardcodes them is a case that
# reports SKIP on the next cluster for the wrong reason. Both are found here by
# the properties that actually matter: an ACTIVE Ethernet mlx5 port (lid == 0,
# which is what clears support_half_av_seg) and a GID whose type is RoCE v2.
IB_DEV="" ETH_DEV="" ETH_GID=""
for d in /sys/class/infiniband/*; do
  [ -e "$d/ports/1/link_layer" ] || continue
  n=$(basename "$d")
  ll=$(cat "$d/ports/1/link_layer" 2>/dev/null)
  st=$(cat "$d/ports/1/state" 2>/dev/null)
  case "$st" in *ACTIVE*) ;; *) continue ;; esac
  if [ "$ll" = "InfiniBand" ] && [ -z "$IB_DEV" ]; then
    IB_DEV="$n"
  elif [ "$ll" = "Ethernet" ] && [ -z "$ETH_DEV" ]; then
    # A ROUTABLE RoCE v2 GID. Two filters, and the second one was learned the
    # expensive way. RoCE v1 is excluded because it is L2-only. And a RoCE v2
    # GID whose address is LINK-LOCAL (fe80::/10) is excluded because it is a
    # per-interface address that does not route between nodes: MN5's mlx5_2
    # exposes one at index 1 and a routable IPv4-mapped one at index 3, and
    # taking simply "the first RoCE v2 GID" picked index 1 — after which the
    # workload hung until the leg's timeout with no error to read (job
    # 45584080). The address is what distinguishes them, so the address is what
    # this filters on.
    for i in $(seq 0 15); do
      t=$(cat "$d/ports/1/gid_attrs/types/$i" 2>/dev/null)
      [ "$t" = "RoCE v2" ] || continue
      g=$(cat "$d/ports/1/gids/$i" 2>/dev/null)
      case "$g" in fe80:*) continue ;; esac
      ETH_DEV="$n"; ETH_GID="$i"; break
    done
  fi
done
echo "=== 25_av_widths : ib=$IB_DEV eth=$ETH_DEV gid=$ETH_GID ==="
[ -n "$IB_DEV" ] || skip "no ACTIVE InfiniBand port — the rc and dc legs need one"
[ -n "$ETH_DEV" ] || \
  skip "no ACTIVE Ethernet mlx5 port with a RoCE v2 GID — the full 48-byte AV (av_ds=3) is unreachable on this host"

# ---- which doorbell mechanism can this host use? --------------------------
# Identical to case 12, and deliberately not factored out: the two cases share a
# prerequisite, not an abstraction, and the day one of them needs a different
# rule the shared helper would be the thing standing in the way.
have_peermem=0
lsmod 2>/dev/null | grep -qE "nvidia_peermem|nv_peer_mem" && have_peermem=1
if grep -q "PeerMappingOverride=1" /proc/driver/nvidia/params 2>/dev/null && \
   [ "$have_peermem" = "1" ]; then
  DOORBELL_FLAG=--enable-doorbell-hijack; HANDLER=gpu; ARM=gpu
else
  DOORBELL_FLAG=--enable-host-doorbell;   HANDLER=cpu; ARM=host
fi
echo "=== 25_av_widths : arm=$ARM (NIC handler $HANDLER, $DOORBELL_FLAG) ==="

# Runtime BEFORE the launcher check, and LD_LIBRARY_PATH after the build block.
# Both orderings are the ones case 04 documents and case 12 paid for.
pcomm_load_runtime
command -v srun >/dev/null || fail "no srun on PATH"
# nvcc is still reached through mpic++ below, so mpirun's toolchain has to be
# present even though it does not launch anything here.

APP="${PCOMM_IBGDA_APP:-$HOME/nvshmem_putget_sm80}"
if [ ! -x "$APP" ]; then
  pcomm_load_build
  command -v "${PCOMM_NVCC:-nvcc}" >/dev/null 2>&1 || fail "no nvcc (PCOMM_SDK=${PCOMM_SDK:-unset})"
  [ -n "${NVSHMEM_HOME:-}" ] && [ -d "$NVSHMEM_HOME" ] || fail "NVSHMEM_HOME did not resolve"
  ${PCOMM_NVCC:-nvcc} -ccbin=mpic++ -O2 -rdc=true \
    -gencode arch="${PCOMM_CUDA_ARCH:-compute_80}",code="${PCOMM_CUDA_CODE:-sm_80}" \
    -Wno-deprecated-gpu-targets \
    "$PCOMM_ROOT/tests/hpc/cases/04_nvshmem/putget.cu" -o "$APP" \
    -I"$NVSHMEM_HOME/include" -L"$NVSHMEM_HOME/lib" \
    -lnvshmem_host -lnvshmem_device -lcudart || fail "workload did not build"
  pcomm_load_runtime
fi
export LD_LIBRARY_PATH="$NVSHMEM_HOME/lib:${NCCL_HOME:+$NCCL_HOME/lib:}${LD_LIBRARY_PATH:-}"

# srun + PMI-2, NOT mpirun + the MPI bootstrap that cases 07 and 12 use.
#
# This is the one thing here that is chosen by measurement rather than by
# convention, and the convention lost. The probes that first produced all three
# AV widths (jobs 45462619, 45580050, 45582508) launch this exact shape — two
# nodes, one PE each, one GPU — with srun --mpi=pmi2 and NVSHMEM_BOOTSTRAP=PMI.
# Written first with mpirun to match the neighbouring cases, this case hung in
# nvshmem_init and timed out with the application producing not one line (job
# 45584080). srun is already how cases 01, 08 and 23 launch, so this is inside
# the suite's vocabulary, not an exception to it.
#
# NVSHMEM_BOOTSTRAP is the FAMILY and NVSHMEM_BOOTSTRAP_PMI is the variant, and
# `PMI2` is not legal in the first. Getting that backwards costs four jobs and
# an errno 7 that prints as "No such file or directory" — see
# probes/uar_doorbell_mode/README.md, which has the receipts.
export NVSHMEM_BOOTSTRAP=PMI
export NVSHMEM_BOOTSTRAP_PMI=PMI-2
export NVSHMEM_DISABLE_P2P=1
export NVSHMEM_REMOTE_TRANSPORT=ibgda
export NVSHMEM_IB_ENABLE_IBGDA=1
export NVSHMEM_IBGDA_NIC_HANDLER="$HANDLER"
export NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE=gpumem

ELEMS=1024 ITERS=20

# The output directory is a parameter rather than derived from the leg name, so
# the roce_dc leg can write straight into $OUT — the capture the suite's
# cross-cutting assertions run against. The alternative was capturing to a
# per-leg directory and copying one of them over $OUT afterwards, which
# duplicates a multi-megabyte capture to save a function argument.
leg() {  # leg <name> <out-dir> <hca> <gid-or-empty> <extra NVSHMEM env...>
  local name="$1" out="$2" hca="$3" gid="$4"; shift 4
  rm -rf "$out" "$out.csv"; mkdir -p "$out"
  echo "######## leg $name : hca=$hca gid=${gid:-default} ($*) ########"
  # --export=ALL, so the exports below reach both ranks without the -x list
  # mpirun would need. That list is its own failure mode: an env var added to
  # the case and forgotten in the -x list is silently absent on every rank.
  ( export NVSHMEM_HCA_LIST="$hca:1"
    [ -n "$gid" ] && export NVSHMEM_IB_GID_INDEX="$gid"
    export "$@"
    timeout 300 srun --mpi=pmi2 -n 2 --nodes=2 --ntasks-per-node=1 \
      --gpus-per-task=1 --overlap --export=ALL \
      "$PCOMM" run -p ib,mlx5dv "$DOORBELL_FLAG" -o "$out" -- "$APP" "$ELEMS" "$ITERS" \
      > "$out.app" 2> "$out.err" )
  local rc=$?
  echo "-- leg $name rc=$rc --"
  grep -aE "host doorbell:|doorbell hijack:" "$out.err" 2>/dev/null | tail -3
  [ $rc -eq 0 ] || fail "leg $name did not complete (rc=$rc; 124 = timed out)"
  # The host arm proves the platform before it swaps anything. Only "DOES map"
  # means we picked the wrong arm; the other two decline branches are benign,
  # which case 12 learned by failing a correct run (MN5 job 45463226).
  if [ "$ARM" = host ] && grep -q "DOES map the NIC doorbell page" "$out.err" 2>/dev/null; then
    fail "host arm refused to arm on leg $name — this host supports GPU submission"
  fi
  "$PCOMM" export "$out" --csv "$out.csv" > /dev/null || fail "leg $name did not export"
  [ -f "$out.csv/mlx5_device_send.csv" ] || fail "leg $name decoded no sends at all"
}

leg rc      "$OUT.rc" "$IB_DEV"  ""         NVSHMEM_IBGDA_NUM_RC_PER_PE=2
leg dc      "$OUT.dc" "$IB_DEV"  ""         NVSHMEM_IBGDA_NUM_RC_PER_PE=0 NVSHMEM_IBGDA_NUM_DCI=1
leg roce_dc "$OUT"    "$ETH_DEV" "$ETH_GID" NVSHMEM_IBGDA_NUM_RC_PER_PE=0 NVSHMEM_IBGDA_NUM_DCI=1

CSV_rc="$OUT.rc.csv" CSV_dc="$OUT.dc.csv" CSV_roce_dc="$OUT.csv"
csv_of() { eval echo "\$CSV_$1/mlx5_device_send.csv"; }

# ---- per-leg: the width each leg exists to produce -------------------------
# awk over the CSV by HEADER NAME, not column number. mlx5_device_send has
# gained three columns this month (dest_dct, decode, av_ds) and a positional
# script would have silently read the wrong one after each — which is the same
# defect tools/check_csv_columns.py was written for, one layer out.
col() {  # col <csv> <name> -> 1-based index
  head -1 "$1" | tr ',' '\n' | grep -nx "$2" | cut -d: -f1
}

for pair in "rc 0" "dc 1" "roce_dc 3"; do
  set -- $pair
  f=$(csv_of "$1")
  # Squeezed and trimmed rather than compared against "$2 ": the trailing space
  # from `tr` is an artefact of formatting, and a check that depends on it is one
  # `sort -u` away from passing for the wrong reason.
  got=$(awk -F, -v o="$(col "$f" opcode_name)" -v a="$(col "$f" av_ds)" \
        'NR>1 && $o=="rdma_write" {print $a}' "$f" | sort -u | tr '\n' ' ' \
        | tr -s ' ' | sed 's/^ *//; s/ *$//')
  echo "=== leg $1 : av_ds seen = [$got] (expected $2) ==="
  [ -n "$got" ] || fail "leg $1 decoded no rdma_write rows — it exercised nothing"
  [ "$got" = "$2" ] || fail "leg $1 produced av_ds [$got], expected exactly $2"
done

# Every RDMA write must decode fully in every leg. A Partial or Truncated write
# here means a segment offset was not reachable, which is the failure the width
# derivation exists to prevent.
for l in rc dc roce_dc; do
  f=$(csv_of "$l")
  bad=$(awk -F, -v o="$(col "$f" opcode_name)" -v d="$(col "$f" decode_name)" \
        'NR>1 && $o=="rdma_write" && $d!="full"' "$f" | wc -l)
  [ "$bad" -eq 0 ] || fail "leg $l has $bad rdma_write rows that did not decode full"
done

# ---- THE CROSS-LEG CHECK ---------------------------------------------------
# Same workload, same iteration count, three different segment layouts. The
# (length, count) histogram of the RDMA writes must be identical, because a
# mis-derived AV width moves `length` and nothing else in this comparison can.
hist() {
  local f; f=$(csv_of "$1")
  awk -F, -v o="$(col "$f" opcode_name)" -v L="$(col "$f" length)" \
    'NR>1 && $o=="rdma_write" {print $L}' "$f" | sort -n | uniq -c
}
h_rc=$(hist rc); h_dc=$(hist dc); h_roce=$(hist roce_dc)
echo "=== rdma_write length histograms ==="
echo "rc:      $(echo "$h_rc"   | tr '\n' ';')"
echo "dc:      $(echo "$h_dc"   | tr '\n' ';')"
echo "roce_dc: $(echo "$h_roce" | tr '\n' ';')"
[ -n "$h_rc" ] || fail "the rc leg decoded no rdma_write rows — nothing to compare against"
[ "$h_rc" = "$h_dc" ] || \
  fail "compact-AV leg disagrees with RC on rdma_write lengths — the 16-byte AV offset is wrong"
[ "$h_rc" = "$h_roce" ] || \
  fail "full-AV leg disagrees with RC on rdma_write lengths — the 48-byte AV offset is wrong"

# ---- assert the full-AV capture --------------------------------------------
# roce_dc captured straight into $OUT because it carries the width that exists
# nowhere else on the board, so it is the one the suite's cross-cutting
# assertions — av_width_is_stable_per_qp, dc_send_names_its_destination,
# atomic_operand_is_a_word — should run against.
echo "=== captured streams (roce_dc) ==="; "$PCOMM" read "$OUT"
pcomm_derive "$OUT"
bash "$PCOMM_ROOT/tests/hpc/suite/sql_check.sh" "$OUT.csv" "$CASE_DIR"
