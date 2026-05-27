####################################################################
# AMIO - Asynchronous Multidimensional I/O
#
# Top-level CMake build for libamio (C++20 core), the C99 FFI surface,
# the Fortran 2003 iso_c_binding wrapper, and the three pluggable
# backend drivers (NetCDF-4, Zarr v3, GRIB2).
#
# This file establishes the *target topology* described in the design
# document.  Subsequent tasks (1.2 - 13.x) populate the targets with
# concrete sources; the build is structured so that:
#
#   * `amio_public_headers` is the only target that downstream
#     consumers of `libamio` ever see on their include path.  Its
#     INTERFACE_INCLUDE_DIRECTORIES is restricted to the C99-only
#     include tree (R10.1, R10.2, R13.6).
#
#   * `amio_core` (SHARED, libamio.so) PRIVATE-links every C++
#     dependency (eckit, kokkos/mdspan, MPI, optionally TensorStore,
#     netCDF-cxx4, nceplibs-g2c).  None of those headers leak through
#     the public surface.
#
#   * `driver_netcdf`, `driver_zarr`, `driver_grib2` are STATIC
#     archives that are PRIVATE-linked into `amio_core`.  They are
#     never installed and never visible to consumers.
#
#   * `amio_fortran` (STATIC) depends only on `amio_public_headers`
#     and on libamio.so's exported `extern "C"` symbols.  No C++
#     symbols are reachable from Fortran link units (R10.4, R10.5).
#
#   * `install(EXPORT)` produces `AMIOConfig.cmake` and an associated
#     targets file so `find_package(AMIO)` provides imported targets
#     with no manual include / link / flag plumbing (R13.1).
#
# Requirements satisfied by this file: R13.1, R13.6.
# Hard dependency discovery (R13.2 - Rust/Cargo/Go prohibitions,
# R13.3-R13.5 - TensorStore/NCZarr fallback flow) is delegated to
# task 1.2 (`cmake/FindTensorStore.cmake`) and the CI checks added in
# task 1.4.
####################################################################

cmake_minimum_required(VERSION 3.20)

project(AMIO
    VERSION   0.1.0
    LANGUAGES C CXX Fortran
    DESCRIPTION "Asynchronous Multidimensional I/O engine for NOAA NWS Earth system models"
)

####################################################################
# Global build settings
####################################################################

# C99 for the public FFI surface (R10.1, R10.2).
set(CMAKE_C_STANDARD            99)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_C_EXTENSIONS          OFF)

# C++20 for AMIO_Core (design.md - "Implementation language").
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Fortran 2003+ for the iso_c_binding wrapper.
set(CMAKE_Fortran_STANDARD      2003)

