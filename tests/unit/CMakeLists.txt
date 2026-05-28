# ####################################################################################################################################################
# tests/unit/CMakeLists.txt
#
# Lightweight C++20 unit tests for the AMIO_C_API entry points.
#
# These tests deliberately avoid pulling in Catch2 or Rapidcheck -- task 13.1 is responsible for adding those dependencies for the property-based test
# suite.  At this stage of the build a plain `<cassert>`-based harness keeps the test target small and means a downstream consumer running `ctest`
# against AMIO needs nothing on their machine that AMIO itself does not already require.
#
# Each test_* executable links against AMIO::amio_core (so the real, installed `extern "C"` symbols are exercised, not a private copy) and against
# AMIO::public_headers for the C99 declarations.
#
# Per-test ownership map:
#
# * test_amio_strerror -- task 3.3, validates R12.5 - R12.8. Pins the strerror table contract that property test P17 (task 13.18) verifies
#   universally.
#
# * test_handle_table  -- task 3.2, validates R10.5 - R10.7. Compiles `handle_table.cpp` directly into the test binary because the header lives under
#   `src/c_boundary/` and is private to the AMIO_Core build.  Pins the generation-counter / reuse-after-free invariant that property test P4 (task
#   13.5) verifies universally.
#
# * test_amio_api_handle_validation -- task 3.2, validates the public C-Boundary handle / argument / NULL-output cordon (R10.6, R10.7, R12.2) through
#   the installed `extern "C"` AMIO_C_API symbols.  The implementation bodies behind each entry point are still stubs at this stage, so successful
#   cordon traversal lands in AMIO_ERR_BACKEND_FAILURE -- the cordon itself is what this test pins.
# ####################################################################################################################################################

add_executable(test_amio_strerror test_amio_strerror.cpp)

target_link_libraries(test_amio_strerror PRIVATE AMIO::public_headers AMIO::amio_core)

# The harness uses std::thread; on most Unix toolchains this requires linking pthreads.  CMake's Threads module exposes that portably.
find_package(Threads QUIET)
if(Threads_FOUND)
    target_link_libraries(test_amio_strerror PRIVATE Threads::Threads)
endif()

add_test(NAME unit.amio_strerror COMMAND test_amio_strerror)
set_tests_properties(unit.amio_strerror PROPERTIES LABELS "unit;c_boundary;errors" TIMEOUT 30)

# ####################################################################################################################################################
# test_handle_table
#
# Compiles `handle_table.cpp` directly into the test binary so the private header `src/c_boundary/handle_table.hpp` is reachable without exposing it
# through the public include tree.
# ####################################################################################################################################################
add_executable(test_handle_table test_handle_table.cpp ${CMAKE_SOURCE_DIR}/src/c_boundary/handle_table.cpp)

target_include_directories(test_handle_table PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_handle_table PRIVATE AMIO::public_headers)
if(Threads_FOUND)
    target_link_libraries(test_handle_table PRIVATE Threads::Threads)
endif()

add_test(NAME unit.handle_table COMMAND test_handle_table)
set_tests_properties(unit.handle_table PROPERTIES LABELS "unit;c_boundary;handle_table" TIMEOUT 30)

# ####################################################################################################################################################
# test_amio_api_handle_validation
#
# Exercises the C-Boundary cordon through the installed AMIO_C_API symbols.  Links AMIO::amio_core to pick up the real `extern "C"` entry points; the
# underlying bodies are still stubs (task 3.2) but the cordon validation paths (NULL handle, garbage handle, NULL out-pointer, exception translation)
# are fully wired.
# ####################################################################################################################################################
add_executable(test_amio_api_handle_validation test_amio_api_handle_validation.cpp)

target_link_libraries(test_amio_api_handle_validation PRIVATE AMIO::public_headers AMIO::amio_core)

add_test(NAME unit.amio_api_handle_validation COMMAND test_amio_api_handle_validation)
set_tests_properties(unit.amio_api_handle_validation PROPERTIES LABELS "unit;c_boundary;handles" TIMEOUT 30)

# ####################################################################################################################################################
# test_thread_pinning
#
# Compiles `thread_pinning.cpp` directly into the test binary so the private header `src/workers/thread_pinning.hpp` is reachable without exposing it
# through the public include tree.
#
# Validates: R3.2, R3.3
# ####################################################################################################################################################
add_executable(test_thread_pinning test_thread_pinning.cpp ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp)

target_include_directories(test_thread_pinning PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_thread_pinning PRIVATE AMIO::public_headers)
if(Threads_FOUND)
    target_link_libraries(test_thread_pinning PRIVATE Threads::Threads)
