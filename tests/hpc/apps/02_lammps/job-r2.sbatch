#!/bin/bash
# 02_lammps ROUND 2 — LAMMPS/KOKKOS at 16 ranks over 4 nodes under
# `-p ucx,ucx_deep`, to ask whether comm_edges.scope and transport_mix.scope put
# the right bytes on the right side of the node boundary.
#
# The oracle is PREDICTION-r2.md, committed in d1c4bd2 before this script had
# ever run.
#
# FOUR executions in ONE allocation, in this order and for these reasons:
#
#   1. bare,  --map-by ppr:4:node   \  the workload works, AND the three oracle
#   2. bare,  --map-by node         /  lines are read here: the processor grid,
#                                      Nghost, and hostname-per-rank.
#   3. pcomm, --map-by ppr:4:node      the primary capture   (placement `block`)
#   4. pcomm, --map-by node            the inverted control  (placement `cyclic`)
#
# The bare pair is not optional (apps/README.md: "a capture of a workload that
# did not work is indistinguishable from a tracer that captured nothing").
# Runs 1 and 2 are also where the rank-to-node map is established BY THE
# OPERATING SYSTEM rather than by rank_identity, which is what stops
# scope_follows_the_launcher_placement.sql from being pcomm marking its own
# homework.
#
#   sbatch tests/hpc/apps/02_lammps/job-r2.sbatch
#
# ONE JOB AT A TIME, ACCOUNT-WIDE. Before submitting:
#   while squeue -u koc296797 -h | grep -q .; do sleep 60; done
#
#SBATCH --job-name=pcomm-app-lammps-r2
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=01:00:00
#SBATCH --output=%x.%j.out

set -x
PCOMM_ROOT="${PCOMM_ROOT:-$HOME/proj/pcomm}"
CASE="$PCOMM_ROOT/tests/hpc/apps/02_lammps"

# ---------------------------------------------------------------- environment
# The LAMMPS module's own prereq set, in its own order. `module purge` first
# BREAKS it — the modulefile activates a conda env through miniforge and needs
# bsc/1.0 alive (round 1 NOTES.md #3). common.sh's own GPP-inheritance purge is
# therefore run BEFORE these loads, by sourcing it first.
source "$PCOMM_ROOT/tests/hpc/suite/common.sh"
module load cuda/12.6 openmpi/4.1.5-gcc fftw/3.3.10-gcc-ompi gsl/2.8-gcc mkl
module load lammps/10Sep2025

# NOT pcomm_load_runtime: it loads nvidia-hpc-sdk/25.7, which puts the SDK's own
# mpirun ahead of the one LAMMPS is linked against (libmpi.so.40 from
# /apps/ACC/OPENMPI/4.1.5/GCC). All pcomm needs from it is gcc-13.2's libstdc++.
module load gcc/13.2.0

# THE BUILD. build-apps06 is a PRIVATE tree at the repo checkout, built for this
# study, so that nothing here depends on or disturbs the shared $PCOMM_ROOT/build
# another agent may be using (apps/README.md: do not rebuild the shared one).
# Its ucx and ucx_deep providers are built against /usr/include and
# /usr/lib64/libucp.so — the ucx-devel-1.20.0 package — i.e. against the SAME
# UCX 1.20.0 in /lib64 that this OpenMPI's mca_pml_ucx.so runs on.
#
# That is the root fix for round 1's biggest trap. Round 1 used providers built
# against HPC-X 2.22.1's UCX, whose absolute RUNPATH dragged HPC-X's libucp into
# a process using the system libuct/libucs, and ucp_ep_create segfaulted inside
# ucp_wireup_select_transport during MPI_Init; the workaround was to append
# /lib64 to LD_LIBRARY_PATH so the system UCX won everywhere. Building against
# the system headers removes both the hybrid and the reason for the workaround —
# and it matters far more for round 2 than round 1, because ucx_deep patches
# uct_iface_ops_t, an installed struct with no ABI promise, and must be looking
# at the same layout the running library was compiled with.
export PCOMM_BUILD="${PCOMM_BUILD_R2:-$PCOMM_ROOT/build-apps06}"
export PCOMM="$PCOMM_BUILD/pcomm/pcomm"
export PCOMM_PROVIDER_PATH="$PCOMM_BUILD/providers-sysucx/ucx:$PCOMM_BUILD/providers-sysucx/ucx_deep"
export LD_LIBRARY_PATH="$PCOMM_BUILD/libpcomm:${LD_LIBRARY_PATH:-}:/lib64"
unset PCOMM_UCX_DEEP_FORCE          # a forced binding is not an ordinary pass
# /lib64 APPENDED, never prepended. It is belt-and-braces now that the providers
# carry no UCX in their RUNPATH — nothing should be able to offer a different
# libucp — but a module that quietly put one on LD_LIBRARY_PATH would silently
# recreate round 1's hybrid, and appending costs nothing. Prepending would
# shadow gcc-13.2's libstdc++ with /lib64's and every pcomm binary would then die
# with `GLIBCXX_3.4.30 not found`, which reads as a completely different bug
# (round 1 NOTES.md trap 1).

