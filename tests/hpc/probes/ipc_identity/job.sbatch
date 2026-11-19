#!/bin/bash
#SBATCH --job-name=comshu-ipc-identity
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=20
#SBATCH --gres=gpu:2
#SBATCH --time=00:05:00
#SBATCH --output=ipc-identity-%j.out
#
# PROBE — is there an identity, visible to the importer's driver calls, that
# ties an imported cuMem mapping to the exporter's allocation? See probe.cu for
# the candidates and how a verdict is reached. Bare, no tracer: the question is
# about the driver, and a tracer in the process could only confuse it.
#
#   sbatch tests/hpc/probes/ipc_identity/job.sbatch    (from ~/proj/comshu)
set -x
module load gcc/13.2.0 nvidia-hpc-sdk/25.7 cuda/12.6 >/dev/null 2>&1
DIR="${COMSHU_ROOT:-$HOME/proj/comshu}/tests/hpc/probes/ipc_identity"
BIN="$HOME/ipc-identity-$SLURM_JOB_ID"
nvidia-smi -L
cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null
nvcc -O1 -o "$BIN" "$DIR/probe.cu" -lcuda || { echo "BUILD FAILED"; exit 1; }
"$BIN"
echo "probe exit=$?"
# Twice: an identity must not depend on which run it was.
"$BIN"
echo "probe exit=$?"
