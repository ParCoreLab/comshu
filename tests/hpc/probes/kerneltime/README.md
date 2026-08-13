# probes/kerneltime — how should pcomm capture kernel lifetimes?

Not a suite case. It answered one architectural question and is kept as the
record behind `plans/PLAN-20260813-0600-kernel-lifetimes-belong-to-the-engine`.

## The question

pcomm captures no kernel record at all. SigmaTrace does (`CudaKernelLaunch =
300` / `CudaKernelExit`). A comm-only timeline can show that a rank went quiet
and can never say whether it was computing or blocked, so the gap is worth
closing — but *where* it closes (engine or provider) depends on facts about
CUPTI, NVBit and three clocks that nobody had measured.

## The parts

| file | question | headline answer |
|---|---|---|
| `probe.cu` | Does CUPTI work unprivileged? What clock is it on? How far is submission from execution? | works; **CUPTI timebase IS `CLOCK_REALTIME`**; async skew **9–13 ms** vs 11–20 µs kernels |
| `nvbit_coexist.cu` | Can CUPTI and NVBit share a process? | **No.** NVBit dlopens libcupti and takes the one subscriber slot |
| `events_probe.cu` | Can `cudaEventRecord` replace CUPTI? | **No.** 48 % overhead *and* a +3.7 µs systematic bias |
| `globaltimer_probe.cu` | Is `%globaltimer` a usable fallback clock? | Yes, 1.1 % / 131 ns per kernel — but **drifts per GPU** (−13.9 ppm H100, +5.7 ppm A100), so it needs its own anchored domain |
| `cupti_shim.cpp` | What does it cost on a real workload? | preloadable prototype of the engine feature |
| `job.sbatch` | ...at 8 ranks over IB, on dedicated nodes | **+0.65 %**, which is below the run's own ~±5 % noise |

## Running them

The microbenchmarks need no allocation — MN5 login nodes have H100s and CUDA
runs there directly, which is why these iterate in seconds rather than queue
waits. Only `job.sbatch` needs nodes.

```bash
module load cuda/12.6
nvcc -arch=sm_90 -O2 -o probe probe.cu \
     -I$CUDA_HOME/extras/CUPTI/include -L$CUDA_HOME/extras/CUPTI/lib64 -lcupti
./probe 2000

# coexistence — needs the nvbit tree beside it
nvcc -arch=sm_90 -O2 -Xcompiler -fPIC -shared -o nvbit_coexist.so nvbit_coexist.cu \
     -I<nvbit-1.7.7.1> -L<nvbit-1.7.7.1> -lnvbit -lcuda
LD_PRELOAD=./nvbit_coexist.so ./probe 500      # -> MULTIPLE_SUBSCRIBERS

sbatch job.sbatch                               # the four-arm cost measurement
```

On du04 (A100, CUDA 13.0) use `-arch=sm_80` and
`/usr/local/cuda/targets/x86_64-linux/{include,lib}` for CUPTI.

## What was built from this

`plans/PLAN-20260813-0600` shipped both sources. Verified captures:

| source | how | result |
|---|---|---|
| CUPTI | `pcomm run -p cupti` | 1402 rows, 0 dropped, domain 1 fits at **1.0000000** ns/tick — no calibration needed |
| NVBit globaltimer | gpumem preloaded, no CUPTI in the process | **502 spans, 0 incomplete**, domain 2, fits at **−7.6 ppm** |

The globaltimer path needed one non-obvious fix. Its domain first fitted at
**+111.7 ppm** — 8x the drift this probe measures on a clean run — because a
two-point fit converts anchor *offset* error into apparent *rate* error, and the
first `sample_gpu_anchor` allocated inside its own host bracket. Tightening the
bracket to 7.2-7.7 us (best of 8, allocation hoisted) brought the fit to −7.6
ppm. `globaltimer_probe`'s `host_window` output is the number to watch when this
recurs.

## How to read the coexistence result — it is order-dependent and asymmetric

```
LD_PRELOAD=nvbit:shim   -> CUPTI wins, NVBit dies CUDA_ERROR_ALREADY_ACQUIRED, exit 1
LD_PRELOAD=shim:nvbit   -> CUPTI soft-fails MULTIPLE_SUBSCRIBERS, everything works, exit 0
```

**Only one of those is survivable, and the dangerous one fails silently.** Job
44546506 arm E ran the real `libgpumem_trace.so` with the shim in the chain: it
created four rank directories and wrote **zero `.pcs` files**. Not a crash — an
empty capture. Any implementation must be tested by running gpumem and counting
output files, not by reading the code.
