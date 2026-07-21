add_executable(pcomm_read main.cpp)
set_target_properties(pcomm_read PROPERTIES OUTPUT_NAME "pcomm-read")
# The reader decodes every provider's record schema; ib_records.hpp is
# vendor-neutral (no verbs.h), so this include adds no ibverbs dependency.
target_include_directories(pcomm_read PRIVATE ${CMAKE_SOURCE_DIR}/providers/ibverbs)
target_link_libraries(pcomm_read PRIVATE pcomm::pcomm)
