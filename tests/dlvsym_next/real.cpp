// The library RTLD_NEXT should find: vsym_probe in two versions, so a wrong
// version is distinguishable from a wrong library.
extern "C" int vsym_probe_v1() { return 101; }
extern "C" int vsym_probe_v2() { return 102; }
__asm__(".symver vsym_probe_v1,vsym_probe@V1");
__asm__(".symver vsym_probe_v2,vsym_probe@@V2");