echo "=== stack ==="
which lmp_mpi mpirun
"$PCOMM" --version 2>/dev/null || true
"$PCOMM" types | wc -l
echo "--- is GpuDevice (type 528) in this build's type list? PREDICTION-r2.md §4 says no"
"$PCOMM" types | grep -E '^(524|526|528|530)\b' || true
for so in providers-sysucx/ucx/libucx_trace.so providers-sysucx/ucx_deep/libucx_deep_trace.so; do
  echo "--- $so"
  ldd "$PCOMM_BUILD/$so" 2>&1 | grep -E 'libucp|libuct|libucs|not found'
done
rpm -q ucx 2>/dev/null || true

# Captures go to SCRATCH, not $HOME. gpfs_home is at 78.6 of 80 GB for this
# account and a 16-rank ucx_deep capture is not something to spend the last
# gigabyte on.
WORK="${PCOMM_APPS_ROOT:-/gpfs/scratch/etur02/koc296797-apps}/lammps-r2-$SLURM_JOB_ID"
mkdir -p "$WORK" || { echo "!! cannot create $WORK"; exit 1; }
cd "$WORK" || exit 1

# ------------------------------------------------------------------ the input
# bench/in.lj from lammps-patch_10Sep2025, verbatim except the three marked
# lines. `processors 2 2 4 map xyz` is the oracle: `map xyz` replaces the
# MPI_Cart_create default with the explicit function at src/procmap.cpp:409, so
# the neighbour set is a statement in LAMMPS's source rather than something Open
# MPI's Cartesian reordering happened to produce.
cat > in.lj <<'IN'
# 3d Lennard-Jones melt  (LAMMPS bench/in.lj)
variable        x index 2
variable        y index 2
variable        z index 2
variable        xx equal 20*$x
variable        yy equal 20*$y
variable        zz equal 20*$z
units           lj
atom_style      atomic
processors      2 2 4 map xyz          # <- added: the oracle
lattice         fcc 0.8442
region          box block 0 ${xx} 0 ${yy} 0 ${zz}
create_box      1 box
create_atoms    1 box
mass            1 1.0
velocity        all create 1.44 87287 loop geom
pair_style      lj/cut 2.5
pair_coeff      1 1 1.0 1.0 2.5
neighbor        0.3 bin
neigh_modify    delay 0 every 20 check no
fix             1 all nve
thermo          100                    # <- added: one thermo block, not 10
run             100
IN

LMP=(lmp_mpi -k on g 4 -sf kk -in in.lj -log none)
MPI_ENV=(-x PATH -x LD_LIBRARY_PATH -x PCOMM_PROVIDER_PATH -x CUDA_VISIBLE_DEVICES
         -x OMPI_MCA_pml=ucx -x OMPI_MCA_osc=ucx)

# THE TWO PLACEMENTS. Everything else is identical between the arms.
map_args() {
  case "$1" in
    block)  echo "--map-by ppr:4:node" ;;
    cyclic) echo "--map-by node" ;;
  esac
}

# --------------------------------------------------------- 0. WHERE THE RANKS
# ARE, from the operating system. Printed for both placements BEFORE any capture
# exists, so that comm_edges.scope is judged against the launcher and not
# against another pcomm table. If these two blocks come out identical the
# experiment has not happened and everything below is one arm run twice.
for arm in block cyclic; do
  echo "=== PLACEMENT ORACLE ($arm): rank -> host ==="
  # shellcheck disable=SC2046
  mpirun -np 16 $(map_args "$arm") --bind-to none "${MPI_ENV[@]}" \
      bash -c 'printf "RANKHOST rank=%03d local=%s host=%s\n" "$OMPI_COMM_WORLD_RANK" "$OMPI_COMM_WORLD_LOCAL_RANK" "$(hostname)"' \
    2>&1 | grep RANKHOST | sort