# Position-independent code is required because static driver archives
# are linked into `libamio.so`.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Hide all symbols by default; libamio.so explicitly marks the
# `extern "C"` AMIO_C_API entry points with default visibility (added
# in task 3.1 / 3.2).  This keeps the exported symbol set small and
# makes the `nm -D` symbol-mangling check in task 1.4 meaningful.
set(CMAKE_C_VISIBILITY_PRESET   hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Standardize CMake module path so cmake/FindTensorStore.cmake (task
# 1.2) and friends are picked up automatically.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(AMIO_BUILD_TESTING "Build AMIO unit and property-based tests" OFF)
if(AMIO_BUILD_TESTING)
    enable_testing()
endif()

####################################################################
# Dependency discovery
#
# All discovery is performed at the top level so that target-creation
# blocks below can reference the imported targets.  Discovery is
# intentionally tolerant in task 1.1 (placeholder sources do not yet
# need any of these libraries to compile); task 1.2 hardens
# TensorStore + NCZarr discovery into a single configure-time flow
# with FATAL_ERROR semantics, and task 1.3 declares the closure in the
# Spack recipe.
####################################################################

# eckit - infrastructure dependency (config, threads, MPI wrapping,
# Factory, exceptions, logging).  Required for AMIO_Core.
find_package(eckit CONFIG QUIET)

# kokkos/mdspan - reference implementation of std::experimental::mdspan
# used for Memory_View.  Header-only; PRIVATE to amio_core.
find_package(mdspan CONFIG QUIET)

# MPI - required for parallel I/O paths (NetCDF-4 Parallel HDF5,
# eckit::mpi communicator splits).
find_package(MPI QUIET COMPONENTS C CXX Fortran)

# NetCDF-cxx4 + Parallel HDF5 - NetCDF_Driver.
find_package(netCDF       CONFIG QUIET)
find_package(netCDFCxx    CONFIG QUIET)

####################################################################
# TensorStore discovery + NCZarr fallback (R13.3, R13.4, R13.5, R8.6)
#
# Mirrors the mermaid flow in design.md ("TensorStore discovery and
# NCZarr fallback (configure-time)").  The decision is:
#
#   1. AMIO_FORCE_NCZARR=ON    -> skip TensorStore discovery entirely
#   2. find_package(TensorStore) succeeds -> TensorStore mode
#   3. otherwise                -> NCZarr fallback (requires NCZarr-
#                                  capable netCDF-c)
#   4. neither available        -> FATAL_ERROR (no Zarr backend)
#
# The actual probe lives in cmake/FindTensorStore.cmake.  This block
# only arbitrates between the resulting AMIO_HAS_TENSORSTORE state and
# the netCDF-c NCZarr capability detected via check_symbol_exists().
####################################################################

option(AMIO_FORCE_NCZARR
    "Skip TensorStore discovery; build Zarr_Driver in NCZarr fallback mode"
    OFF)

# Custom find module -- emits AMIO_HAS_TENSORSTORE and TensorStore_REASON.
find_package(TensorStore MODULE QUIET)

# Detect whether the linked netCDF-c was built with NCZarr support.
# `NC_HAS_NCZARR` (defined to 1 in netcdf-c >= 4.8.0 when configured
# with `--enable-nczarr`) is the canonical capability macro and lives
# in `netcdf_meta.h`.  Older netcdf-c only exposes `NC_FORMATX_NCZARR`
# in `netcdf.h`; we accept that as a heuristic so legacy installs can
# still drive the fallback path.
set(AMIO_NETCDF_HAS_NCZARR FALSE)
if(netCDF_FOUND)
    include(CheckCSourceCompiles)
    set(_amio_nc_inc "")
    if(DEFINED netCDF_INCLUDE_DIRS AND netCDF_INCLUDE_DIRS)
        set(_amio_nc_inc "${netCDF_INCLUDE_DIRS}")
    elseif(TARGET netCDF::netcdf)
        get_target_property(_amio_nc_inc netCDF::netcdf
                            INTERFACE_INCLUDE_DIRECTORIES)
    endif()
    set(CMAKE_REQUIRED_INCLUDES "${_amio_nc_inc}")
    set(CMAKE_REQUIRED_QUIET    FALSE)

    # Probe 1: the canonical `NC_HAS_NCZARR` macro from netcdf_meta.h.
    check_c_source_compiles("
        #include <netcdf_meta.h>
        #if !defined(NC_HAS_NCZARR) || (NC_HAS_NCZARR + 0) == 0
        #  error \"NC_HAS_NCZARR not enabled\"
        #endif
        int main(void) { return 0; }
        " AMIO_NETCDF_HAS_NCZARR_META)

    # Probe 2: legacy heuristic against `NC_FORMATX_NCZARR` in netcdf.h.
    check_c_source_compiles("
        #include <netcdf.h>
        #ifndef NC_FORMATX_NCZARR
        #  error \"NC_FORMATX_NCZARR not declared\"
        #endif
        int main(void) { return 0; }
        " AMIO_NETCDF_HAS_NCZARR_LEGACY)

    if(AMIO_NETCDF_HAS_NCZARR_META OR AMIO_NETCDF_HAS_NCZARR_LEGACY)
        set(AMIO_NETCDF_HAS_NCZARR TRUE)
    endif()

    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_QUIET)
    unset(_amio_nc_inc)
endif()

# Decide which Zarr backend the build will use, set the human-readable
# summary line, and FATAL_ERROR if neither path is viable.  The actual
# selection / messaging contract lives in cmake/AMIOZarrBackend.cmake
# so the same code path is exercised by both this production configure
# and the build-time verification tests in tests/cmake_discovery/
# (task 1.5).
include(AMIOZarrBackend)
amio_select_zarr_backend()

# nceplibs-g2c - GRIB2 encoder/decoder.
find_package(g2c CONFIG QUIET)

####################################################################
# Target 1: amio_public_headers (INTERFACE, C99-only)
#
# This target's INTERFACE_INCLUDE_DIRECTORIES exposes ONLY the
# `include/` tree, which is constrained to C99 headers.  No transitive
# include directory from eckit, TensorStore, netCDF, or g2c is allowed
# to reach this target -- the header-isolation CI check (task 1.4)
# verifies this with `gcc -std=c99 -pedantic -Werror -c amio.h`
# compiled with no other include flags.
####################################################################

add_library(amio_public_headers INTERFACE)
add_library(AMIO::public_headers ALIAS amio_public_headers)

target_include_directories(amio_public_headers
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Public headers compile against C99.  Note: this is INTERFACE so the
# constraint propagates to consumers, ensuring they never accidentally
# require a C99-incompatible feature in code that includes amio.h.
target_compile_features(amio_public_headers
    INTERFACE
        c_std_99
)

####################################################################
# Static backend driver targets
#
# Each driver is a STATIC archive PRIVATE-linked into amio_core.  They
# are not installed.  Each driver's link dependencies are declared
# locally so its public surface is minimal.  Real sources are added
# in tasks 7.1-7.4; for task 1.1 each driver compiles a single stub
# translation unit that registers the driver's factory key (the
# registration body itself arrives in task 6.2 / 7.x).
####################################################################

# ---- driver_netcdf ----------------------------------------------------
add_library(driver_netcdf STATIC
    src/drivers/netcdf/netcdf_driver_stub.cpp
)
add_library(AMIO::driver_netcdf ALIAS driver_netcdf)

target_include_directories(driver_netcdf
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(driver_netcdf
    PRIVATE
        amio_public_headers
)
if(eckit_FOUND)
    target_link_libraries(driver_netcdf PRIVATE eckit)
endif()
if(netCDFCxx_FOUND)
    target_link_libraries(driver_netcdf PRIVATE netCDF::netcdf-cxx4)
elseif(netCDF_FOUND)
    target_link_libraries(driver_netcdf PRIVATE netCDF::netcdf)
endif()
if(MPI_CXX_FOUND)
    target_link_libraries(driver_netcdf PRIVATE MPI::MPI_CXX)
endif()

# ---- driver_zarr -----------------------------------------------------
add_library(driver_zarr STATIC
    src/drivers/zarr/zarr_driver_stub.cpp
)
add_library(AMIO::driver_zarr ALIAS driver_zarr)

target_include_directories(driver_zarr
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(driver_zarr
    PRIVATE
        amio_public_headers
)
if(eckit_FOUND)
    target_link_libraries(driver_zarr PRIVATE eckit)
endif()
if(AMIO_HAS_TENSORSTORE)
    # TensorStore mode: link the TensorStore CONFIG-imported target.
    # No -DAMIO_NCZARR_FALLBACK is set; the driver compiles its
    # TensorStore variant (R8.1-R8.5).
    target_link_libraries(driver_zarr PRIVATE TensorStore::tensorstore)
    target_compile_definitions(driver_zarr PRIVATE AMIO_HAS_TENSORSTORE=1)
else()
    # NCZarr fallback mode: link netCDF-c (the NCZarr capability check
    # in the dependency-discovery block above already gated this with
    # FATAL_ERROR if neither backend is available).  The driver
    # compiles its NCZarr variant (R8.6-R8.8, R13.5).
    target_link_libraries(driver_zarr PRIVATE netCDF::netcdf)
    target_compile_definitions(driver_zarr PRIVATE AMIO_NCZARR_FALLBACK)
endif()

# ---- driver_grib2 ----------------------------------------------------
add_library(driver_grib2 STATIC
    src/drivers/grib2/grib2_driver_stub.cpp
)
add_library(AMIO::driver_grib2 ALIAS driver_grib2)

target_include_directories(driver_grib2
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(driver_grib2
    PRIVATE
        amio_public_headers
)
if(eckit_FOUND)
    target_link_libraries(driver_grib2 PRIVATE eckit)
endif()
if(g2c_FOUND)
    target_link_libraries(driver_grib2 PRIVATE g2c::g2c)
endif()

####################################################################
# Target 2: amio_core (SHARED, libamio.so)
#
# AMIO_Core owns the C-Boundary, handle table, Staging_Pool,
# Worker_Pool, Backend_Factory, Config_Loader, and exception bridge.
# It links the static driver archives PRIVATELY so the drivers do not
# appear in the install tree and their headers are never reachable by
# consumers.
####################################################################

add_library(amio_core SHARED
    src/c_boundary/amio_api.cpp
    src/c_boundary/amio_core_stubs.cpp
    src/c_boundary/handle_table.cpp
    src/c_boundary/amio_strerror.cpp
    src/staging/staging_pool.cpp
    src/workers/worker_pool.cpp
    src/workers/thread_pinning.cpp
    src/workers/comm_split.cpp
    src/config/config_loader.cpp
    src/factory/backend_factory.cpp
)
add_library(AMIO::amio_core ALIAS amio_core)

# `amio_api.cpp` checks `AMIO_BUILDING_LIBRARY` so the AMIO_API macro
# emits `__declspec(dllexport)` on Windows when libamio itself is
# being built.  On ELF / Mach-O targets visibility is governed by
# `__attribute__((visibility("default")))` and this define is a
# harmless no-op.
target_compile_definitions(amio_core PRIVATE AMIO_BUILDING_LIBRARY=1)

# When eckit is part of the build closure, expose AMIO_HAS_ECKIT to
# the C-Boundary translation unit so it can install a dedicated
# `catch (eckit::Exception &)` arm ahead of the generic
# `std::exception` handler (R12.2).  When eckit is absent the
# `std::exception` handler still covers eckit::Exception by
# inheritance, so correctness is preserved either way.
if(eckit_FOUND)
    target_compile_definitions(amio_core PRIVATE AMIO_HAS_ECKIT=1)
endif()

set_target_properties(amio_core PROPERTIES
    OUTPUT_NAME            amio
    VERSION                ${PROJECT_VERSION}
    SOVERSION              ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON
    # All exported symbols are the C99 `amio_*` entry points; no C++
    # mangled names should appear in the dynamic symbol table (task
    # 1.4 verifies via `nm -D`).
    C_VISIBILITY_PRESET    hidden
    CXX_VISIBILITY_PRESET  hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(amio_core
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# PUBLIC link to the public-headers target so that downstream
# consumers transitively pick up the C99 include path.  Every other
# dependency is PRIVATE so its headers do not leak (R13.6).
target_link_libraries(amio_core
    PUBLIC
        amio_public_headers
    PRIVATE
        driver_netcdf
        driver_zarr
        driver_grib2
)
if(eckit_FOUND)
    target_link_libraries(amio_core PRIVATE eckit)
endif()
if(TARGET std::mdspan)
    target_link_libraries(amio_core PRIVATE std::mdspan)
elseif(TARGET mdspan::mdspan)
    target_link_libraries(amio_core PRIVATE mdspan::mdspan)
endif()
if(MPI_CXX_FOUND)
    target_link_libraries(amio_core PRIVATE MPI::MPI_CXX)
endif()

# Force the linker to include all object files from the static driver
# archives so their static-init factory registrations (task 6.2) fire
# at libamio load time.  Without --whole-archive (or equivalent),
# unused-from-amio_core objects would be stripped.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    target_link_options(amio_core PRIVATE
        "LINKER:--whole-archive"
        $<TARGET_FILE:driver_netcdf>
        $<TARGET_FILE:driver_zarr>
        $<TARGET_FILE:driver_grib2>
        "LINKER:--no-whole-archive"
    )
endif()

####################################################################
# Target 3: amio_fortran (STATIC)
#
# The Fortran iso_c_binding wrapper module.  Depends ONLY on
# amio_public_headers (for the C99 declarations the interface block
# mirrors) and on libamio.so's `extern "C"` symbols at link time.  No
# C++ symbol or header is reachable from this target.
####################################################################

add_library(amio_fortran STATIC
    fortran/amio_mod_stub.f90
)
add_library(AMIO::amio_fortran ALIAS amio_fortran)

set_target_properties(amio_fortran PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    Fortran_MODULE_DIRECTORY  ${CMAKE_CURRENT_BINARY_DIR}/fortran/modules
)

target_include_directories(amio_fortran
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/fortran/modules>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/amio/fortran>
)

# Link against amio_public_headers for the C99 declaration set, and
# against amio_core for the runtime symbols `bind(C, name="amio_*")`
# resolves to.  No third-party C++ dependency is referenced here -
# task 1.4's symbol-mangling check ensures only `amio_*` symbols are
# pulled in.
target_link_libraries(amio_fortran
    PUBLIC
        amio_public_headers
        amio_core
)

####################################################################
# Subdirectories
####################################################################

if(AMIO_BUILD_TESTING)
    add_subdirectory(tests)
endif()

####################################################################
# Install + export
#
# Produces:
#   * libamio.so + AMIO::amio_core imported target
#   * amio_public_headers INTERFACE target (no artifact, just headers)
#   * amio_fortran static archive + .mod files
#   * AMIOConfig.cmake + AMIOConfigVersion.cmake + AMIOTargets.cmake
#
# Backend driver static archives (`driver_netcdf`, `driver_zarr`,
# `driver_grib2`) are PRIVATE to libamio and are NOT installed - they
# are not part of AMIO's public coupling surface (R4.8).  Likewise,
# none of eckit / TensorStore / netCDF / g2c are exported as transitive
# requirements of AMIOConfig.cmake (R13.6).
####################################################################

install(DIRECTORY include/amio
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(TARGETS
        amio_public_headers
        amio_core
        amio_fortran
    EXPORT AMIOTargets
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/fortran/modules/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/amio/fortran
    FILES_MATCHING PATTERN "*.mod"
)

install(EXPORT AMIOTargets
    FILE        AMIOTargets.cmake
    NAMESPACE   AMIO::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AMIO
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/AMIOConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AMIOConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AMIO
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/AMIOConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/AMIOConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/AMIOConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AMIO
)

####################################################################
# Configure-time summary
#
# Includes the Zarr-backend line in the format documented in
# design.md ("TensorStore discovery and NCZarr fallback (configure-
# time)"), so configure logs are immediately greppable for which Zarr
# path is active and -- when in fallback -- why (R13.4).
####################################################################

message(STATUS "")
message(STATUS "AMIO ${PROJECT_VERSION} configuration summary")
message(STATUS "  C++ standard          : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  C   standard          : C${CMAKE_C_STANDARD}")
message(STATUS "  Build testing         : ${AMIO_BUILD_TESTING}")
message(STATUS "  TensorStore available : ${AMIO_HAS_TENSORSTORE}")
message(STATUS "  Force NCZarr fallback : ${AMIO_FORCE_NCZARR}")
message(STATUS "  Install prefix        : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "AMIO Zarr backend: ${AMIO_ZARR_BACKEND_DESC}")
if(AMIO_ZARR_BACKEND_REASON)
    message(STATUS "   reason: ${AMIO_ZARR_BACKEND_REASON}")
endif()
message(STATUS "")
