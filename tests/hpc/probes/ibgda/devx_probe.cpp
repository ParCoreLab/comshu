// devx_probe — a throwaway LD_PRELOAD shim that answers one question:
//
//   when NVSHMEM runs with IBGDA, does NVBit's instrumentation see the GPU
//   stores that build WQEs in the send queue?
//
// This is Stage 2 of PLAN-20260803-2253-ibgda-device-initiated-capture. It is a
// PROBE, not a provider: it records nothing through pcomm, links nothing from
// libpcomm, and writes a flat text file. If the answer is yes it gets deleted
// and rewritten properly as providers/mlx5dv/; if no, it gets deleted anyway.
//
// It deliberately does NOT decode DEVX command buffers. Decoding create_qp to
// find sq_base needs the vendored mlx5_ifc.h accessors, and the question does
// not require it: the SQ is *one of* the buffers NVSHMEM hands the NIC through
// mlx5dv_devx_umem_reg, and we can tell which of those live in GPU memory by
// asking CUDA. "Did an instrumented access land in any GPU-resident umem the NIC
// was given?" is a strictly weaker question than "did it land in the SQ", and a
// NO answer to it kills option B just as dead.
//
//   nvcc/g++ -shared -fPIC -o devx_probe.so devx_probe.cpp -lcuda -ldl
//   LD_PRELOAD=./devx_probe.so PCOMM_PROBE_LOG=/path/probe.$rank.txt ...
//
// Output lines (space-separated, stable for awk):
//   umem  <addr_hex> <size> <mem_type> <device_ordinal> <umem_id>
//   uar   <reg_addr_hex> <page_id>
//   devx  <opcode_first_bytes_hex>          (one per mlx5dv_devx_obj_create)
//   note  <text>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cuda.h>
#include <infiniband/mlx5dv.h>

namespace {

FILE* log_file() {
    static FILE* f = [] {
        const char* path = std::getenv("PCOMM_PROBE_LOG");
        if (!path)
            return stderr;
        // One file per rank. All eight ranks sharing one path corrupted job
        // 44156479's log into unparseable fragments ("1 uuar 0x7f613uar ..."):
        // fprintf+fflush is not atomic across processes, so records interleaved
        // mid-line. Per-rank files also make per-rank symmetry checkable, which
        // one merged file cannot express.
        const char* rank = std::getenv("OMPI_COMM_WORLD_RANK");
        if (!rank)
            rank = std::getenv("SLURM_PROCID");
        if (!rank)
            rank = std::getenv("PMI_RANK");
        char named[4096];
        std::snprintf(named, sizeof(named), "%s.%s", path, rank ? rank : "unknown");
        FILE* out = std::fopen(named, "we");
        return out ? out : stderr;
    }();
    return f;
}

// A umem CUDA does not recognise is host memory and cannot be an IBGDA send
// queue. But "does not recognise" and "could not be asked" are different
// answers, and job 44156479 could not tell them apart: every row came back
// -1 -1, which reads as "all host memory" and is equally consistent with the
// driver never having been initialised in this process yet. So the failure
// reason is now recorded rather than flattened into a sentinel.
void classify(const void* addr, int* mem_type, int* ordinal, int* err) {
    *mem_type = -1;
    *ordinal = -1;
    unsigned int type = 0;
    CUresult     rc = cuPointerGetAttribute(&type, CU_POINTER_ATTRIBUTE_MEMORY_TYPE,
                                            reinterpret_cast<CUdeviceptr>(addr));
    *err = static_cast<int>(rc);
    if (rc == CUDA_SUCCESS) {
        *mem_type = static_cast<int>(type);
    }
    int ord = -1;
    if (cuPointerGetAttribute(&ord, CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL,
                              reinterpret_cast<CUdeviceptr>(addr)) == CUDA_SUCCESS) {
        *ordinal = ord;
    }
}

template <typename Fn>
Fn next(const char* name) {
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}

}  // namespace

extern "C" mlx5dv_devx_umem* mlx5dv_devx_umem_reg(ibv_context* ctx, void* addr, size_t size,
                                                  uint32_t access) {
    static auto real = next<mlx5dv_devx_umem* (*)(ibv_context*, void*, size_t, uint32_t)>(
        "mlx5dv_devx_umem_reg");
    mlx5dv_devx_umem* out = real ? real(ctx, addr, size, access) : nullptr;
    if (out) {
        int mem_type = -1, ordinal = -1, err = 0;
        classify(addr, &mem_type, &ordinal, &err);
        std::fprintf(log_file(), "umem 0x%llx %zu %d %d %u cuerr=%d\n",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr)), size,
                     mem_type, ordinal, out->umem_id, err);
        std::fflush(log_file());
    }
    return out;
}

extern "C" mlx5dv_devx_uar* mlx5dv_devx_alloc_uar(ibv_context* ctx, uint32_t flags) {
    static auto real =
        next<mlx5dv_devx_uar* (*)(ibv_context*, uint32_t)>("mlx5dv_devx_alloc_uar");
    mlx5dv_devx_uar* out = real ? real(ctx, flags) : nullptr;
    if (out) {
        std::fprintf(log_file(), "uar 0x%llx %u\n",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(out->reg_addr)),
                     out->page_id);
        std::fflush(log_file());
    }
    return out;
}

// Not decoded — counted. A nonzero count is the proof that IBGDA took the DEVX
// path at all, which is the engagement guard the plan insists on: without it,
// every "we saw no doorbells" result is indistinguishable from a silent
// fallback to the IBRC proxy transport.
extern "C" mlx5dv_devx_obj* mlx5dv_devx_obj_create(ibv_context* ctx, const void* in, size_t inlen,
                                                   void* out, size_t outlen) {
    static auto real =
        next<mlx5dv_devx_obj* (*)(ibv_context*, const void*, size_t, void*, size_t)>(
            "mlx5dv_devx_obj_create");
    mlx5dv_devx_obj* obj = real ? real(ctx, in, inlen, out, outlen) : nullptr;
    if (obj && inlen >= 4) {
        // First 16 bits of a DEVX command buffer are the opcode, big-endian.
        const auto* bytes = static_cast<const unsigned char*>(in);
        std::fprintf(log_file(), "devx 0x%04x\n", (bytes[0] << 8) | bytes[1]);
        std::fflush(log_file());
    }
    return obj;
}
