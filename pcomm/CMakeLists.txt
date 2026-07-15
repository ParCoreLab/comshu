add_executable(pcomm_launcher main.cpp)
set_target_properties(pcomm_launcher PROPERTIES OUTPUT_NAME "pcomm")
target_link_libraries(pcomm_launcher PRIVATE pcomm::pcomm)
