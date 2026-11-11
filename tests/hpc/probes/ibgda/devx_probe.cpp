// devx_probe — a throwaway LD_PRELOAD shim that answers one question:
//
//   when NVSHMEM runs with IBGDA, does NVBit's instrumentation see the GPU
//   stores that build WQEs in the send queue?
//
// This is Stage 2 of PLAN-20260803-2253-ibgda-device-initiated-capture. It is a
// PROBE, not a provider: it records nothing through comshu, links nothing from
// libcomshu, and writes a flat text file. If the answer is yes it gets deleted
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
//   LD_PRELOAD=./devx_probe.so COMSHU_PROBE_LOG=/path/probe.$rank.txt ...
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
#include <initializer_list>

#include <cuda.h>
#include <infiniband/mlx5dv.h>

namespace {

FILE* log_file() {
    static FILE* f = [] {
        const char* path = std::getenv("COMSHU_PROBE_LOG");
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

// RTLD_NEXT alone is not enough, and getting this wrong does not produce a bad
// trace — it breaks the application. On du04 every mlx5dv_* lookup failed, the
// interposers dutifully returned nullptr, and NVSHMEM read that as "UAR
// allocation failed" and disabled the entire IBGDA transport:
//
//   ibgda.cpp:1830: NULL value cannot allocate mlx5dv_devx_uar
//   WARN: GPU cannot map UAR of device mlx5_0. Skipping...
//
// The same workload without the shim runs fine, so the probe was the defect.
// Cause: mlx5dv_* is exported by libmlx5.so.1, which rdma-core dlopens as a
// provider plugin, and dlopen'd objects are not in the RTLD_NEXT chain.
//
// So: fall back to the provider libraries by name, and if the real function
// still cannot be found, ABORT. Never hand the application a failure it did not
// cause — a profiler that silently degrades what it observes is worse than one
// that refuses to start.
template<typename Fn> Fn next(const char* name) {
    void* p = dlsym(RTLD_NEXT, name);
    if (!p) {
        for (const char* so : {"libmlx5.so.1", "libibverbs.so.1"}) {
            if (void* h = dlopen(so, RTLD_LAZY | RTLD_NOLOAD)) {
                p = dlsym(h, name);
                if (p)
                    break;
            }
        }
    }
    if (!p) {
        std::fprintf(stderr,
                     "devx_probe: FATAL cannot resolve real %s (RTLD_NEXT and "
                     "libmlx5/libibverbs both failed). Aborting rather than returning a "
                     "failure the application did not cause.\n",
                     name);
        std::abort();
    }
    return reinterpret_cast<Fn>(p);
}

}  // namespace

extern "C" mlx5dv_devx_umem* mlx5dv_devx_umem_reg(ibv_context* ctx, void* addr, size_t size,
                                                  uint32_t access) {
    static auto real =
        next<mlx5dv_devx_umem* (*)(ibv_context*, void*, size_t, uint32_t)>("mlx5dv_devx_umem_reg");
    mlx5dv_devx_umem* out = real(ctx, addr, size, access);
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

// The one NVSHMEM's IBGDA actually calls. `nm -D --undefined-only` on
// nvshmem_transport_ibgda.so.3.0.0 lists BOTH umem_reg and umem_reg_ex, and the
// first run here logged zero umem lines while creating six DEVX queue pairs —
// every IBGDA registration goes through the _ex variant, which the legacy hook
// cannot see.
//
// Both are hooked, and each tags which entry point fired, deliberately. In
// rdma-core the legacy mlx5dv_devx_umem_reg is implemented on top of _ex, so
// hooking both may double-count — that is exactly what happened with
// ibv_reg_mr over ibv_reg_mr_iova2 (ib_hooks.hpp:26-42, case 01 went from one
// region per rank to two identical rows). The tag lets this run MEASURE whether
// the chaining happens on this rdma-core rather than assume it either way; the
// provider can then hook only the funnel, with evidence.
// ---------------------------------------------------------------------------
// Driver-API entry points. LOG ONLY — nothing here replaces a function pointer.
//
// Hooking cuMemGetHandleForAddressRange by LD_PRELOAD produced zero hits, which
// means NVSHMEM does not call it as a dynamic symbol. CUDA hands out entry
// points through cuGetProcAddress and cuGetExportTable, and SigmaTrace patches
// both (ExportTablePreload.cpp:90-147: walk the table to a 0xffff…ffff
// sentinel, mprotect, overwrite in place). That is the most invasive code in
// the reference implementation and it is not what this probe is for.
//
// So this only *observes* the requests. If cuMemGetHandleForAddressRange shows
// up here, the route is confirmed and the export-table subsystem is justified
// on evidence. If it does not, patching cuGetProcAddress would not have helped
// either, and the next suspect is the export table proper.
//
// Deliberately not chained into the returned pfn: replacing a driver entry
// point is exactly the class of change that broke the transport an hour ago,
// and there is no reason to risk it to answer a question about routing.
extern "C" CUresult cuGetProcAddress_v2(const char* symbol, void** pfn, int cudaVersion,
                                        cuuint64_t                      flags,
                                        CUdriverProcAddressQueryResult* symbolStatus) {
    static auto real =
        next<CUresult (*)(const char*, void**, int, cuuint64_t, CUdriverProcAddressQueryResult*)>(
            "cuGetProcAddress_v2");
    CUresult rc = real(symbol, pfn, cudaVersion, flags, symbolStatus);
    if (symbol) {
        std::fprintf(log_file(), "cuproc %s rc=%d\n", symbol, static_cast<int>(rc));
        std::fflush(log_file());
    }
    return rc;
}

extern "C" CUresult cuGetExportTable(const void** ppExportTable, const CUuuid* pExportTableId) {
    static auto real = next<CUresult (*)(const void**, const CUuuid*)>("cuGetExportTable");
    CUresult    rc = real(ppExportTable, pExportTableId);
    std::fprintf(log_file(), "cuexport rc=%d\n", static_cast<int>(rc));
    std::fflush(log_file());
    return rc;
}

// The missing link. IBGDA registers its NIC buffers by dmabuf fd, not by
// address: every umem_ex on du04 came back `addr=0x0 dmabuf=85`, so there is no
// pointer for cuPointerGetAttribute to classify and the GPU-residency test
// cannot work on the umem alone.
//
// The fd is minted here, from a GPU virtual address range, so this is where the
// range → fd mapping exists. Recording it turns `dmabuf=85` back into a
// [base, base+size) in GPU memory, which is what the gpu_access join needs.
//
// If NVSHMEM reaches this through cuGetProcAddress rather than the dynamic
// symbol, LD_PRELOAD will not see it and nothing is logged — that is the
// SigmaTrace export-table problem, and the absence of `dmabuf_range` lines is
// how this run reports it rather than guessing.
extern "C" CUresult cuMemGetHandleForAddressRange(void* handle, CUdeviceptr dptr, size_t size,
                                                  CUmemRangeHandleType handleType,
                                                  unsigned long long   flags) {
    static auto real =
        next<CUresult (*)(void*, CUdeviceptr, size_t, CUmemRangeHandleType, unsigned long long)>(
            "cuMemGetHandleForAddressRange");
    CUresult rc = real(handle, dptr, size, handleType, flags);
    if (rc == CUDA_SUCCESS && handle) {
        std::fprintf(log_file(), "dmabuf_range 0x%llx %zu fd=%d type=%d\n",
                     static_cast<unsigned long long>(dptr), size, *static_cast<const int*>(handle),
                     static_cast<int>(handleType));
        std::fflush(log_file());
    }
    return rc;
}

extern "C" mlx5dv_devx_umem* mlx5dv_devx_umem_reg_ex(ibv_context*         ctx,
                                                     mlx5dv_devx_umem_in* umem_in) {
    static auto real =
        next<mlx5dv_devx_umem* (*)(ibv_context*, mlx5dv_devx_umem_in*)>("mlx5dv_devx_umem_reg_ex");
    mlx5dv_devx_umem* out = real(ctx, umem_in);
    if (out && umem_in) {
        int mem_type = -1, ordinal = -1, err = 0;
        classify(umem_in->addr, &mem_type, &ordinal, &err);
        // dmabuf_fd is only meaningful when comp_mask advertises it. Reading it
        // unconditionally is how the first du04 run reported "dmabuf=85" — which
        // may have been an uninitialised field, and a conclusion was drawn from
        // it. comp_mask is logged so that claim can be checked rather than
        // trusted.
        std::fprintf(log_file(),
                     "umem_ex 0x%llx %zu %d %d %u cuerr=%d comp_mask=0x%llx dmabuf=%d acc=0x%x\n",
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(umem_in->addr)),
                     umem_in->size, mem_type, ordinal, out->umem_id, err,
                     static_cast<unsigned long long>(umem_in->comp_mask), umem_in->dmabuf_fd,
                     umem_in->access);
        std::fflush(log_file());
    }
    return out;
}

extern "C" mlx5dv_devx_uar* mlx5dv_devx_alloc_uar(ibv_context* ctx, uint32_t flags) {
    static auto real = next<mlx5dv_devx_uar* (*)(ibv_context*, uint32_t)>("mlx5dv_devx_alloc_uar");
    mlx5dv_devx_uar* out = real(ctx, flags);
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
    static auto real = next<mlx5dv_devx_obj* (*)(ibv_context*, const void*, size_t, void*, size_t)>(
        "mlx5dv_devx_obj_create");
    mlx5dv_devx_obj* obj = real(ctx, in, inlen, out, outlen);
    if (obj && inlen >= 4) {
        // First 16 bits of a DEVX command buffer are the opcode, big-endian.
        const auto* bytes = static_cast<const unsigned char*>(in);
        std::fprintf(log_file(), "devx 0x%04x\n", (bytes[0] << 8) | bytes[1]);
        std::fflush(log_file());
    }
    return obj;
}
