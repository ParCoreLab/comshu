#!/bin/bash
# 02_lammps — LAMMPS/KOKKOS (CUDA) under comshu on MareNostrum 5.
#
# Two questions, in one allocation (tests/hpc/apps/README.md: "do not spend a
# slot on a single question"):
#
#   (A) Does this build allocate MANAGED memory?  plans/todos.txt item 1 says
#       LAMMPS/Kokkos "has a CudaUVM backend" and would make mem_object.device's
#       fixed-at-allocation residency a LIVE defect.  The capture cannot answer
#       that — mem_object has no discriminator for the allocation door — so the
#       run carries an LD_PRELOAD counter over the four CUDA allocation doors
#       instead.  Built here, from source, so the answer is reproducible.
#
#   (B) What does `-pk kokkos gpu/aware on|off` change on the wire?  From
#       src/KOKKOS/kokkos.cpp:615-641 the flag flips forward/reverse/exchange
#       comm to the "legacy" (host-staged) path, and from
#       src/KOKKOS/comm_kokkos.cpp:136-157 the legacy path calls
#       CommBrick::forward_comm — the SAME loop, over the SAME swap table, with
#       a host pointer instead of a device pointer.  So the MESSAGE COUNT AND
#       SIZES MUST NOT CHANGE.  If the capture shows fewer messages with
#       gpu/aware on, that is a door comshu does not hook, not a speedup.
#
# Both configurations run bare first.  A capture of a workload that did not
# work is indistinguishable from a tracer that captured nothing.
#
#   sbatch tests/hpc/apps/02_lammps/job.sbatch
#
#SBATCH --job-name=comshu-app-lammps
#SBATCH --account=etur02
#SBATCH --qos=acc_debug
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --gres=gpu:4
#SBATCH --time=00:45:00
#SBATCH --output=%x.%j.out

set -x
COMSHU_ROOT="${COMSHU_ROOT:-$HOME/proj/comshu}"
CASE="$COMSHU_ROOT/tests/hpc/apps/02_lammps"

# ---------------------------------------------------------------- environment
# The LAMMPS module's own prereq set, in its own order.  `module purge` first
# BREAKS it (the lammps modulefile activates a conda env through miniforge and
# needs bsc/1.0 alive) — see NOTES.md.
module load cuda/12.6 openmpi/4.1.5-gcc fftw/3.3.10-gcc-ompi gsl/2.8-gcc mkl
module load lammps/10Sep2025

source "$COMSHU_ROOT/tests/hpc/suite/common.sh"

# NOT comshu_load_runtime.  That loads nvidia-hpc-sdk/25.7, which puts the SDK's
# own mpirun ahead of the one LAMMPS is linked against (libmpi.so.40 from
# /apps/ACC/OPENMPI/4.1.5).  All comshu needs from it here is gcc-13.2's
# libstdc++ (GLIBCXX_3.4.30+).
module load gcc/13.2.0

# THE UCX ABI FIX, and it is load-bearing.  libucx_trace.so carries an absolute
# RUNPATH to HPC-X 2.22.1's UCX, while this OpenMPI's mca_pml_ucx.so is built
# against the SYSTEM UCX 1.20.0 in /lib64.  Left alone the loader mixes them —
# HPC-X libucp with system libuct/libucs — and ucp_ep_create segfaults inside
# ucp_wireup_select_transport during MPI_Init.  /lib64 anywhere on
# LD_LIBRARY_PATH beats a RUNPATH (new dtags), and APPENDING keeps gcc-13.2's
# libstdc++ ahead of /lib64's.  See NOTES.md.
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/lib64"

export COMSHU_OUT_ROOT="${COMSHU_OUT_ROOT:-$HOME/comshu-apps}"
WORK="$HOME/comshu-apps/lammps-work-${SLURM_JOB_ID}"
mkdir -p "$WORK"; cd "$WORK"

MPI_ENV=(-x PATH -x LD_LIBRARY_PATH -x COMSHU_PROVIDER_PATH -x CUDA_VISIBLE_DEVICES
         -x OMPI_MCA_pml=ucx -x OMPI_MCA_osc=ucx)
