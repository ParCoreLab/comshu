// CR-10: dlvsym(RTLD_NEXT, ...) through the engine resolves relative to the
// CALLER and honours the requested version. Run with the engine first in
// LD_PRELOAD, so the load order is exe, engine, libdlvsym_caller,
// libdlvsym_real — where resolving relative to the engine finds the caller's own
// vsym_probe@V1 (returns 1) instead of the next library's (101).
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

extern "C" void* caller_next(const char* version);

int main() {
    // The engine must actually be the dlvsym in front, or this proves nothing.
    Dl_info info{};
    void*   front = dlsym(RTLD_DEFAULT, "dlvsym");
    if (!front || !dladdr(front, &info) || !info.dli_fname ||
        !std::strstr(info.dli_fname, "comshu_engine")) {
        std::fprintf(stderr, "dlvsym_next: the engine is not interposing dlvsym (front=%s)\n",
                     info.dli_fname ? info.dli_fname : "?");
        return 1;
    }
    int fail = 0;
    const struct { const char* version; int want; } cases[] = {{"V1", 101}, {"V2", 102}};
    for (const auto& c : cases) {
        auto* fn = reinterpret_cast<int (*)()>(caller_next(c.version));
        const int got = fn ? fn() : -1;
        if (got != c.want) {
            std::fprintf(stderr, "dlvsym_next: RTLD_NEXT vsym_probe@%s returned %d, want %d%s\n",
                         c.version, got, c.want,
                         got == 1 ? " (the caller's OWN definition)" : "");
            fail = 1;
        }
    }
    if (!fail)
        std::puts("dlvsym_next: ok");
    return fail;
}
