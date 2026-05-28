# ####################################################################################################################################################
# tests/CMakeLists.txt
#
# Registers AMIO's CTest targets.  Included from the top-level CMakeLists.txt only when AMIO_BUILD_TESTING=ON.
#
# Sub-directories grouped by concern:
#
# * header_isolation/  -- task 1.4: header-isolation and dependency-closure CI checks (C99 compile, C++ inclusion, symbol mangling, no Rust/Cargo/Go
#   closure).
#
# * cmake_discovery/   -- task 1.5: build-time verification of the TensorStore discovery / NCZarr fallback configure flow described in design.md
#   (mermaid diagram "TensorStore discovery and NCZarr fallback (configure-time)"). Validates R13.3, R13.4, R13.5.
#
# * unit/              -- C++20 unit tests for individual AMIO_C_API entry points.  No Catch2 / Rapidcheck dependency at this stage; a plain cassert
#   harness keeps the test target lean while the FFI surface is being assembled (tasks 3.x - 9.x).
#
# * (future) pbt/      -- tasks 13.x will populate property-based tests with Rapidcheck + Catch2.
#
# Each subdirectory is responsible for its own add_test() calls; this file only wires them in.
# ####################################################################################################################################################

add_subdirectory(header_isolation)
add_subdirectory(cmake_discovery)
add_subdirectory(unit)

# PBT (property-based tests) require Catch2 v3 and RapidCheck. These are installed at /opt/catch2 and /opt/rapidcheck in the dev container.  Skip this
# subdirectory gracefully when they are not available (e.g., lightweight CI builds without PBT deps).
find_package(Catch2 3 CONFIG QUIET)
find_package(rapidcheck CONFIG QUIET)
if(Catch2_FOUND AND rapidcheck_FOUND)
    add_subdirectory(pbt)
else()
    message(STATUS "AMIO PBT: Catch2 or RapidCheck not found; skipping tests/pbt/")
endif()
