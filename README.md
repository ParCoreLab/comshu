# pcomm

pcomm records how a GPU program communicates. It captures InfiniBand traffic,
GPU-initiated InfiniBand, UCX, NCCL collectives, CUDA kernels and NVLink peer
load/store from a running workload, then reads the capture back as tables you
can export, query, or open in a browser.

## Install

pcomm carries its own spack package repository, so spack needs no fork:

```bash
spack repo add https://github.com/ParCoreLab/pcomm.git
spack install pcomm +ibverbs +ucx
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

`+nccl` builds against NCCL, which asks for a GPU target:

```bash
spack install pcomm +nccl ^nccl cuda_arch=80
```

Two things are not in the package, because each needs a second toolchain:

- The NVLink provider (`gpumem`), which needs NVBit and its own CUDA build tree.
- The web UI (`pcomm ui`), which embeds a bundle built from `ui/` with `pnpm`.

Build from source if you want either.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

The core and its tests build on a host with no CUDA and no verbs. Providers are
turned on one at a time and need their vendor SDK:

```bash
cmake -B build -DPCOMM_PROVIDER_IBVERBS=ON -DPCOMM_PROVIDER_UCX=ON
```

`cmake --install build` writes a tree that locates its own libraries, so the
installed `pcomm` needs no environment variables.

## Usage

Capture a run. Providers are picked by name, and most of them share one pass:

```bash
pcomm list                              # what this build can capture
pcomm run -p ib      -- ./app args      # InfiniBand traffic
pcomm run -p gpumem  -- ./app args      # NVLink peer load/store
pcomm run -p ib,nccl -- ./app args      # both, in one run
```

Read the capture back:

```bash
pcomm read pcomm-out                    # what is in it
pcomm export pcomm-out --csv out.csv    # tables as CSV
pcomm export pcomm-out --parquet out    # tables as Parquet
pcomm sql out "select count(*) from ib_post_send"
```

Visualize it:

```bash
pcomm ui pcomm-out                      # panels at 127.0.0.1:8777
```

`sql` needs a build with DuckDB, which is the `+duckdb` variant. `ui` needs the
frontend bundle as well, so it is only in a source build. When it is built, the
bundle is compiled into the binary, and `pcomm ui` serves it with no other files
needed.

For the layout, the gates, and how to add a record type or a provider, see
[`docs/development.md`](docs/development.md).