LAUNCH=(mpirun -np 8 --map-by ppr:4:node --bind-to none "${MPI_ENV[@]}")

# ---------------------------------------------------------- (A) alloc counter
# Counts the CUDA allocation doors by interposition.  Kokkos 4.x compiles
# CudaUVMSpace::impl_allocate whether or not UVM is enabled, so
# `nm -D --undefined-only` shows cudaMallocManaged in EVERY Kokkos/CUDA binary —
# that test is a false positive and this one is not.
cat > alloc_probe.c <<'PROBE'
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
static long c_dev, c_managed, c_host, c_drv_dev, c_drv_managed, c_drv_host;
static long c_dev_async, c_drv_dev_async, c_drv_pool_async, c_host_alloc;
static unsigned long long b_dev, b_managed, b_dev_async;
typedef int (*t2)(void **, size_t);
typedef int (*t3)(void **, size_t, unsigned int);
typedef int (*t2d)(unsigned long long *, size_t);
typedef int (*t3d)(unsigned long long *, size_t, unsigned int);
typedef int (*t3s)(void **, size_t, void *);
typedef int (*t4s)(void **, size_t, void *, void *);
typedef int (*t3ds)(unsigned long long *, size_t, void *);
typedef int (*t4ds)(unsigned long long *, size_t, void *, void *);
#define NEXT(v, ty, nm) static ty v; if (!v) v = (ty)dlsym(RTLD_NEXT, nm);
int cudaMalloc(void **p, size_t s) { NEXT(f,t2,"cudaMalloc") __sync_fetch_and_add(&c_dev,1); b_dev+=s; return f(p,s); }
int cudaMallocManaged(void **p, size_t s, unsigned int fl) { NEXT(f,t3,"cudaMallocManaged") __sync_fetch_and_add(&c_managed,1); b_managed+=s; return f(p,s,fl); }
int cudaMallocHost(void **p, size_t s) { NEXT(f,t2,"cudaMallocHost") __sync_fetch_and_add(&c_host,1); return f(p,s); }
int cudaHostAlloc(void **p, size_t s, unsigned int fl) { NEXT(f,t3,"cudaHostAlloc") __sync_fetch_and_add(&c_host_alloc,1); return f(p,s,fl); }
int cudaMallocAsync(void **p, size_t s, void *st) { NEXT(f,t3s,"cudaMallocAsync") __sync_fetch_and_add(&c_dev_async,1); b_dev_async+=s; return f(p,s,st); }
int cudaMallocFromPoolAsync(void **p, size_t s, void *pl, void *st) { NEXT(f,t4s,"cudaMallocFromPoolAsync") __sync_fetch_and_add(&c_dev_async,1); b_dev_async+=s; return f(p,s,pl,st); }
int cuMemAlloc_v2(unsigned long long *p, size_t s) { NEXT(f,t2d,"cuMemAlloc_v2") __sync_fetch_and_add(&c_drv_dev,1); return f(p,s); }
int cuMemAllocManaged(unsigned long long *p, size_t s, unsigned int fl) { NEXT(f,t3d,"cuMemAllocManaged") __sync_fetch_and_add(&c_drv_managed,1); return f(p,s,fl); }
int cuMemAllocHost_v2(void **p, size_t s) { NEXT(f,t2,"cuMemAllocHost_v2") __sync_fetch_and_add(&c_drv_host,1); return f(p,s); }
int cuMemAllocAsync(unsigned long long *p, size_t s, void *st) { NEXT(f,t3ds,"cuMemAllocAsync") __sync_fetch_and_add(&c_drv_dev_async,1); return f(p,s,st); }
int cuMemAllocFromPoolAsync(unsigned long long *p, size_t s, void *pl, void *st) { NEXT(f,t4ds,"cuMemAllocFromPoolAsync") __sync_fetch_and_add(&c_drv_pool_async,1); return f(p,s,pl,st); }
__attribute__((destructor)) static void report(void) {
    const char *r = getenv("OMPI_COMM_WORLD_RANK"); if (!r) r = getenv("SLURM_PROCID"); if (!r) r = "?";
    fprintf(stderr, "ALLOC_PROBE rank=%s pid=%d cudaMalloc=%ld(%lluB) cudaMallocManaged=%ld(%lluB) "
            "cudaMallocHost=%ld cudaHostAlloc=%ld cudaMallocAsync=%ld(%lluB) cuMemAlloc=%ld "
            "cuMemAllocManaged=%ld cuMemAllocHost=%ld cuMemAllocAsync=%ld cuMemAllocFromPoolAsync=%ld\n",
            r, (int)getpid(), c_dev, b_dev, c_managed, b_managed, c_host, c_host_alloc,
            c_dev_async, b_dev_async, c_drv_dev, c_drv_managed, c_drv_host, c_drv_dev_async, c_drv_pool_async);
    fflush(stderr);
}
PROBE
gcc -O2 -fPIC -shared -o alloc_probe.so alloc_probe.c -ldl

