# ####################################################################################################################################################
# tests/ci/CMakeLists.txt
#
# CI gate for the shared library runtime dependency closure.
#
# ci.runtime_dependencies -- all shared libraries required by libamio resolve on the test host.
# ####################################################################################################################################################

add_test(NAME ci.runtime_dependencies COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/check_runtime_dependencies.sh $<TARGET_FILE:amio_core>)
set_tests_properties(ci.runtime_dependencies PROPERTIES LABELS "ci;regression" TIMEOUT 30 SKIP_REGULAR_EXPRESSION "SKIP:")
