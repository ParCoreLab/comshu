# Comshu

Comshu in Turkish means neighbor. With communication tools, under one roof, 
we can observe what GPU neighbors are saying to each other. 

comshu records how a GPU program communicates. It captures InfiniBand traffic,
GPU-initiated InfiniBand, UCX, NCCL collectives, CUDA kernels and NVLink peer
load/store from a running workload, then reads the capture back as tables you
can export, query, or open in a browser.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/comshu-logo-dark.png">
    <img src="assets/comshu-logo.png" alt="Comshu" width="420">
  </picture>
</p>

## Install

comshu carries its own spack package repository, so spack needs no fork:

```bash
spack repo add https://github.com/ParCoreLab/comshu.git
spack install comshu +ibverbs +ucx
```

Every capture provider is a variant, and all of them are off by default:

| Variant | Captures |
|---|---|
| `+ibverbs` | InfiniBand verbs, host side |
| `+mlx5dv` | GPU-initiated InfiniBand (IBGDA) |
| `+ucx` | UCX, both the application and the transport layer |
| `+nccl` | NCCL collectives |
| `+cupti` | CUDA kernels and memory copies |
| `+duckdb` | Parquet export and SQL over a capture |
| `+ui` | The panels in a browser, with `comshu ui` |

`+nccl` builds against NCCL, which asks for a GPU target:

```bash
spack install comshu +nccl ^nccl cuda_arch=80
```

The NVLink provider (`gpumem`) is not in the package. It needs NVBit and its own
CUDA build tree, so build that one from source.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

The core and its tests build on a host with no CUDA and no verbs. Providers are
turned on one at a time and need their vendor SDK:

```bash
cmake -B build -DCOMSHU_PROVIDER_IBVERBS=ON -DCOMSHU_PROVIDER_UCX=ON
```

`cmake --install build` writes a tree that locates its own libraries, so the
installed `comshu` needs no environment variables.

## Usage

Capture a run. Providers are picked by name, and most of them share one pass:

```bash
comshu list                              # what this build can capture
comshu run -p ib      -- ./app args      # InfiniBand traffic
comshu run -p gpumem  -- ./app args      # NVLink peer load/store
comshu run -p ib,nccl -- ./app args      # both, in one run
```

Read the capture back:

```bash
comshu read comshu-out                    # what is in it
comshu export comshu-out --csv out.csv    # tables as CSV
comshu export comshu-out --parquet out    # tables as Parquet
comshu sql out "select count(*) from ib_post_send"
```

Visualize it:

```bash
comshu ui comshu-out                      # panels at 127.0.0.1:8777
```

`sql` needs `+duckdb`, and `ui` needs `+ui`, which turns on `+duckdb` too. The
page is compiled into the binary, so `comshu ui` needs no other files. This is
the usual way to read a cluster capture: record it there, copy the directory
home, open it here.

For the layout, the gates, and how to add a record type or a provider, see
[`docs/development.md`](docs/development.md).

Related Papers:

Comshu brings together our communication profiling tools published in the following papers:

- **Snoopie** — instrumentation-based multi-GPU communication profiler built on NVBit,
  tracking P2P transfers and GPU-centric communication library calls (NCCL, NVSHMEM)
  with source-line and data-object attribution.
  📄 [Snoopie: A Multi-GPU Communication Profiler and Visualizer (ICS '24)](https://dl.acm.org/doi/10.1145/3650200.3656597)
  💻 [github.com/ParCoreLab/snoopie](https://github.com/ParCoreLab/snoopie)

- **ucTrace** — multi-layer profiler for UCX-driven communication, linking MPI calls to
  transport-level events across host, GPU, and NIC; works with GPU-aware MPI.
  📄 [ucTrace: A Multi-Layer Profiling Tool for UCX-driven Communication (IPDPS '26)](https://arxiv.org/abs/2602.19084)
  💻 [github.com/ParCoreLab/ucTrace](https://github.com/ParCoreLab/ucTrace)

- **SigmaTrace** — library-agnostic RDMA profiling framework with fine-grained,
  per-operation visibility for CPU- and GPU-initiated networking
  📄 [SigmaTrace: Library-Agnostic Observability for CPU/GPU-Initiated RDMA Communication (SC '26)](https://arxiv.org/abs/2610.24328)
  💻 [github.com/ParCoreLab/SigmaTrace](https://github.com/ParCoreLab/SigmaTrace)