done

# --------------------------------------------------------------- 1. BARE runs
run_bare() {                          # $1 = block|cyclic
  local arm="$1"
  echo "=== BARE $arm ==="
  # shellcheck disable=SC2046
  mpirun -np 16 $(map_args "$arm") --bind-to none "${MPI_ENV[@]}" \
      "${LMP[@]}" 2>&1 | tee "bare-$arm.log" | tail -40
  echo "bare_${arm}_exit=${PIPESTATUS[0]}"
}

run_bare block
run_bare cyclic

# ------------------------------------------------------------- THE ORACLES ---
# Read from LAMMPS's own output, not from a capture, and printed before any
# capture exists. PREDICTION-r2.md P1.3 and P2.4.
echo "=== LAMMPS ORACLES (from LAMMPS, not from pcomm) ==="
grep -E "MPI processor grid|Ghost atom cutoff|master list distance cutoff|Nlocal:|Nghost:|Neighs:|Loop time|will use up to|using view layout|unified memory" bare-block.log
echo "--- predicted: '2 by 2 by 4 MPI processor grid', Nlocal ave 16000, Nghost ave ~13041"
NGHOST="$(sed -n 's/^Nghost:[[:space:]]*\([0-9.]*\).*/\1/p' bare-block.log | head -1)"
echo "nghost_ave = ${NGHOST:-<not printed>}  (PREDICTION-r2.md P2.4 predicts ~13041)"
echo "--- wall clock, both placements: cyclic moves 55.7 % of its halo over IB, block 44.3 %"
grep -H "Loop time" bare-block.log bare-cyclic.log || true

# ------------------------------------------------------------ 2. the captures
run_traced() {                        # $1 = block|cyclic
  local arm="$1" out
  out="$WORK/lammps-r2-$arm-$SLURM_JOB_ID"
  rm -rf "$out" "$out.csv" "$out.pq"; mkdir -p "$out"
  echo "=== CAPTURE $arm  (-p ucx,ucx_deep) ==="
  # shellcheck disable=SC2046
  mpirun -np 16 $(map_args "$arm") --bind-to none "${MPI_ENV[@]}" \
      "$PCOMM" run -p ucx,ucx_deep -o "$out" -- "${LMP[@]}" 2>&1 | tail -25
  echo "capture_${arm}_exit=$?"

  # A capture directory with no rank-* subdirectory means the workload never
  # started. Read this before reading anything else.
  ls -d "$out"/rank-* >/dev/null 2>&1 || { echo "!! NO rank-* IN $out — the workload never started"; return 1; }

  echo "--- captured streams ($arm) ---"; "$PCOMM" read "$out"
  # uct_binding FIRST, always. accepted=0 is why uct_send would be empty and the
  # record exists to say so. PREDICTION-r2.md failure mode 3.
  echo "--- uct_binding ($arm) ---"; "$PCOMM" dump "$out" 806 2>/dev/null | head -20 || true
  "$PCOMM" export "$out" --csv "$out.csv"
  pcomm_derive "$out"
  return 0
}

run_traced block
run_traced cyclic

# ------------------------------------------------------------- 3. assertions
for arm in block cyclic; do
  echo "########## ASSERTIONS: $arm ##########"
  bash "$CASE/run-asserts-r2.sh" "$WORK/lammps-r2-$arm-$SLURM_JOB_ID.pq" "$CASE" "$arm"
  echo "asserts_${arm}_exit=$?"
done

