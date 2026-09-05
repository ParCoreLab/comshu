// Minimal DEVX UAR probe: allocate a doorbell page, write one 8-byte value the
// way a CPU-assisted transport would, free it. Enough to exercise the host
// doorbell path's arm/guard/disarm without an NVSHMEM job.
#include <stdio.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <cuda.h>

int main(void) {
    // A CUDA context, so libcuda is loaded and the host-doorbell guard can
    // actually ask the driver whether this platform maps a BAR into a GPU.
    CUdevice dev; CUcontext cuctx;
    if (cuInit(0) == CUDA_SUCCESS && cuDeviceGet(&dev, 0) == CUDA_SUCCESS)
        { cuDevicePrimaryCtxRetain(&cuctx, dev); cuCtxSetCurrent(cuctx); }

    int n = 0;
    struct ibv_device **devs = ibv_get_device_list(&n);
    if (!devs || n == 0) { printf("probe: no verbs devices\n"); return 0; }
    struct ibv_context *ctx = NULL;
    for (int i = 0; i < n && !ctx; ++i)
        if (mlx5dv_is_supported(devs[i])) ctx = ibv_open_device(devs[i]);
    if (!ctx) { printf("probe: no mlx5 device\n"); ibv_free_device_list(devs); return 0; }

    struct mlx5dv_devx_uar *uar = mlx5dv_devx_alloc_uar(ctx, MLX5DV_UAR_ALLOC_TYPE_NC_DEDICATED);
    if (!uar) uar = mlx5dv_devx_alloc_uar(ctx, 0);
    if (!uar) { printf("probe: alloc_uar failed\n"); ibv_close_device(ctx); ibv_free_device_list(devs); return 0; }

    printf("probe: uar=%p reg_addr=%p page_id=%u\n", (void*)uar, uar->reg_addr, uar->page_id);
    mlx5dv_devx_free_uar(uar);
    ibv_close_device(ctx);
    ibv_free_device_list(devs);
    printf("probe: done\n");
    return 0;
}
