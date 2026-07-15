# libibverbs_trace.so — proof-of-concept network provider, ported from
# SigmaTrace/Ibtrace. Validates the hook engine (pcomm::hook) against the new
# record model. Requires libibverbs headers.

add_library(ibverbs_trace SHARED
  ibverbs_provider.cpp
)
set_target_properties(ibverbs_trace PROPERTIES PREFIX "lib" OUTPUT_NAME "ibverbs_trace")
target_link_libraries(ibverbs_trace PRIVATE pcomm::pcomm)
