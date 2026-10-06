find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME host_adapter_closeout_overlay
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_overlay.py")
set_tests_properties(host_adapter_closeout_overlay PROPERTIES TIMEOUT 30)
