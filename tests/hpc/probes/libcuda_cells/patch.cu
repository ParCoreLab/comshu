// Overwrite libcuda's one cell for cuMemGetHandleForAddressRange right after
// dlopen (before cuInit, before cudart) and check every route returns ours.
#include <cuda.h>
#include <cuda_runtime.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

static void* real_fn; static int calls;
extern "C" CUresult mine(void* h, CUdeviceptr p, size_t s, CUmemRangeHandleType t, unsigned long long f) {
  ++calls; return ((CUresult(*)(void*, CUdeviceptr, size_t, CUmemRangeHandleType, unsigned long long))real_fn)(h, p, s, t, f);
}
static int cb(struct dl_phdr_info* i, size_t, void*) {
  if (!i->dlpi_name || !strstr(i->dlpi_name, "libcuda.so")) return 0;
  for (int k = 0; k < i->dlpi_phnum; ++k) {
    auto& p = i->dlpi_phdr[k];
    if (p.p_type != PT_LOAD || (p.p_flags & PF_X)) continue;
    uintptr_t lo = (i->dlpi_addr + p.p_vaddr + 7) & ~7ul, hi = i->dlpi_addr + p.p_vaddr + p.p_memsz;
    for (uintptr_t a = lo; a + 8 <= hi; a += 8)
      if (*(void**)a == real_fn) {
        uintptr_t pg = a & ~0xffful;
        int rc = mprotect((void*)pg, 4096, PROT_READ | PROT_WRITE);
        *(void**)a = (void*)&mine;
        printf("patched cell %#lx (mprotect rc=%d)\n", a, rc);
      }
  }
  return 1;
}
int main() {
  void* lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
  real_fn = dlsym(lib, "cuMemGetHandleForAddressRange");
  dl_iterate_phdr(cb, nullptr);
  void* fn = nullptr; cudaDriverEntryPointQueryResult q;
  cudaGetDriverEntryPointByVersion("cuMemGetHandleForAddressRange", &fn, 12000, cudaEnableDefault, &q);
  printf("cudart entry point = %p  mine=%p real=%p\n", fn, (void*)&mine, real_fn);
  void* fn2 = nullptr; CUdriverProcAddressQueryResult q2;
  auto gpa = (CUresult(*)(const char*, void**, int, cuuint64_t, CUdriverProcAddressQueryResult*))dlsym(lib, "cuGetProcAddress_v2");
  gpa("cuMemGetHandleForAddressRange", &fn2, 12000, 0, &q2);
  printf("cuGetProcAddress_v2 = %p\n", fn2);
  // real call through cudart's pointer: dmabuf export of a cudaMalloc buffer
  void* d; cudaMalloc(&d, 2 << 20);
  int fd = -1;
  auto r = ((CUresult(*)(void*, CUdeviceptr, size_t, CUmemRangeHandleType, unsigned long long))fn)(&fd, (CUdeviceptr)d, 2 << 20, CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
  printf("call rc=%d fd=%d calls_through_mine=%d\n", r, fd, calls);
  return 0;
}
