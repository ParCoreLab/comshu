// Calls ucp_ep_rkey_unpack_internal by name, through the PLT, so an interposer
// preloaded ahead of libucp is what answers — the path a real libucp's own
// internal callers take. argv[1] is the argument count the loaded libucp takes;
// the call always passes eight (the eighth is ignored by a seven-argument
// callee), with rkey_p in the slot that library reads.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" int ucp_ep_rkey_unpack_internal(void*, const void*, size_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t);

int main(int argc, char** argv) {
    const int nargs = argc > 1 ? atoi(argv[1]) : 0;
    void*     rkey = nullptr;
    // A decoy in the slot the wrong layout would read: if an interposer
    // forwarded too few arguments, libucp's store goes wherever this points.
    void* decoy = nullptr;
    char  buf[16] = {0};
    int   rc;
    if (nargs == 8)
        rc = ucp_ep_rkey_unpack_internal(reinterpret_cast<void*>(0xe9), buf, sizeof buf, 1, 0, 0, 0xF1A6ULL,
                                         (uint64_t)(uintptr_t)&rkey);
    else if (nargs == 7)
        rc = ucp_ep_rkey_unpack_internal(reinterpret_cast<void*>(0xe9), buf, sizeof buf, 1, 0, 0,
                                         (uint64_t)(uintptr_t)&rkey,
                                         (uint64_t)(uintptr_t)&decoy);
    else {
        fprintf(stderr, "usage: driver 7|8\n");
        return 2;
    }
    if (rc != 0 || rkey != reinterpret_cast<void*>(0x5eed0000) || decoy != nullptr) {
        fprintf(stderr, "driver: rc=%d rkey=%p decoy=%p — the call did not reach libucp intact\n",
                rc, rkey, decoy);
        return 1;
    }
    printf("unpack ok rkey=%p\n", rkey);
    return 0;
}
