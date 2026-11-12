// Does libcuda keep the pointers it hands out in its own loaded segments?
// Count words equal to a few driver entry points in libcuda's PT_LOAD
// segments: right after dlopen, after cuInit, and after cudart (static) init.
// Also report where each cuGetExportTable-ish pointer cudart got lives.
#include <cuda.h>
#include <cuda_runtime.h>
#include <dlfcn.h>
#include <link.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

struct Seg { uintptr_t lo, hi; int flags; };
static std::vector<Seg> segs;
static std::string cudapath;

static int cb(struct dl_phdr_info* i, size_t, void*) {
  if (!i->dlpi_name || !strstr(i->dlpi_name, "libcuda.so")) return 0;
  cudapath = i->dlpi_name;
  for (int k = 0; k < i->dlpi_phnum; ++k) {
    auto& p = i->dlpi_phdr[k];
    if (p.p_type == PT_LOAD && !(p.p_flags & PF_X))
      segs.push_back({i->dlpi_addr + p.p_vaddr, i->dlpi_addr + p.p_vaddr + p.p_memsz, (int)p.p_flags});
    if (p.p_type == PT_GNU_RELRO)
      printf("  relro %#lx..%#lx\n", i->dlpi_addr + p.p_vaddr, i->dlpi_addr + p.p_vaddr + p.p_memsz);
  }
  return 0;
}

static const char* names[] = {"cuMemGetHandleForAddressRange", "cuGetProcAddress_v2",
  "cuMemHostRegister_v2", "cuMemHostGetDevicePointer_v2", "cuMemAlloc_v2"};

static void scan(const char* when, void* lib) {
  segs.clear(); dl_iterate_phdr(cb, nullptr);
  size_t total = 0; for (auto& s : segs) total += s.hi - s.lo;
  printf("[%s] %s non-exec segs=%zu bytes=%zu\n", when, cudapath.c_str(), segs.size(), total);
  for (auto n : names) {
    void* f = dlsym(lib, n);
    int hits = 0;
    for (auto& s : segs)
      for (uintptr_t a = (s.lo + 7) & ~7ul; a + 8 <= s.hi; a += 8)
        if (*(void**)a == f) { ++hits; if (hits <= 6) printf("    %s hit @%#lx (seg flags %d, off %#lx)\n", n, a, 
              [&]{for(auto&t:segs) if(a>=t.lo&&a<t.hi) return t.flags; return -1;}(), a - segs[0].lo); }
    printf("  %-32s %p hits=%d\n", n, f, hits);
  }
}

int main() {
  void* lib = dlopen("libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!lib) { printf("no libcuda\n"); return 1; }
  scan("after dlopen", lib);
  auto cuInit_ = (CUresult(*)(unsigned))dlsym(lib, "cuInit");
  printf("cuInit=%d\n", cuInit_(0));
  scan("after cuInit", lib);
  void* p; cudaMalloc(&p, 1 << 20); cudaFree(p);
  scan("after cudart init", lib);
  void* fn = nullptr; cudaDriverEntryPointQueryResult q;
  cudaGetDriverEntryPointByVersion("cuMemGetHandleForAddressRange", &fn, 12000, cudaEnableDefault, &q);
  printf("cudart entry point for cuMemGetHandleForAddressRange = %p (dlsym %p)\n", fn, dlsym(lib, "cuMemGetHandleForAddressRange"));
  return 0;
}