endif()

add_test(NAME unit.thread_pinning COMMAND test_thread_pinning)
set_tests_properties(unit.thread_pinning PROPERTIES LABELS "unit;workers;pinning" TIMEOUT 30)

# ####################################################################################################################################################
# test_comm_split
#
# Compiles `comm_split.cpp` directly into the test binary so the private header `src/workers/comm_split.hpp` is reachable without exposing it through
# the public include tree.
#
# Validates: R3.5, R3.6
# ####################################################################################################################################################
add_executable(test_comm_split test_comm_split.cpp ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp)

target_include_directories(test_comm_split PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_comm_split PRIVATE AMIO::public_headers)
if(Threads_FOUND)
    target_link_libraries(test_comm_split PRIVATE Threads::Threads)
endif()

add_test(NAME unit.comm_split COMMAND test_comm_split)
set_tests_properties(unit.comm_split PROPERTIES LABELS "unit;workers;comm_split" TIMEOUT 30)

# ####################################################################################################################################################
# test_staging_pool
#
# Compiles `staging_pool.cpp` directly into the test binary so the private header `src/staging/staging_pool.hpp` is reachable without exposing it
# through the public include tree.
#
# Validates: R1.3, R1.5, R2.2, R2.6, R2.9, R3.10, R5.6, R5.9
# ####################################################################################################################################################
add_executable(test_staging_pool test_staging_pool.cpp ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp)

target_include_directories(test_staging_pool PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_staging_pool PRIVATE AMIO::public_headers)
if(Threads_FOUND)
    target_link_libraries(test_staging_pool PRIVATE Threads::Threads)
endif()

add_test(NAME unit.staging_pool COMMAND test_staging_pool)
set_tests_properties(unit.staging_pool PROPERTIES LABELS "unit;staging;pool" TIMEOUT 60)