# ------------------------------------------------------------------ the input
# bench/in.lj from the LAMMPS distribution, verbatim except for the three lines
# marked below.  `processors 2 2 2` is the whole point: it makes the processor
# grid an ORACLE rather than something the automatic factorization in
# src/comm.cpp picked, and with 4 ranks/node and MPI_Cart row-major ordering
# (src/procmap.cpp ProcMap::cart_map, reorder=0) it puts the x-neighbour — and
# only the x-neighbour — on the other node.
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
processors      2 2 2            # <- added: fixes the oracle
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
thermo          100              # <- added: one thermo block, not 10
run             100
IN

LMP=(lmp_mpi -k on g 4 -sf kk -in in.lj -log none)

# --------------------------------------------------------------- 1. BARE runs
# Not optional.  These are what separate "comshu captured nothing" from "the
# workload never ran", and the gpu/aware=on one carries the (A) probe.
echo "=== BARE gpu/aware on (+ alloc_probe) ==="
"${LAUNCH[@]}" -x LD_PRELOAD="$WORK/alloc_probe.so" \
    "${LMP[@]}" -pk kokkos gpu/aware on 2>&1 | tee bare-on.log | tail -60
echo "bare_on_exit=${PIPESTATUS[0]}"
echo "--- ALLOC_PROBE lines ---"; grep ALLOC_PROBE bare-on.log

echo "=== BARE gpu/aware off ==="
"${LAUNCH[@]}" "${LMP[@]}" -pk kokkos gpu/aware off 2>&1 | tee bare-off.log | tail -40
echo "bare_off_exit=${PIPESTATUS[0]}"

echo "--- what LAMMPS says about its own decomposition and memory space ---"
grep -E "processor grid|using unified memory|using view layout|Kokkos|GPU-aware|will use up to" bare-on.log

