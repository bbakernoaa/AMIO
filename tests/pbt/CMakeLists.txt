####################################################################
# tests/pbt/CMakeLists.txt
#
# Property-based test targets using Catch2 v3 + RapidCheck.
#
# Catch2 v3 is installed at /opt/catch2 in the dev container.
# RapidCheck is installed at /opt/rapidcheck (built WITHOUT
# RC_ENABLE_CATCH since the bundled catch submodule was missing).
#
# Integration pattern: since RC_ENABLE_CATCH=OFF, we use RapidCheck
# standalone (rc::check) wrapped in Catch2 TEST_CASE macros:
#
#   TEST_CASE("Property: ...", "[pbt]") {
#       rc::check("description", [](/* generated args */) {
#           RC_ASSERT(condition);
#       });
#   }
#
# RC_PARAMS max_success=100 is configured via the ENVIRONMENT
# property on each test target.
#
# Validates: R11.2 (testing infrastructure)
####################################################################

# Catch2 and RapidCheck are already confirmed found by the parent
# tests/CMakeLists.txt before add_subdirectory(pbt) is called.
# Re-find here for clarity and to get the imported targets.
find_package(Catch2 3 REQUIRED CONFIG)
find_package(rapidcheck REQUIRED CONFIG)

# Common include directory for PBT headers (generators, mocks, etc.)
set(PBT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Threads (needed for mock_backend_driver and mock_mpi)
find_package(Threads QUIET)

####################################################################
# AMIO::pbt_common -- INTERFACE library providing common PBT headers
# and link dependencies.  All PBT test executables link against this
# target to get the include paths, Catch2, RapidCheck, and AMIO.
####################################################################
add_library(amio_pbt_common INTERFACE)
add_library(AMIO::pbt_common ALIAS amio_pbt_common)

target_include_directories(amio_pbt_common
    INTERFACE
        ${PBT_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(amio_pbt_common
    INTERFACE
        AMIO::public_headers
        AMIO::amio_core
        Catch2::Catch2WithMain
        rapidcheck
)

if(Threads_FOUND)
    target_link_libraries(amio_pbt_common INTERFACE Threads::Threads)
endif()

####################################################################
# pbt_infra_test -- validates that the PBT infrastructure compiles
# and runs: generators produce valid values, RC_PARAMS is configured,
# AMIO C API is accessible.
####################################################################
add_executable(pbt_infra_test
    test_pbt_infrastructure.cpp
)

target_link_libraries(pbt_infra_test
    PRIVATE
        AMIO::pbt_common
)

add_test(
    NAME    pbt.infra_smoke
    COMMAND pbt_infra_test
)
set_tests_properties(pbt.infra_smoke PROPERTIES
    LABELS  "pbt;infrastructure"
    TIMEOUT 120
    ENVIRONMENT "RC_PARAMS=max_success=100"
)

####################################################################
# pbt_p7_order_preservation -- P7: Order preservation per
# (dataset, variable).
#
# Validates: R6.3
# For any sequence of writes to same (dataset, variable): storage
# layer observes writes in submission order regardless of thread
# count, NUMA pinning, or queue depth.
####################################################################
add_executable(pbt_p7_order_preservation
    test_p7_order_preservation.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp
)

target_link_libraries(pbt_p7_order_preservation
    PRIVATE
        AMIO::pbt_common
)

add_test(
    NAME    pbt.p7_order_preservation
    COMMAND pbt_p7_order_preservation
)
set_tests_properties(pbt.p7_order_preservation PROPERTIES
    LABELS  "pbt;p7;order_preservation"
    TIMEOUT 180
    ENVIRONMENT "RC_PARAMS=max_success=100"
)

####################################################################
# pbt_p9_worker_pool_admission -- Property P9: Worker_Pool admission
# control invariant.
#
# Tests:
#   P9a: Backpressure blocks at H, resumes at L (R6.8)
#   P9b: No-backpressure returns AMIO_ERR_QUEUE_FULL (R6.9)
#
# Validates: R6.8, R6.9
####################################################################
add_executable(pbt_p9_worker_pool_admission
    test_p9_worker_pool_admission.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp
)

target_link_libraries(pbt_p9_worker_pool_admission
    PRIVATE
        AMIO::pbt_common
)

add_test(
    NAME    pbt.p9_worker_pool_admission
    COMMAND pbt_p9_worker_pool_admission
)
set_tests_properties(pbt.p9_worker_pool_admission PROPERTIES
    LABELS  "pbt;p9;worker_pool;admission"
    TIMEOUT 300
    ENVIRONMENT "RC_PARAMS=max_success=100"
)

####################################################################
# pbt_p8_staging_backpressure -- Property 8: Staging_Pool
# backpressure timeout.
#
# For any config with all buffers occupied and any timeout T in
# [1,100]ms: StagingPool::acquire either acquires buffer or returns
# nullptr (AMIO_ERR_STAGING_BACKPRESSURE) within T+epsilon, source
# pointer unmodified, queue depth unchanged.
#
# Validates: R2.6
####################################################################
add_executable(pbt_p8_staging_backpressure
    test_p8_staging_backpressure.cpp
    ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp
)

target_link_libraries(pbt_p8_staging_backpressure
    PRIVATE
        AMIO::pbt_common
)

add_test(
    NAME    pbt.p8_staging_backpressure
    COMMAND pbt_p8_staging_backpressure
)
set_tests_properties(pbt.p8_staging_backpressure PROPERTIES
    LABELS  "pbt;staging;backpressure"
    TIMEOUT 300
    ENVIRONMENT "RC_PARAMS=max_success=100"
)

####################################################################
# pbt_p5_backend_factory_dispatch -- P5: Backend factory dispatch
# invariant.
#
# Validates: R4.1, R4.3, R4.4, R4.5, R4.6, R4.7
####################################################################
add_executable(pbt_p5_backend_factory_dispatch
    test_p5_backend_factory_dispatch.cpp
    ${CMAKE_SOURCE_DIR}/src/factory/backend_factory.cpp
)

target_link_libraries(pbt_p5_backend_factory_dispatch
    PRIVATE
        AMIO::pbt_common
)

add_test(
    NAME    pbt.p5_backend_factory_dispatch
    COMMAND pbt_p5_backend_factory_dispatch
)
set_tests_properties(pbt.p5_backend_factory_dispatch PROPERTIES
    LABELS  "pbt;factory"
    TIMEOUT 120
    ENVIRONMENT "RC_PARAMS=max_success=100"
)

####################################################################
# pbt_p6_staging_pool_conservation -- Property 6: Staging_Pool
# conservation invariant.
#
# After flush + all views released, free list count == original
# buffer_count; no buffer simultaneously in free list and
# outstanding task/view.
#
# Validates: R2.9, R3.10, R5.6, R5.9, R5.10
####################################################################
add_executable(pbt_p6_staging_pool_conservation
    test_p6_staging_pool_conservation.cpp
    ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp
)

target_link_libraries(pbt_p6_staging_pool_conservation
    PRIVATE
        AMIO::pbt_common
)

add_test(
    NAME    pbt.p6_staging_pool_conservation
    COMMAND pbt_p6_staging_pool_conservation
)
set_tests_properties(pbt.p6_staging_pool_conservation PROPERTIES
    LABELS  "pbt;p6;staging_pool;conservation"
    TIMEOUT 120
    ENVIRONMENT "RC_PARAMS=max_success=100"
)

####################################################################
# test_p3_config_roundtrip -- Property P3: Configuration round-trip
#
# For any Config produced by parse: serialize then re-parse yields
# deeply equal Config (field set, values, nesting).
# Min 100 iterations.
#
# Validates: R11.5
####################################################################
add_executable(test_p3_config_roundtrip
    test_p3_config_roundtrip.cpp
    ${CMAKE_SOURCE_DIR}/src/config/config_loader.cpp
)

target_link_libraries(test_p3_config_roundtrip
    PRIVATE
        AMIO::pbt_common
)

add_test(
    NAME    pbt.p3_config_roundtrip
    COMMAND test_p3_config_roundtrip
)
set_tests_properties(pbt.p3_config_roundtrip PROPERTIES
    LABELS      "pbt;config;roundtrip;P3"
    TIMEOUT     120
    ENVIRONMENT "RC_PARAMS=max_success=100"
)

####################################################################
# test_pbt_handle_safety -- Property P4: Handle safety invariant.
#
# For any API entry point and any handle in {null, never-initialized,
# garbage, finalized, already-released, double-init}: returns
# appropriate AMIO_ERR_*, no dereference, no state mutation, output
# args unmodified.
#
# Validates: R1.6, R1.9, R1.10, R10.6, R10.7
####################################################################
add_executable(test_pbt_handle_safety test_p4_handle_safety.cpp)
target_link_libraries(test_pbt_handle_safety PRIVATE AMIO::pbt_common)
if(Threads_FOUND)
    target_link_libraries(test_pbt_handle_safety PRIVATE Threads::Threads)
endif()
add_test(NAME pbt.handle_safety COMMAND test_pbt_handle_safety)
set_tests_properties(pbt.handle_safety PROPERTIES
    LABELS "pbt;handles;safety"
    TIMEOUT 120
    ENVIRONMENT "RC_PARAMS=max_success=100"
)

####################################################################
# test_p2_config_validity -- Property P2: Configuration validity
# invariant.
#
# For any generated manifest: amio_init succeeds iff all numeric
# fields in declared ranges, all required fields present, all codecs
# on allow-list; on rejection returns documented AMIO_ERR_* with no
# resource allocation.
#
# Validates: R1.3, R1.5, R3.1, R5.1, R5.5, R8.3, R8.10, R11.1,
#            R11.4, R11.6, R11.7
####################################################################
add_executable(test_p2_config_validity
    test_p2_config_validity.cpp
    ${CMAKE_SOURCE_DIR}/src/config/config_loader.cpp
)

target_link_libraries(test_p2_config_validity
    PRIVATE
        AMIO::pbt_common
)
if(eckit_FOUND)
    target_link_libraries(test_p2_config_validity PRIVATE eckit)
endif()

add_test(
    NAME    pbt.p2_config_validity
    COMMAND test_p2_config_validity
)
set_tests_properties(pbt.p2_config_validity PROPERTIES
    LABELS  "pbt;p2;config;validity"
    TIMEOUT 120
    ENVIRONMENT "RC_PARAMS=max_success=100"
)
