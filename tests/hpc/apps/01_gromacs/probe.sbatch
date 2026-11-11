#!/bin/bash
# probe.sbatch — settle ONE question the main job (45082564) left open.
#
# The main job's direct-GPU-comm arm demonstrably took the direct path (mdrun
# log: "GPU-aware MPI detected, enabling direct GPU communication", +19 % ns/day)
# and yet every one of its 1,564 NIC registrations resolved to HOST memory
# (mem_object.device = 65535 for all of them, zero ibv_reg_dmabuf_mr rows). Two
# readings survive that, and they are opposites:
#
#   H1  UCX never did GPUDirect RDMA. It pipelined the device buffers through
#       host bounce buffers, so the NIC really only ever saw host memory and
#       comshu's capture is CORRECT and complete.
#   H2  UCX did register device memory, through mlx5dv_devx_umem_reg — which
#       `libuct_ib.so.0` imports twice — and comshu was BLIND to it because the
#       main job ran `-p ib,ucx` and the mlx5dv provider was not loaded.
#
# Two things separate them, and both are cheap:
#   * add the mlx5dv provider. If H2, mlx5_umem rows appear carrying device
#     memory; if H1, they carry the same host ranges ib_mem_region already has.
#   * UCX_LOG_LEVEL=info makes uct_ib_check_gpudirect_driver() print
#     "GPUDirect RDMA is enabled/disabled" per device. That is UCX's own
#     statement about H1 vs H2 and it costs nothing.
#
# 200 steps, ON arm only — this is a door probe, not a measurement.
#
#SBATCH --job-name=comshu-app-gromacs-probe
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:4
#SBATCH --time=00:25:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
source "$COMSHU_ROOT/tests/hpc/suite/common.sh"
comshu_load_runtime
module load fftw/3.3.10-gcc-nvhpcx
module load gromacs/2025.2-gcc-nvhpcx
HPCX=/gpfs/apps/MN5/ACC/NVIDIA-HPC-SDK/23.11/Linux_x86_64/23.11/comm_libs/12.3/hpcx/hpcx-2.16
export PATH="$HPCX/ompi/bin:$PATH"
export LD_LIBRARY_PATH="$HPCX/ucx/lib:$HPCX/ompi/lib:$LD_LIBRARY_PATH"
export OPAL_PREFIX="$HPCX/ompi"
export SLURM_CPU_BIND=none
unset GMX_ENABLE_DIRECT_GPU_COMM
unset GMX_DISABLE_DIRECT_GPU_COMM

WORK="$HOME/comshu-apps/gromacs"
RUN="$WORK/probe-$SLURM_JOB_ID"; mkdir -p "$RUN"; cd "$RUN" || exit 1

# What UCX itself thinks it can do with GPU memory, from a compute node.
echo "=== ucx_info: registration methods and GPUDirect ==="
"$HPCX/ucx/bin/ucx_info" -c 2>/dev/null | grep -iE 'REG_METHODS|GPU_DIRECT|DMABUF|RNDV_SCHEME|RNDV_THRESH' | head -20
"$HPCX/ucx/bin/ucx_info" -d 2>/dev/null | grep -iE 'gdr|cuda|Memory domain' | head -20

OUT="$WORK/gromacs-probe-$SLURM_JOB_ID"; rm -rf "$OUT" "$OUT.csv"; mkdir -p "$OUT"
mpirun -np 8 --map-by ppr:4:node --bind-to none \
  -x PATH -x LD_LIBRARY_PATH -x OPAL_PREFIX -x COMSHU_PROVIDER_PATH \
  -x UCX_LOG_LEVEL=info \
  "$COMSHU" run -p ib,ucx,mlx5dv -o "$OUT" -- \
  gmx_mpi mdrun -s "$WORK/benchRIB.tpr" -nsteps 200 -npme 1 -dd 7 1 1 -ddorder pp_pme \
    -nb gpu -pme gpu -notunepme -dlb no -nstlist 100 \
    -ntomp "${SLURM_CPUS_PER_TASK:-20}" -pin on -noconfout \
    -g probe.log -e probe.edr -o probe.trr
echo "probe exit=$?"

echo "=== GPUDirect verdict, straight from UCX ==="
grep -hiE 'GPUDirect RDMA is|gdr_copy|dmabuf|nv_peer_mem' "$SLURM_SUBMIT_DIR"/comshu-app-gromacs-probe."$SLURM_JOB_ID".out 2>/dev/null | sort -u | head -20

echo "=== captured streams ==="; "$COMSHU" read "$OUT"
"$COMSHU" export "$OUT" --csv "$OUT.csv"
grep -c . "$OUT.csv"/mlx5_umem.csv "$OUT.csv"/mlx5_device_send.csv "$OUT.csv"/ib_post_send.csv 2>/dev/null
