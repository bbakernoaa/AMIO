# AMIO Development Container (full integration build)
#
# Builds the complete AMIO dependency stack:
#   ecbuild → eckit → kokkos/mdspan → nceplibs-g2c → TensorStore
#   → Catch2 → RapidCheck
#
# netCDF-c (with NCZarr, parallel HDF5) and netCDF-cxx4 come from
# Ubuntu 24.04 system packages (netcdf-c 4.9.2 has NCZarr built-in).
#
# TensorStore is built from source via CMake (heavy, ~30min).

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8

# ===================================================================
# Core build tools, compilers, and system-packaged dependencies
# ===================================================================
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gfortran \
    cmake \
    ninja-build \
    git \
    curl \
    wget \
    pkg-config \
    ca-certificates \
    python3 python3-pip python3-venv \
    nasm \
    libopenmpi-dev openmpi-bin \
    libhdf5-mpi-dev \
    libnetcdf-dev libnetcdf-mpi-dev \
    libnetcdf-c++4-dev \
    libblosc-dev libzstd-dev libaec-dev \
    libcurl4-openssl-dev libssl-dev zlib1g-dev \
    libpng-dev libjpeg-dev libxml2-dev \
    && rm -rf /var/lib/apt/lists/*

ENV CC=gcc
ENV CXX=g++
ENV FC=gfortran

# ===================================================================
# ecbuild (ECMWF CMake macros - required by eckit)
# ===================================================================
RUN git clone --depth 1 --branch 3.8.5 https://github.com/ecmwf/ecbuild.git /tmp/ecbuild-src \
    && mkdir /tmp/ecbuild-build && cd /tmp/ecbuild-build \
    && cmake /tmp/ecbuild-src -GNinja -DCMAKE_INSTALL_PREFIX=/opt/ecbuild \
    && ninja install \
    && rm -rf /tmp/ecbuild-src /tmp/ecbuild-build

ENV PATH="/opt/ecbuild/bin:${PATH}"

# ===================================================================
# eckit (ECMWF toolkit - built with ecbuild)
# ===================================================================
RUN git clone --depth 1 --branch 1.26.2 https://github.com/ecmwf/eckit.git /tmp/eckit-src \
    && mkdir /tmp/eckit-build && cd /tmp/eckit-build \
    && cmake /tmp/eckit-src -GNinja \
    -DCMAKE_INSTALL_PREFIX=/opt/eckit \
    -DCMAKE_PREFIX_PATH=/opt/ecbuild \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_MPI=ON \
    -DENABLE_TESTS=OFF \
    -DENABLE_ECKIT_SQL=OFF \
    -DENABLE_ECKIT_CMD=OFF \
    -DENABLE_BZIP2=OFF \
    -DENABLE_CURL=OFF \
    -DENABLE_JEMALLOC=OFF \
    -DENABLE_LZ4=OFF \
    -DENABLE_SNAPPY=OFF \
    -DENABLE_AEC=OFF \
    && ninja -j$(nproc) && ninja install \
    && rm -rf /tmp/eckit-src /tmp/eckit-build

# ===================================================================
# kokkos/mdspan (header-only)
# ===================================================================
RUN git clone --depth 1 --branch mdspan-0.6.0 https://github.com/kokkos/mdspan.git /tmp/mdspan-src \
    && mkdir /tmp/mdspan-build && cd /tmp/mdspan-build \
    && cmake /tmp/mdspan-src -GNinja \
    -DCMAKE_INSTALL_PREFIX=/opt/mdspan \
    -DMDSPAN_ENABLE_TESTS=OFF \
    -DMDSPAN_ENABLE_BENCHMARKS=OFF \
    && ninja install \
    && rm -rf /tmp/mdspan-src /tmp/mdspan-build

# ===================================================================
# JasPer (JPEG2000 library - required by nceplibs-g2c for GRIB2)
# Not available as a system package on Ubuntu 24.04.
# ===================================================================
RUN git clone --depth 1 --branch version-4.2.4 https://github.com/jasper-software/jasper.git /tmp/jasper-src \
    && mkdir /tmp/jasper-build && cd /tmp/jasper-build \
    && cmake /tmp/jasper-src -GNinja \
    -DCMAKE_INSTALL_PREFIX=/opt/jasper \
    -DCMAKE_BUILD_TYPE=Release \
    -DJAS_ENABLE_PROGRAMS=OFF \
    -DJAS_ENABLE_DOC=OFF \
    -DBUILD_TESTING=OFF \
    && ninja -j$(nproc) && ninja install \
    && rm -rf /tmp/jasper-src /tmp/jasper-build

# ===================================================================
# nceplibs-g2c (GRIB2 C library)
# ===================================================================
RUN git clone --depth 1 --branch v1.9.0 https://github.com/NOAA-EMC/NCEPLIBS-g2c.git /tmp/g2c-src \
    && mkdir /tmp/g2c-build && cd /tmp/g2c-build \
    && cmake /tmp/g2c-src -GNinja \
    -DCMAKE_INSTALL_PREFIX=/opt/g2c \
    -DCMAKE_PREFIX_PATH=/opt/jasper \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    && ninja -j$(nproc) && ninja install \
    && rm -rf /tmp/g2c-src /tmp/g2c-build

# ===================================================================
# TensorStore
#
# TensorStore is NOT pre-built in this container due to its extreme
# build time and memory requirements (especially on ARM64).
#
# Instead, AMIO integrates TensorStore via CMake FetchContent when
# -DAMIO_HAS_TENSORSTORE=ON is passed at AMIO configure time.
# The first AMIO build with TensorStore enabled will be slow (~30min)
# but subsequent builds are incremental.
#
# For development without TensorStore, use -DAMIO_FORCE_NCZARR=ON
# which routes all Zarr I/O through the NCZarr fallback (system netCDF).
# ===================================================================

# ===================================================================
# Catch2 v3 (test framework)
# ===================================================================
RUN git clone --depth 1 --branch v3.5.2 https://github.com/catchorg/Catch2.git /tmp/catch2-src \
    && mkdir /tmp/catch2-build && cd /tmp/catch2-build \
    && cmake /tmp/catch2-src -GNinja \
    -DCMAKE_INSTALL_PREFIX=/opt/catch2 \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    && ninja -j$(nproc) && ninja install \
    && rm -rf /tmp/catch2-src /tmp/catch2-build

# ===================================================================
# RapidCheck (property-based testing)
# ===================================================================
RUN git clone --depth 1 https://github.com/emil-e/rapidcheck.git /tmp/rc-src \
    && mkdir /tmp/rc-build && cd /tmp/rc-build \
    && cmake /tmp/rc-src -GNinja \
    -DCMAKE_INSTALL_PREFIX=/opt/rapidcheck \
    -DCMAKE_BUILD_TYPE=Release \
    -DRC_ENABLE_CATCH=OFF \
    -DRC_ENABLE_TESTS=OFF \
    && ninja -j$(nproc) && ninja install \
    && rm -rf /tmp/rc-src /tmp/rc-build

# ===================================================================
# Environment
# ===================================================================
ENV CMAKE_PREFIX_PATH="/opt/ecbuild;/opt/eckit;/opt/mdspan;/opt/jasper;/opt/g2c;/opt/catch2;/opt/rapidcheck"
ENV LD_LIBRARY_PATH="/opt/eckit/lib:/opt/jasper/lib:/opt/g2c/lib"
ENV PATH="/opt/ecbuild/bin:${PATH}"

# Verify NCZarr support in system netCDF
RUN nc-config --has-nczarr || echo "WARNING: system netCDF may lack NCZarr"

WORKDIR /workspace