# ------------------------------------------------- 4. the report's raw numbers
# Printed so REPORT-r2.md can quote them without a second job. Every one is a
# query a reader can re-run.
for arm in block cyclic; do
  PQ="$WORK/lammps-r2-$arm-$SLURM_JOB_ID.pq"
  echo "########## NUMBERS: $arm ##########"
  echo "=== comm_edges, every row ==="
  "$PCOMM" sql "$PQ" "SELECT src_rank, dst_rank, transport, unit, scope, sends, bytes FROM comm_edges ORDER BY src_rank, bytes DESC"
  echo "=== comm_edges rolled up by scope ==="
  "$PCOMM" sql "$PQ" "SELECT scope, count(*) AS edges, sum(sends) AS sends, sum(bytes) AS bytes FROM comm_edges GROUP BY scope ORDER BY bytes DESC"
  echo "=== the network share of EDGE bytes (PREDICTION-r2.md P2.3: block 55.68 %, cyclic 44.32 %) ==="
  "$PCOMM" sql "$PQ" "SELECT sum(CASE WHEN scope='network' THEN bytes ELSE 0 END) AS net, sum(CASE WHEN scope='intra-node' THEN bytes ELSE 0 END) AS intra, sum(CASE WHEN scope NOT IN ('network','intra-node') OR scope IS NULL THEN bytes ELSE 0 END) AS unk FROM comm_edges"
  echo "=== per-rank per-peer shares, rank 0 (predicted 20.45 / 23.86 / 27.84 / 27.84) ==="
  "$PCOMM" sql "$PQ" "SELECT dst_rank, scope, sends, bytes, (10000*bytes)/(SELECT sum(bytes) FROM comm_edges WHERE src_rank=0) AS share_bp FROM comm_edges WHERE src_rank=0 ORDER BY bytes DESC"
  echo "=== transport_mix, full ==="
  "$PCOMM" sql "$PQ" "SELECT rank, tl_name, dev_name, scope, op, ops, out_bytes, in_bytes, advertised_bytes, header_bytes FROM transport_mix ORDER BY rank, tl_name, op"
  echo "=== transport_mix rolled up by scope (the wire answer to the same question) ==="
  "$PCOMM" sql "$PQ" "SELECT scope, count(*) AS rows, sum(ops) AS ops, sum(out_bytes) AS out_b, sum(in_bytes) AS in_b, sum(advertised_bytes) AS adv_b, sum(header_bytes) AS hdr_b FROM transport_mix GROUP BY scope ORDER BY scope"
  echo "=== the tl_name -> scope mapping this run produced ==="
  "$PCOMM" sql "$PQ" "SELECT scope, tl_name, dev_name, count(*) AS rows, sum(ops) AS ops FROM transport_mix GROUP BY 1,2,3 ORDER BY scope, tl_name"
  echo "=== rank_identity ==="
  "$PCOMM" sql "$PQ" "SELECT rank, node_ix, node_hash, device, devices, label FROM rank_identity ORDER BY rank"
  echo "=== how lossy is the UCX cross-rank join (round 1 D1 measured 0 of 621) ==="
  "$PCOMM" sql "$PQ" "SELECT s.rank, count(*) AS sends, count(c.endpoint) AS with_ep_connection, count(w.prefix_hash) AS with_peer_rank FROM ucx_tag_send s LEFT JOIN ucx_ep_connection c ON c.rank=s.rank AND c.endpoint=s.endpoint LEFT JOIN ucx_worker_address w ON w.prefix_hash=c.remote_prefix_hash GROUP BY 1 ORDER BY 1"
  echo "=== ucx_tag_send totals per rank (the denominator for edge coverage) ==="
  "$PCOMM" sql "$PQ" "SELECT rank, count(*) AS sends, sum(count) AS bytes FROM ucx_tag_send GROUP BY 1 ORDER BY 1"
  echo "=== tag_flow status distribution ==="
  "$PCOMM" sql "$PQ" "SELECT status, count(*) AS flows, sum(ucp_sends) AS ucp_sends, sum(uct_ops) AS uct_ops FROM tag_flow GROUP BY status" || true
  echo "=== link_health rubric ==="
  "$PCOMM" sql "$PQ" "SELECT finding, n, note FROM link_health ORDER BY n DESC"
  echo "=== is there a gpu_device table? PREDICTION-r2.md §4 predicts no ==="
  ls "$PQ" | grep -i gpu || echo "  (no gpu_* parquet in the export)"
done

echo "=== artefacts ==="
ls -la "$WORK"
du -sb "$WORK"/lammps-r2-*-"$SLURM_JOB_ID"* 2>/dev/null   # -sb, NOT -sh: GPFS
                                                          # block accounting lies
                                                          # about a fresh directory
