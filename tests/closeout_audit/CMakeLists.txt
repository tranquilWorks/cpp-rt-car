find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME closeout_audit COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_closeout.py)
set_tests_properties(closeout_audit PROPERTIES TIMEOUT 60)
