#!/bin/bash
# Which kernel issued a GPU-initiated put? Measure before designing the join.
#
# Phase 4 of plans/PLAN-20261109-1354-device-sends-have-no-buffer. A decoded
# device send carries the time its DOORBELL rang. Under the host doorbell that
# is a CPU proxy thread, which rings after the GPU wrote the WQE and possibly
# after the issuing kernel has ended. So "the kernel running on the queue's GPU
# at the doorbell's time" may be wrong, and how wrong is the question.
#
# This runs case 26 (NCCL GIN put-ring, one kernel launch per iteration) with
# cupti added, so the capture holds gpu_kernel windows next to mlx5_device_send.
# The case's own assertions still run on it. Then, offline:
#   lag = doorbell time - end of the latest kernel on that device that started
#         before it; and how many kernels' windows contain each doorbell.
#
#   sbatch --account=etur02 --qos=acc_debug tests/hpc/probes/kernel_attr/mn5.sbatch
#
#SBATCH --job-name=comshu-kattr
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:1
#SBATCH --time=00:15:00
#SBATCH --output=kattr_%j.out
export COMSHU_GIN_PROVIDERS=ib,mlx5dv,cupti
exec bash "${COMSHU_ROOT:-$HOME/proj/comshu}/tests/hpc/cases/26_nccl_gin/job.sbatch"