# ------------------------------------------------------- 2. captured runs
run_capture() {                     # $1 = label, $2 = providers, $3 = on|off
  local out; out="$(comshu_out "lammps-$1")"; rm -rf "$out" "$out.csv" "$out.pq"
  echo "=== CAPTURE $1  providers=$2  gpu/aware=$3 ==="
  "${LAUNCH[@]}" "$COMSHU" run -p "$2" -o "$out" -- "${LMP[@]}" -pk kokkos gpu/aware "$3" \
      2>&1 | tail -25
  echo "capture_${1}_exit=$?"
  if [ ! -d "$out/rank-0" ]; then echo "!! $1: NO rank-0 — the workload never started"; return 1; fi
  echo "--- streams ---"; "$COMSHU" read "$out" | head -40
  "$COMSHU" export "$out" --csv "$out.csv"
  comshu_derive "$out"
  echo "--- non-empty CSV tables ---"
  for f in "$out.csv"/*.csv; do
    n=$(( $(wc -l < "$f") - 1 )); [ "$n" -gt 0 ] && printf "  %-26s %d\n" "$(basename "$f" .csv)" "$n"
  done
  return 0
}

run_capture ucx-on  ib,ucx on
run_capture ucx-off ib,ucx off
run_capture ib-on   ib      on
run_capture ib-off  ib      off

# -------------------------------------------------------------- 3. the answer
ON="$(comshu_out lammps-ucx-on).pq"
OFF="$(comshu_out lammps-ucx-off).pq"

# The UCX rank matrix.  comm_edges is built from ib_post_send / mlx5_device_send
# / gpu_access only (comshu-read/link_sql.hpp:1112) — nothing UCX contributes to
# it — so the rank-space topology of an MPI application on this stack has to be
# assembled by hand, through the one join that crosses a rank boundary:
#   ucx_tag_send.endpoint -> ucx_ep_connection.remote_prefix_hash
#                         -> ucx_worker_address.prefix_hash -> that worker's rank
MATRIX='
SELECT s.rank AS src_rank, w.rank AS dst_rank,
       count(*) AS sends, sum(s.count) AS bytes
FROM ucx_tag_send s
JOIN ucx_ep_connection c ON c.rank = s.rank AND c.endpoint = s.endpoint
JOIN ucx_worker_address w ON w.prefix_hash = c.remote_prefix_hash
GROUP BY 1,2 HAVING sum(s.count) > 1000000 ORDER BY 1,2'

for tag in ON OFF; do
  pq="${!tag}"
  echo "=== [$tag] UCX rank matrix (edges over 1 MB) ==="; "$COMSHU" sql "$pq" "$MATRIX"
  echo "=== [$tag] ucx_tag_send totals per rank ==="
  "$COMSHU" sql "$pq" "SELECT rank, count(*) AS sends, sum(count) AS bytes FROM ucx_tag_send GROUP BY 1 ORDER BY 1"
  echo "=== [$tag] ucx_tag_send size histogram, top 12 per rank 0 ==="
  "$COMSHU" sql "$pq" "SELECT count AS msg_bytes, count(*) AS n FROM ucx_tag_send WHERE rank=0 GROUP BY 1 ORDER BY n DESC LIMIT 12"
  echo "=== [$tag] ib side (separate statements: an EMPTY stream is an ABSENT table in parquet) ==="
  for t in ib_post_send ib_poll_cq ib_post_recv ib_queue_pair ib_mem_region ib_modify_qp mlx5_device_send; do
    printf "  %-18s " "$t"; "$COMSHU" sql "$pq" "SELECT count(*) AS n FROM $t" 2>&1 | tail -1
  done
  echo "=== [$tag] how lossy is the UCX rank join ==="
  "$COMSHU" sql "$pq" "
    SELECT s.rank,
           count(*)                                                AS sends,
           count(c.endpoint)                                       AS with_ep_connection,
           count(w.prefix_hash)                                    AS with_peer_rank
    FROM ucx_tag_send s
    LEFT JOIN ucx_ep_connection c  ON c.rank = s.rank AND c.endpoint = s.endpoint
    LEFT JOIN ucx_worker_address w ON w.prefix_hash = c.remote_prefix_hash
    GROUP BY 1 ORDER BY 1"
  "$COMSHU" sql "$pq" "SELECT 'ucx_worker' t, count(*) n FROM ucx_worker" 2>&1 | tail -1
  "$COMSHU" sql "$pq" "SELECT 'ucx_worker_address' t, count(*) n FROM ucx_worker_address" 2>&1 | tail -1
  "$COMSHU" sql "$pq" "SELECT 'ucx_endpoint' t, count(*) n FROM ucx_endpoint" 2>&1 | tail -1
  "$COMSHU" sql "$pq" "SELECT 'ucx_ep_connection' t, count(*) n FROM ucx_ep_connection" 2>&1 | tail -1
  echo "=== [$tag] comm_edges ==="; "$COMSHU" sql "$pq" "SELECT * FROM comm_edges LIMIT 20" 2>&1 | tail -25
  echo "=== [$tag] mem_object: how many, and how big ==="
  "$COMSHU" sql "$pq" "SELECT rank, count(*) AS objects, sum(range_size) AS bytes FROM mem_object GROUP BY 1 ORDER BY 1"
done

# THE KNOB, side by side.  kokkos.cpp:615-641 + comm_kokkos.cpp:136-157 say the
# two configurations run the SAME swap loop over the SAME swap table, so these
# two columns must agree.  A difference is a door, not a speedup.
echo "=== KNOB: gpu/aware on vs off, message counts and bytes per rank ==="
"$COMSHU" sql "$ON" "SELECT rank, count(*) AS sends_on, sum(count) AS bytes_on FROM ucx_tag_send GROUP BY 1 ORDER BY 1"
"$COMSHU" sql "$OFF" "SELECT rank, count(*) AS sends_off, sum(count) AS bytes_off FROM ucx_tag_send GROUP BY 1 ORDER BY 1"

# ------------------------------------------------------------- 4. assertions
#
# NOT suite/sql_check.sh.  Its `params` table is built from a CLOSED column list
# (ranks, nodes, gpus, iters, msg_bytes, elems, elem_bytes, app_buffer_bytes,
# regions_per_rank, umem_binding, gpudirect, rx_depth, colls_per_iter,
# p2p_per_iter, groups_per_iter) and rejects the whole meta.json with
# "unknown workload key(s)" for anything else.  This case's oracle is a
# processor grid, a reneighbour period and a halo-size floor, none of which are
# in that vocabulary, and widening a shared suite file from an app study is not
# this study's call to make (tests/hpc/apps/README.md: do not edit outside your
# own case directory).  So the case materialises `params` from the SAME
# meta.json "workload" block and runs the SAME asserts/*.sql against the linked
# Parquet export through `comshu sql` -- which is the engine sql_check.sh uses
# for assertions anyway whenever a Parquet export exists.
run_asserts() {                     # $1 = <export>.pq
  local PQ="$1" PARAMS XFAIL fail=0 name body out rows mark
  PARAMS=$(python3 -c 'import json,sys; w=json.load(open(sys.argv[1]))["workload"]; print("WITH params AS (SELECT " + ", ".join("%d AS %s" % (int(v), k) for k, v in w.items()) + ") ")' "$CASE/meta.json")
  XFAIL=$(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1])).get("expect_fail_asserts", {})))' "$CASE/meta.json")
  echo "  -- assertions (derived from meta.json's workload block; empty result = pass)"
  for f in "$CASE"/asserts/*.sql; do
    name=$(basename "$f" .sql)
    # Two `comshu sql` warts, worked around here so the .sql files stay ordinary
    # SQL: it rejects a statement whose first token is a comment (documented in
    # tests/hpc/apps/README.md), and it wraps the text in `COPY (<query>) TO`,
    # which makes a TRAILING SEMICOLON a syntax error too.
    body=$(sed '/^[[:space:]]*--/d; /^[[:space:]]*$/d' "$f" | sed '$ s/;[[:space:]]*$//')
    out=$("$COMSHU" sql "$PQ" "$PARAMS$body" 2>&1)
    rows=$(printf '%s\n' "$out" | tail -n +2 | grep -c . || true)
    case " $XFAIL " in *" $name "*) mark="xfail" ;; *) mark="" ;; esac
    if printf '%s' "$out" | grep -qiE "query failed|^(Error|Parser Error|Binder Error|Catalog Error)"; then
      echo "  [ERR ] $name"; printf '%s\n' "$out" | head -4 | sed 's/^/         /'; fail=1
    elif [ "$rows" -eq 0 ]; then
      if [ -n "$mark" ]; then echo "  [XPASS] $name - passing while marked expect_fail; re-check the marker"
      else echo "  [ ok ] $name"; fi
    else
      if [ -n "$mark" ]; then echo "  [xfail] $name - $rows offending row(s), known:"
      else echo "  [FAIL] $name - $rows offending row(s):"; fail=1; fi
      printf '%s\n' "$out" | head -10 | sed 's/^/         /'
    fi
  done
  return $fail
}

echo "########## ASSERTIONS against gpu/aware=on ##########";  run_asserts "$ON";  echo "asserts_on_exit=$?"
echo "########## ASSERTIONS against gpu/aware=off ##########"; run_asserts "$OFF"; echo "asserts_off_exit=$?"
