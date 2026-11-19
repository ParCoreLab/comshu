// A stand-in libucp.so.0 for tests/ucx_rkey_abi.sh (CR-05). It exports only
// what that test reaches: ucp_get_version, the provider's other link-time
// import, and ucp_ep_rkey_unpack_internal in ONE of the shapes UCX shipped —
// seven arguments (1.19-1.22) or eight (1.23, `uct_flags` before rkey_p),
// chosen with -DFAKE_UCP_ARGS. The version it reports comes from
// FAKE_UCX_VERSION so one build can pose as 1.23.0 and 1.23.2.
//
// The function writes kFakeRkey through rkey_p, which is exactly the store that
// lands on a stray stack qword when an interposer forwards too few arguments.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" {

#define kFakeRkey (reinterpret_cast<void*>(0x5eed0000))
#define kFlags 0xF1A6ULL

void ucp_get_version(unsigned* major, unsigned* minor, unsigned* release) {
    unsigned a = 0, b = 0, c = 0;
    const char* v = getenv("FAKE_UCX_VERSION");
    if (!v || sscanf(v, "%u.%u.%u", &a, &b, &c) != 3)
        a = 0;
    *major = a, *minor = b, *release = c;
}

int ucp_mem_query(const void* memh, void* attr) {
    (void)memh, (void)attr;
    return -1;  // UCS_ERR_NO_MESSAGE-ish: never reached by the test
}

#if FAKE_UCP_ARGS == 8
int ucp_ep_rkey_unpack_internal(void* ep, const void* buf, size_t len, uint64_t unpack,
                                uint64_t skip, uint64_t sys_device, uint64_t uct_flags,
                                void** rkey_p) {
    (void)ep, (void)buf, (void)len, (void)unpack, (void)skip, (void)sys_device;
    if (uct_flags != kFlags) {
        fprintf(stderr, "fake_ucp: uct_flags=%#llx, expected %#llx\n",
                (unsigned long long)uct_flags, kFlags);
        return -3;
    }
    *rkey_p = kFakeRkey;
    return 0;
}
#elif FAKE_UCP_ARGS == 7
int ucp_ep_rkey_unpack_internal(void* ep, const void* buf, size_t len, uint64_t unpack,
                                uint64_t skip, uint64_t sys_device, void** rkey_p) {
    (void)ep, (void)buf, (void)len, (void)unpack, (void)skip, (void)sys_device;
    *rkey_p = kFakeRkey;
    return 0;
}
#else
#error "FAKE_UCP_ARGS must be 7 or 8"
#endif

}  // extern "C"