# ####################################################################################################################################################
# test_worker_pool
#
# Compiles `worker_pool.cpp` directly into the test binary so the private header `src/workers/worker_pool.hpp` is reachable without exposing it
# through the public include tree.
#
# Also compiles `thread_pinning.cpp` and `comm_split.cpp` because worker_pool.hpp includes their headers for the WorkerPoolConfig integration (R3.2,
# R3.3, R3.5, R3.6).
#
# Validates: R3.1, R3.2, R3.3, R3.4, R3.5, R3.6, R3.7, R3.8, R6.1, R6.2, R6.3, R6.8, R6.9
# ####################################################################################################################################################
add_executable(
    test_worker_pool
    test_worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_include_directories(test_worker_pool PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_worker_pool PRIVATE AMIO::public_headers)
if(Threads_FOUND)
    target_link_libraries(test_worker_pool PRIVATE Threads::Threads)
endif()

add_test(NAME unit.worker_pool COMMAND test_worker_pool)
set_tests_properties(unit.worker_pool PROPERTIES LABELS "unit;workers;pool" TIMEOUT 60)

# ####################################################################################################################################################
# test_exception_bridge
#
# Compiles `exception_bridge.cpp`, `worker_pool.cpp`, `thread_pinning.cpp`, `comm_split.cpp`, and `mpi_threading.cpp` directly into the test binary so
# the private headers under `src/workers/` are reachable.
#
# Validates: R12.1, R12.2, R12.3, R12.4, R12.9, R12.10
# ####################################################################################################################################################
add_executable(
    test_exception_bridge
    test_exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_include_directories(test_exception_bridge PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_exception_bridge PRIVATE AMIO::public_headers)
if(Threads_FOUND)
    target_link_libraries(test_exception_bridge PRIVATE Threads::Threads)
endif()

add_test(NAME unit.exception_bridge COMMAND test_exception_bridge)
set_tests_properties(unit.exception_bridge PROPERTIES LABELS "unit;workers;exception_bridge" TIMEOUT 60)

# ####################################################################################################################################################
# test_backend_factory
#
# Compiles `backend_factory.cpp` directly into the test binary so the private header `src/factory/backend_factory.hpp` is reachable without exposing
# it through the public include tree.
#
# Validates: R4.1, R4.2, R4.3, R4.4, R4.5, R4.6, R4.7, R4.8
# ####################################################################################################################################################
add_executable(test_backend_factory test_backend_factory.cpp ${CMAKE_SOURCE_DIR}/src/factory/backend_factory.cpp)

target_include_directories(test_backend_factory PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_backend_factory PRIVATE AMIO::public_headers)
if(Threads_FOUND)
    target_link_libraries(test_backend_factory PRIVATE Threads::Threads)
endif()

add_test(NAME unit.backend_factory COMMAND test_backend_factory)
set_tests_properties(unit.backend_factory PROPERTIES LABELS "unit;factory;backend" TIMEOUT 30)

# ####################################################################################################################################################
# test_config_loader
#
# Compiles `config_loader.cpp` directly into the test binary so the private header `src/config/config_loader.hpp` is reachable without exposing it
# through the public include tree.
#
# Validates: R1.2, R1.3, R1.5, R11.3, R11.4, R11.5, R11.6, R11.7
# ####################################################################################################################################################
add_executable(test_config_loader test_config_loader.cpp ${CMAKE_SOURCE_DIR}/src/config/config_loader.cpp)

target_include_directories(test_config_loader PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_config_loader PRIVATE AMIO::public_headers)
if(Threads_FOUND)
    target_link_libraries(test_config_loader PRIVATE Threads::Threads)
endif()

add_test(NAME unit.config_loader COMMAND test_config_loader)
set_tests_properties(unit.config_loader PROPERTIES LABELS "unit;config;loader" TIMEOUT 30)

# ####################################################################################################################################################
# test_dataset_lifecycle
#
# Tests the dataset open/close lifecycle through the Backend_Factory (task 6.3).  Links against AMIO::amio_core for the real extern "C" entry points.
# Compiles backend_factory.cpp directly to register a mock driver for testing.
#
# Validates: R4.1, R4.6, R6.4, R6.5
# ####################################################################################################################################################
add_executable(test_dataset_lifecycle test_dataset_lifecycle.cpp ${CMAKE_SOURCE_DIR}/src/factory/backend_factory.cpp)

target_include_directories(test_dataset_lifecycle PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_dataset_lifecycle PRIVATE AMIO::public_headers AMIO::amio_core)
if(Threads_FOUND)
    target_link_libraries(test_dataset_lifecycle PRIVATE Threads::Threads)
endif()

add_test(NAME unit.dataset_lifecycle COMMAND test_dataset_lifecycle)
set_tests_properties(unit.dataset_lifecycle PROPERTIES LABELS "unit;factory;dataset;lifecycle" TIMEOUT 30)

# ####################################################################################################################################################
# test_write_snapshot
#
# Tests the synchronous snapshot write path (task 9.1).  Exercises input validation, snapshot semantics, and io_handle creation through the installed
# extern "C" entry points.
#
# Validates: R2.1, R2.2, R2.3, R2.4, R2.5, R2.7, R2.8, R2.10, R6.1
# ####################################################################################################################################################
add_executable(
    test_write_snapshot
    test_write_snapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_api.cpp
    ${CMAKE_SOURCE_DIR}/src/factory/backend_factory.cpp
    ${CMAKE_SOURCE_DIR}/src/config/config_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/handle_table.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_core_stubs.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp
    ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/prefetch/prefetch_queue.cpp)

target_include_directories(test_write_snapshot PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_write_snapshot PRIVATE AMIO::public_headers)
if(Threads_FOUND)
    target_link_libraries(test_write_snapshot PRIVATE Threads::Threads)
endif()

add_test(NAME unit.write_snapshot COMMAND test_write_snapshot)
set_tests_properties(unit.write_snapshot PROPERTIES LABELS "unit;write;snapshot;staging" TIMEOUT 60)

# ####################################################################################################################################################
# test_read_prefetch
#
# Tests the read prefetch path (task 9.2).  Exercises the PrefetchQueue, amio_read wiring, and view lifecycle through direct unit tests of the
# PrefetchQueue class.
#
# Validates: R5.1, R5.2, R5.3, R5.4, R5.5, R5.7, R5.8
# ####################################################################################################################################################
add_executable(
    test_read_prefetch
    test_read_prefetch.cpp
    ${CMAKE_SOURCE_DIR}/src/prefetch/prefetch_queue.cpp
    ${CMAKE_SOURCE_DIR}/src/staging/staging_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/worker_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/thread_pinning.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/comm_split.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/mpi_threading.cpp
    ${CMAKE_SOURCE_DIR}/src/workers/exception_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/c_boundary/amio_strerror.cpp)

target_include_directories(test_read_prefetch PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_read_prefetch PRIVATE AMIO::public_headers)
if(Threads_FOUND)
    target_link_libraries(test_read_prefetch PRIVATE Threads::Threads)
endif()

add_test(NAME unit.read_prefetch COMMAND test_read_prefetch)
set_tests_properties(unit.read_prefetch PROPERTIES LABELS "unit;prefetch;read" TIMEOUT 60)
