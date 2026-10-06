find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME host_adapter_host_profiles
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_profile.py")
set_tests_properties(host_adapter_host_profiles PROPERTIES TIMEOUT 60)
