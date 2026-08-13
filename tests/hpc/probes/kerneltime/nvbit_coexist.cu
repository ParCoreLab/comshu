// PROBE part 2 — can CUPTI's activity API and NVBit live in one process?
//
// This is the architectural crux, not a detail. If they cannot coexist, then
// putting kernel timing in the ENGINE via CUPTI would break the gpumem provider
// every time both are selected, and `pcomm run -p gpumem,ibverbs` is a
// supported combination.
//
// Static analysis says they should: `nm libnvbit.a` reports ZERO undefined
// cupti symbols, and the only injection string in it is CUDA_INJECTION64_PATH.
// So NVBit hooks the driver through its own injection path while CUPTI uses the
// activity/callback API. Different doors. But "different doors" is a claim
// about two closed-source hook systems sharing one driver, and that is worth
// one job rather than one argument.
//
// This tool deliberately mirrors what nvbit_backend.cu actually does at launch:
// subscribe to the CUDA event stream, resolve the kernel's mangled name, and
// take the g_mutex/skip-callback dance. It does NOT instrument instructions —
// the question is whether the CALLBACK machinery collides, and instrumenting
// would only add NVBit's ~435 s module-lifting cost to a test that is not about
// that.
//
// Build:
//   nvcc -arch=sm_90 -O2 -Xcompiler -fPIC -shared -o nvbit_coexist.so \
//        nvbit_coexist.cu -I<nvbit-1.7.7.1> -L<nvbit-1.7.7.1> -lnvbit -lcuda
// Run:
//   LD_PRELOAD=./nvbit_coexist.so ./probe

#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <cstring>

#include "nvbit.h"
#include "nvbit_tool.h"

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool            g_skip = false;
static unsigned long   g_launches = 0;

void nvbit_at_init() {
    setenv("CUDA_MANAGED_FORCE_DEVICE_ALLOC", "1", 1);
    std::printf("NVBIT init\n");
    std::fflush(stdout);
}

void nvbit_at_cuda_event(CUcontext ctx, int is_exit, nvbit_api_cuda_t cbid,
                         const char* /*name*/, void* params, CUresult*) {
    pthread_mutex_lock(&g_mutex);
    if (g_skip) {
        pthread_mutex_unlock(&g_mutex);
        return;
    }
    g_skip = true;

    CUfunction f = nullptr;
    if (!is_exit && (cbid == API_CUDA_cuLaunchKernel_ptsz || cbid == API_CUDA_cuLaunchKernel))
        f = ((cuLaunchKernel_params*)params)->f;
    else if (!is_exit && cbid == API_CUDA_cuLaunchKernelEx)
        f = ((cuLaunchKernelEx_params*)params)->f;

    if (f) {
        // The same call the backend makes to resolve a name for filtering —
        // this is the work a launch-time host hook actually does.
        const char* n = nvbit_get_func_name(ctx, f, /*mangled=*/true);
        (void)n;
        g_launches++;
    }

    g_skip = false;
    pthread_mutex_unlock(&g_mutex);
}

void nvbit_at_term() {
    std::printf("NVBIT launches_seen=%lu\n", g_launches);
    std::fflush(stdout);
}
