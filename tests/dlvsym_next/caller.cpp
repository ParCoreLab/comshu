// An interposer: it defines vsym_probe@V1 itself and asks RTLD_NEXT for the
// real one, the pattern every LD_PRELOAD wrapper uses. Its own definition is
// what a resolution relative to the wrong object hands back.
#include <dlfcn.h>

extern "C" int vsym_probe_own() { return 1; }
__asm__(".symver vsym_probe_own,vsym_probe@@V1");

// NOT a tail call: RTLD_NEXT is relative to the return address, and `return
// dlvsym(...)` compiles to a jump that leaves main's address there instead.
extern "C" void* caller_next(const char* version) {
    void* r = dlvsym(RTLD_NEXT, "vsym_probe", version);
    __asm__ volatile("" : "+r"(r));
    return r;
}
