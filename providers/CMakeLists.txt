if(PCOMM_PROVIDER_GPUMEM)
  add_subdirectory(gpumem)
endif()
if(PCOMM_PROVIDER_IBVERBS)
  add_subdirectory(ibverbs)
endif()
if(PCOMM_PROVIDER_NCCL)
  add_subdirectory(nccl)
endif()
if(PCOMM_PROVIDER_MLX5DV)
  add_subdirectory(mlx5dv)
endif()
if(PCOMM_PROVIDER_UCX)
  add_subdirectory(ucx)
  # Deliberately NOT its own option. ucx_deep needs exactly the UCX tree the ucx
  # provider needs, and a separate switch would create one more configuration
  # that no gate builds — which is how the DuckDB reader stopped compiling for
  # 39 commits (bea74d8) and how tests/ stopped compiling before that. One
  # switch, both providers, one gate.
  add_subdirectory(ucx_deep)
endif()
