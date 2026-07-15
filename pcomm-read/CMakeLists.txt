add_executable(pcomm_read main.cpp)
set_target_properties(pcomm_read PROPERTIES OUTPUT_NAME "pcomm-read")
target_link_libraries(pcomm_read PRIVATE pcomm::pcomm)
