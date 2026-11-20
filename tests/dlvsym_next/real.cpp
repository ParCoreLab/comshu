// The library RTLD_NEXT should find: vsym_probe in two versions, so a wrong
// version is distinguishable from a wrong library.
extern "C" int vsym_probe_v1() { return 101; }
extern "C" int vsym_probe_v2() { return 102; }
__asm__(".symver vsym_probe_v1,vsym_probe@V1");
__asm__(".symver vsym_probe_v2,vsym_probe@@V2");

// ONLY a compat (non-default) version: no default exists for a lookup that
// walks default versions to find, which is how the old dlvsym missed it.
extern "C" int vsym_compat_v1() { return 201; }
__asm__(".symver vsym_compat_v1,vsym_compat@V1");
