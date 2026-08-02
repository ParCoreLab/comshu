add_executable(pcomm_read main.cpp)
set_target_properties(pcomm_read PROPERTIES OUTPUT_NAME "pcomm-read")
# The reader decodes every provider's record schema; ib_records.hpp is
# vendor-neutral (no verbs.h), so this include adds no ibverbs dependency.
target_include_directories(pcomm_read PRIVATE ${CMAKE_SOURCE_DIR}/providers/ibverbs)
target_link_libraries(pcomm_read PRIVATE pcomm::pcomm)

# Header-only against libpcomm, so it carries no pcomm .so dependency — but it
# is still C++ and needs the toolchain libstdc++ it was built with, which is
# what PCOMM_INSTALL_RPATH_EXTRA supplies to an installed copy.
set_target_properties(pcomm_read PROPERTIES
  INSTALL_RPATH "${PCOMM_INSTALL_RPATH_EXTRA}")
target_link_options(pcomm_read PRIVATE -Wl,--enable-new-dtags)
if(PCOMM_INSTALL_CORE)
  install(TARGETS pcomm_read RUNTIME DESTINATION bin COMPONENT core)
endif()
