# ####################################################################################################################################################
# tests/pbt/CMakeLists.txt
#
# Property-based test targets using Catch2 v3 + RapidCheck.
#
# Catch2 v3 is installed at /opt/catch2 in the dev container. RapidCheck is installed at /opt/rapidcheck (built WITHOUT RC_ENABLE_CATCH since the
# bundled catch submodule was missing).
#
# Integration pattern: since RC_ENABLE_CATCH=OFF, we use RapidCheck standalone (rc::check) wrapped in Catch2 TEST_CASE macros:
#
# TEST_CASE("Property: ...", "[pbt]") { rc::check("description", [](/* generated args */) { RC_ASSERT(condition); }); }
#
# RC_PARAMS max_success=100 is configured via the ENVIRONMENT property on each test target.
#
# Validates: R11.2 (testing infrastructure)
# ####################################################################################################################################################

# Catch2 and RapidCheck are already confirmed found by the parent tests/CMakeLists.txt before add_subdirectory(pbt) is called. Re-find here for
# clarity and to get the imported targets.
find_package(Catch2 3 REQUIRED CONFIG)
find_package(rapidcheck REQUIRED CONFIG)

# Common include directory for PBT headers (generators, mocks, etc.)
set(PBT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Threads (needed for mock_backend_driver and mock_mpi)
find_package(Threads QUIET)

# ####################################################################################################################################################
# AMIO::pbt_common -- INTERFACE library providing common PBT headers and link dependencies.  All PBT test executables link against this target to get
# the include paths, Catch2, RapidCheck, and AMIO.
# ####################################################################################################################################################
add_library(amio_pbt_common INTERFACE)
add_library(AMIO::pbt_common ALIAS amio_pbt_common)

target_include_directories(amio_pbt_common INTERFACE ${PBT_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(amio_pbt_common INTERFACE AMIO::public_headers AMIO::amio_core Catch2::Catch2WithMain rapidcheck)

if(Threads_FOUND)
    target_link_libraries(amio_pbt_common INTERFACE Threads::Threads)
endif()

# ####################################################################################################################################################
# pbt_infra_test -- validates that the PBT infrastructure compiles and runs: generators produce valid values, RC_PARAMS is configured, AMIO C API is
# accessible.
# ####################################################################################################################################################
add_executable(pbt_infra_test test_pbt_infrastructure.cpp)

target_link_libraries(pbt_infra_test PRIVATE AMIO::pbt_common)

add_test(NAME pbt.infra_smoke COMMAND pbt_infra_test)
set_tests_properties(pbt.infra_smoke PROPERTIES LABELS "pbt;infrastructure" TIMEOUT 120 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p7_order_preservation -- P7: Order preservation per (dataset, variable).
#
# Validates: R6.3 For any sequence of writes to same (dataset, variable): storage layer observes writes in submission order regardless of thread
# count, NUMA pinning, or queue depth.
# ####################################################################################################################################################
add_executable(
    pbt_p7_order_preservation
    test_p7_order_preservation.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_link_libraries(pbt_p7_order_preservation PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p7_order_preservation COMMAND pbt_p7_order_preservation)
set_tests_properties(pbt.p7_order_preservation PROPERTIES LABELS "pbt;p7;order_preservation" TIMEOUT 180 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p9_worker_pool_admission -- Property P9: Worker_Pool admission control invariant.
#
# Tests: P9a: Backpressure blocks at H, resumes at L (R6.8) P9b: No-backpressure returns AMIO_ERR_QUEUE_FULL (R6.9)
#
# Validates: R6.8, R6.9
# ####################################################################################################################################################
add_executable(
    pbt_p9_worker_pool_admission
    test_p9_worker_pool_admission.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_link_libraries(pbt_p9_worker_pool_admission PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p9_worker_pool_admission COMMAND pbt_p9_worker_pool_admission)
set_tests_properties(pbt.p9_worker_pool_admission PROPERTIES LABELS "pbt;p9;worker_pool;admission" TIMEOUT 300 ENVIRONMENT
                                                             "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p8_staging_backpressure -- Property 8: Staging_Pool backpressure timeout.
#
# For any config with all buffers occupied and any timeout T in [1,100]ms: StagingPool::acquire either acquires buffer or returns nullptr
# (AMIO_ERR_STAGING_BACKPRESSURE) within T+epsilon, source pointer unmodified, queue depth unchanged.
#
# Validates: R2.6
# ####################################################################################################################################################
add_executable(pbt_p8_staging_backpressure test_p8_staging_backpressure.cpp ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp)

target_link_libraries(pbt_p8_staging_backpressure PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p8_staging_backpressure COMMAND pbt_p8_staging_backpressure)
set_tests_properties(pbt.p8_staging_backpressure PROPERTIES LABELS "pbt;staging;backpressure" TIMEOUT 300 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p5_backend_factory_dispatch -- P5: Backend factory dispatch invariant.
#
# Validates: R4.1, R4.3, R4.4, R4.5, R4.6, R4.7
# ####################################################################################################################################################
add_executable(pbt_p5_backend_factory_dispatch test_p5_backend_factory_dispatch.cpp ${CMAKE_SOURCE_DIR}/src/factory/backend_factory.cpp)

target_link_libraries(pbt_p5_backend_factory_dispatch PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p5_backend_factory_dispatch COMMAND pbt_p5_backend_factory_dispatch)
set_tests_properties(pbt.p5_backend_factory_dispatch PROPERTIES LABELS "pbt;factory" TIMEOUT 120 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p6_staging_pool_conservation -- Property 6: Staging_Pool conservation invariant.
#
# After flush + all views released, free list count == original buffer_count; no buffer simultaneously in free list and outstanding task/view.
#
# Validates: R2.9, R3.10, R5.6, R5.9, R5.10
# ####################################################################################################################################################
add_executable(pbt_p6_staging_pool_conservation test_p6_staging_pool_conservation.cpp ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp)

target_link_libraries(pbt_p6_staging_pool_conservation PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p6_staging_pool_conservation COMMAND pbt_p6_staging_pool_conservation)
set_tests_properties(pbt.p6_staging_pool_conservation PROPERTIES LABELS "pbt;p6;staging_pool;conservation" TIMEOUT 120 ENVIRONMENT
                                                                 "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# test_p3_config_roundtrip -- Property P3: Configuration round-trip
#
# For any Config produced by parse: serialize then re-parse yields deeply equal Config (field set, values, nesting). Min 100 iterations.
#
# Validates: R11.5
# ####################################################################################################################################################
add_executable(test_p3_config_roundtrip test_p3_config_roundtrip.cpp ${CMAKE_SOURCE_DIR}/src/config/config_loader.cpp)

target_link_libraries(test_p3_config_roundtrip PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p3_config_roundtrip COMMAND test_p3_config_roundtrip)
set_tests_properties(pbt.p3_config_roundtrip PROPERTIES LABELS "pbt;config;roundtrip;P3" TIMEOUT 120 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# test_pbt_handle_safety -- Property P4: Handle safety invariant.
#
# For any API entry point and any handle in {null, never-initialized, garbage, finalized, already-released, double-init}: returns appropriate
# AMIO_ERR_*, no dereference, no state mutation, output args unmodified.
#
# Validates: R1.6, R1.9, R1.10, R10.6, R10.7
# ####################################################################################################################################################
add_executable(test_pbt_handle_safety test_p4_handle_safety.cpp)
target_link_libraries(test_pbt_handle_safety PRIVATE AMIO::pbt_common)
if(Threads_FOUND)
    target_link_libraries(test_pbt_handle_safety PRIVATE Threads::Threads)
endif()
add_test(NAME pbt.handle_safety COMMAND test_pbt_handle_safety)
set_tests_properties(pbt.handle_safety PROPERTIES LABELS "pbt;handles;safety" TIMEOUT 120 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# test_p2_config_validity -- Property P2: Configuration validity invariant.
#
# For any generated manifest: amio_init succeeds iff all numeric fields in declared ranges, all required fields present, all codecs on allow-list; on
# rejection returns documented AMIO_ERR_* with no resource allocation.
#
# Validates: R1.3, R1.5, R3.1, R5.1, R5.5, R8.3, R8.10, R11.1, R11.4, R11.6, R11.7
# ####################################################################################################################################################
add_executable(test_p2_config_validity test_p2_config_validity.cpp ${CMAKE_SOURCE_DIR}/src/config/config_loader.cpp)

target_link_libraries(test_p2_config_validity PRIVATE AMIO::pbt_common)
if(eckit_FOUND)
    target_link_libraries(test_p2_config_validity PRIVATE eckit)
endif()

add_test(NAME pbt.p2_config_validity COMMAND test_p2_config_validity)
set_tests_properties(pbt.p2_config_validity PROPERTIES LABELS "pbt;p2;config;validity" TIMEOUT 120 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p10_prefetch_open_scheduling -- Property P10: Prefetch open scheduling.
#
# For any (look-ahead depth N, dataset timestep count M): opening read dataset schedules exactly min(N,M) background fetch tasks before returning.
#
# Validates: R5.2
# ####################################################################################################################################################
add_executable(
    pbt_p10_prefetch_open_scheduling
    test_p10_prefetch_open_scheduling.cpp
    ${CMAKE_SOURCE_DIR}/src/prefetch/prefetch_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_link_libraries(pbt_p10_prefetch_open_scheduling PRIVATE AMIO::pbt_common Threads::Threads)

add_test(NAME pbt.p10_prefetch_open_scheduling COMMAND pbt_p10_prefetch_open_scheduling)
set_tests_properties(pbt.p10_prefetch_open_scheduling PROPERTIES LABELS "pbt;p10;prefetch;scheduling" TIMEOUT 180 ENVIRONMENT
                                                                 "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p11_prefetch_zero_io_on_hit -- Property P11: Prefetch zero-I/O on hit.
#
# For any read dataset and timestep T with completed buffer: amio_read(T) returns Memory_View without invoking Backend_Driver::read or storage I/O on
# calling thread.
#
# Validates: R5.3
# ####################################################################################################################################################
add_executable(
    pbt_p11_prefetch_zero_io_on_hit
    test_p11_prefetch_zero_io_on_hit.cpp
    ${CMAKE_SOURCE_DIR}/src/prefetch/prefetch_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_link_libraries(pbt_p11_prefetch_zero_io_on_hit PRIVATE AMIO::pbt_common Threads::Threads)

add_test(NAME pbt.p11_prefetch_zero_io_on_hit COMMAND pbt_p11_prefetch_zero_io_on_hit)
set_tests_properties(pbt.p11_prefetch_zero_io_on_hit PROPERTIES LABELS "pbt;p11;prefetch;zero_io" TIMEOUT 180 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p12_prefetch_lookahead_replenishment -- Property P12: Prefetch look-ahead replenishment.
#
# For any timestep T returned by amio_read: if T+N within bounds → schedule fetch for T+N; if T+N outside bounds → no additional fetch scheduled.
#
# Validates: R5.4
# ####################################################################################################################################################
add_executable(
    pbt_p12_prefetch_lookahead_replenishment
    test_p12_prefetch_lookahead_replenishment.cpp
    ${CMAKE_SOURCE_DIR}/src/prefetch/prefetch_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_link_libraries(pbt_p12_prefetch_lookahead_replenishment PRIVATE AMIO::pbt_common Threads::Threads)

add_test(NAME pbt.p12_prefetch_lookahead_replenishment COMMAND pbt_p12_prefetch_lookahead_replenishment)
set_tests_properties(pbt.p12_prefetch_lookahead_replenishment PROPERTIES LABELS "pbt;p12;prefetch;replenishment" TIMEOUT 180 ENVIRONMENT
                                                                         "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p13_bounding_box_selectivity -- Property P13: Bounding-box read selectivity.
#
# For any read request with bounding box or stride descriptor: Backend_Driver requests only intersecting byte ranges; bytes outside descriptor not
# transferred from storage.
#
# Validates: R5.7
# ####################################################################################################################################################
add_executable(
    pbt_p13_bounding_box_selectivity
    test_p13_bounding_box_selectivity.cpp
    ${CMAKE_SOURCE_DIR}/src/prefetch/prefetch_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_link_libraries(pbt_p13_bounding_box_selectivity PRIVATE AMIO::pbt_common Threads::Threads)

add_test(NAME pbt.p13_bounding_box_selectivity COMMAND pbt_p13_bounding_box_selectivity)
set_tests_properties(pbt.p13_bounding_box_selectivity PROPERTIES LABELS "pbt;p13;prefetch;bounding_box" TIMEOUT 180 ENVIRONMENT
                                                                 "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p14_prefetch_failure_surfaces -- Property P14: Prefetch failure surfaces on read.
#
# For any timestep T whose background fetch failed: Prefetch_Queue retains failure record; next amio_read(T) returns non-zero AMIO_ERR_* and no
# Memory_View.
#
# Validates: R5.8
# ####################################################################################################################################################
add_executable(
    pbt_p14_prefetch_failure_surfaces
    test_p14_prefetch_failure_surfaces.cpp
    ${CMAKE_SOURCE_DIR}/src/prefetch/prefetch_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_link_libraries(pbt_p14_prefetch_failure_surfaces PRIVATE AMIO::pbt_common Threads::Threads)

add_test(NAME pbt.p14_prefetch_failure_surfaces COMMAND pbt_p14_prefetch_failure_surfaces)
set_tests_properties(pbt.p14_prefetch_failure_surfaces PROPERTIES LABELS "pbt;p14;prefetch;failure" TIMEOUT 180 ENVIRONMENT
                                                                  "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p15_driver_thread_invariant -- Property P15: Driver-thread invariant.
#
# For any amio_write, amio_read, or amio_flush: no Backend_Driver virtual method executes on calling thread; all execute exclusively on Worker_Pool
# threads.
#
# Validates: R3.4, R6.1
# ####################################################################################################################################################
add_executable(
    pbt_p15_driver_thread_invariant
    test_p15_driver_thread_invariant.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_link_libraries(pbt_p15_driver_thread_invariant PRIVATE AMIO::pbt_common Threads::Threads)

add_test(NAME pbt.p15_driver_thread_invariant COMMAND pbt_p15_driver_thread_invariant)
set_tests_properties(pbt.p15_driver_thread_invariant PROPERTIES LABELS "pbt;p15;driver_thread;invariant" TIMEOUT 180 ENVIRONMENT
                                                                "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p16_exception_bridge -- Property P16: Exception bridge invariant.
#
# For any exception on Worker_Pool thread (eckit::Exception, std::exception, unknown): C boundary catches, records AMIO_ERR_* against handle, ensures
# parallel stack-trace emission completes before surfacing, surfaces on next flush/close/wait without remapping, preserves trace for AMIO_Core
# lifetime.
#
# Validates: R12.1, R12.2, R12.4, R12.9, R12.10
# ####################################################################################################################################################
add_executable(
    pbt_p16_exception_bridge
    test_p16_exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_link_libraries(pbt_p16_exception_bridge PRIVATE AMIO::pbt_common Threads::Threads)

add_test(NAME pbt.p16_exception_bridge COMMAND pbt_p16_exception_bridge)
set_tests_properties(pbt.p16_exception_bridge PROPERTIES LABELS "pbt;p16;exception_bridge" TIMEOUT 180 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p17_error_code_totality -- Property P17: Error code totality and strerror correctness.
#
# For any API call with arbitrary inputs: return is AMIO_OK or defined AMIO_ERR_*; for any int i to amio_strerror(i): non-null null-terminated string;
# defined codes → byte-equal across invocations, side-effect free; undefined codes → "unknown" description, non-null, no table mutation.
#
# Validates: R12.5, R12.6, R12.8
# ####################################################################################################################################################
add_executable(pbt_p17_error_code_totality test_p17_error_code_totality.cpp)

target_link_libraries(pbt_p17_error_code_totality PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p17_error_code_totality COMMAND pbt_p17_error_code_totality)
set_tests_properties(pbt.p17_error_code_totality PROPERTIES LABELS "pbt;p17;error_code;strerror" TIMEOUT 120 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p18_invalid_binding_rejection -- Property P18: Invalid binding rejection.
#
# For any manifest with CPU core/NUMA domain not present or not permitted: amio_init returns AMIO_ERR_INVALID_BINDING, no Worker_Pool created, calling
# thread affinity unchanged.
#
# Validates: R3.3
# ####################################################################################################################################################
add_executable(pbt_p18_invalid_binding_rejection test_p18_invalid_binding_rejection.cpp ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp)

target_link_libraries(pbt_p18_invalid_binding_rejection PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p18_invalid_binding_rejection COMMAND pbt_p18_invalid_binding_rejection)
set_tests_properties(pbt.p18_invalid_binding_rejection PROPERTIES LABELS "pbt;p18;invalid_binding" TIMEOUT 120 ENVIRONMENT
                                                                  "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p19_invalid_io_communicator -- Property P19: Invalid I/O communicator rejection.
#
# For any manifest with I/O rank set not subset of world communicator: amio_init returns AMIO_ERR_COMM_SPLIT_FAILED, no Worker_Pool created, world
# communicator unmodified.
#
# Validates: R3.6
# ####################################################################################################################################################
add_executable(pbt_p19_invalid_io_communicator test_p19_invalid_io_communicator.cpp ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp)

target_link_libraries(pbt_p19_invalid_io_communicator PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p19_invalid_io_communicator COMMAND pbt_p19_invalid_io_communicator)
set_tests_properties(pbt.p19_invalid_io_communicator PROPERTIES LABELS "pbt;p19;invalid_comm" TIMEOUT 120 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p20_invalid_write_input -- Property P20: Invalid write input rejection.
#
# For any amio_write with null pointer, unsupported dtype, rank zero, or zero/negative extent: returns AMIO_ERR_INVALID_INPUT, no Staging_Pool buffer
# acquired, no task enqueued.
#
# Validates: R2.10
# ####################################################################################################################################################
add_executable(pbt_p20_invalid_write_input test_p20_invalid_write_input.cpp)

target_link_libraries(pbt_p20_invalid_write_input PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p20_invalid_write_input COMMAND pbt_p20_invalid_write_input)
set_tests_properties(pbt.p20_invalid_write_input PROPERTIES LABELS "pbt;p20;invalid_write;input_validation" TIMEOUT 120 ENVIRONMENT
                                                            "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p21_snapshot_copy_correctness -- Property P21: Snapshot copy correctness.
#
# For any (host pointer, shape, dtype) accepted by amio_write: after return, Staging_Pool buffer [0, payload_bytes) equals bytes at host pointer at
# call entry.
#
# Validates: R2.2
# ####################################################################################################################################################
add_executable(pbt_p21_snapshot_copy_correctness test_p21_snapshot_copy_correctness.cpp ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp)

target_link_libraries(pbt_p21_snapshot_copy_correctness PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p21_snapshot_copy_correctness COMMAND pbt_p21_snapshot_copy_correctness)
set_tests_properties(pbt.p21_snapshot_copy_correctness PROPERTIES LABELS "pbt;p21;snapshot_copy;staging" TIMEOUT 120 ENVIRONMENT
                                                                  "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p22_synchronous_wait_totality -- Property P22: Synchronous wait status totality.
#
# For any (pending I/O handle, timeout in [0, 86_400_000]ms): amio_wait returns exactly one of {AMIO_OK, AMIO_ERR_BACKEND_FAILURE, AMIO_ERR_TIMEOUT};
# same trichotomy for amio_flush aggregated over dataset.
#
# Validates: R3.9, R6.4
# ####################################################################################################################################################
add_executable(pbt_p22_synchronous_wait_totality test_p22_synchronous_wait_totality.cpp)

target_link_libraries(pbt_p22_synchronous_wait_totality PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p22_synchronous_wait_totality COMMAND pbt_p22_synchronous_wait_totality)
set_tests_properties(pbt.p22_synchronous_wait_totality PROPERTIES LABELS "pbt;p22;wait;totality" TIMEOUT 180 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p23_driver_failure_recorded -- Property P23: Driver failure recorded against handle.
#
# For any Backend_Driver serialization failure: AMIO_Core records failure against originating write handle, retains until next flush/close/wait,
# surfaces with same AMIO_ERR_* code.
#
# Validates: R6.6
# ####################################################################################################################################################
add_executable(pbt_p23_driver_failure_recorded test_p23_driver_failure_recorded.cpp)

target_link_libraries(pbt_p23_driver_failure_recorded PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p23_driver_failure_recorded COMMAND pbt_p23_driver_failure_recorded)
set_tests_properties(pbt.p23_driver_failure_recorded PROPERTIES LABELS "pbt;p23;driver_failure;recorded" TIMEOUT 180 ENVIRONMENT
                                                                "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p24_grib2_contiguity_gating -- Property P24: GRIB2 contiguity gating.
#
# For any Memory_View to GRIB2_Driver::write: bytes to encoder == row-major flatten; if contiguous+row-major → encoder receives underlying pointer
# directly (no intermediate buffer); otherwise → encoder receives contiguous packed buffer.
#
# Validates: R9.4, R9.5
# ####################################################################################################################################################
add_executable(pbt_p24_grib2_contiguity_gating test_p24_grib2_contiguity_gating.cpp ${CMAKE_SOURCE_DIR}/src/drivers/grib2/grib2_driver.cpp)

target_link_libraries(pbt_p24_grib2_contiguity_gating PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p24_grib2_contiguity_gating COMMAND pbt_p24_grib2_contiguity_gating)
set_tests_properties(pbt.p24_grib2_contiguity_gating PROPERTIES LABELS "pbt;p24;grib2;contiguity" TIMEOUT 180 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p25_grib2_metadata_invariant -- Property P25: GRIB2 metadata invariant.
#
# For any GRIB2_Driver::write: every human-readable metadata string translated to WMO code before encoding; missing WMO key or DRT outside {Adaptive
# Entropy Coding via libaec, Lossless JPEG2000} → eckit::Exception, zero output bytes.
#
# Validates: R9.3, R9.6, R9.7, R9.8
# ####################################################################################################################################################
add_executable(pbt_p25_grib2_metadata_invariant test_p25_grib2_metadata_invariant.cpp ${CMAKE_SOURCE_DIR}/src/drivers/grib2/grib2_driver.cpp)

target_link_libraries(pbt_p25_grib2_metadata_invariant PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p25_grib2_metadata_invariant COMMAND pbt_p25_grib2_metadata_invariant)
set_tests_properties(pbt.p25_grib2_metadata_invariant PROPERTIES LABELS "pbt;p25;grib2;metadata" TIMEOUT 180 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p26_zarr_cloud_failure_idempotence -- Property P26: Storage idempotence on Zarr cloud failure.
#
# For any Zarr_Driver write to cloud URI that fails (network, auth, unsupported scheme): target object byte content unchanged after call; driver
# returns categorized error.
#
# Validates: R8.9
# ####################################################################################################################################################
add_executable(
    pbt_p26_zarr_cloud_failure_idempotence test_p26_zarr_cloud_failure_idempotence.cpp ${CMAKE_SOURCE_DIR}/src/drivers/zarr/zarr_driver.cpp
                                           ${CMAKE_SOURCE_DIR}/src/drivers/zarr/zarr_nczarr_fallback.cpp)

target_link_libraries(pbt_p26_zarr_cloud_failure_idempotence PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p26_zarr_cloud_failure_idempotence COMMAND pbt_p26_zarr_cloud_failure_idempotence)
set_tests_properties(pbt.p26_zarr_cloud_failure_idempotence PROPERTIES LABELS "pbt;p26;zarr;cloud;idempotence" TIMEOUT 180 ENVIRONMENT
                                                                       "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p27_abi_stability -- Property P27: ABI stability invariant.
#
# For any AMIO_ERR_* code: integer value is stable (matches the enum definition); amio_strerror returns consistent non-null strings; all public struct
# sizes/alignments match expected values.
#
# Validates: R10.8, R12.5
# ####################################################################################################################################################
add_executable(pbt_p27_abi_stability test_p27_abi_stability.cpp)

target_link_libraries(pbt_p27_abi_stability PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p27_abi_stability COMMAND pbt_p27_abi_stability)
set_tests_properties(pbt.p27_abi_stability PROPERTIES LABELS "pbt;p27;abi;stability" TIMEOUT 120 ENVIRONMENT "RC_PARAMS=max_success=100")

# ####################################################################################################################################################
# pbt_p28_netcdf_data_model_validation -- Property P28: NetCDF data model validation.
#
# For any data model string: NetCDF_Driver accepts only "classic" and "enhanced"; any other value → error with no file created.
#
# Validates: R7.2, R7.3
# ####################################################################################################################################################
add_executable(pbt_p28_netcdf_data_model_validation test_p28_netcdf_data_model_validation.cpp
                                                    ${CMAKE_SOURCE_DIR}/src/drivers/netcdf/netcdf_driver.cpp)

target_link_libraries(pbt_p28_netcdf_data_model_validation PRIVATE AMIO::pbt_common)

add_test(NAME pbt.p28_netcdf_data_model_validation COMMAND pbt_p28_netcdf_data_model_validation)
set_tests_properties(pbt.p28_netcdf_data_model_validation PROPERTIES LABELS "pbt;p28;netcdf;data_model" TIMEOUT 180 ENVIRONMENT
                                                                     "RC_PARAMS=max_success=100")
