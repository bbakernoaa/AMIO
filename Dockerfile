# syntax=docker/dockerfile:1

# ===================================================================
# AMIO multi-stage build
#
# Stages:
#   deps    - Ubuntu 24.04 + full dependency stack
#             (ecbuild, eckit, mdspan, JasPer, nceplibs-g2c, Catch2,
#              RapidCheck) plus system-packaged netCDF-c/NCZarr, HDF5-MPI.
#   build   - configure + compile + test + install AMIO into /opt/amio.
#   runtime - slim image carrying only the installed library and the
#             shared runtime dependencies needed to load it.
#
# Zarr is built in NCZarr fallback mode (AMIO_FORCE_NCZARR=ON) so the
# image does not pay TensorStore's ~30min build cost. Flip the
# AMIO_HAS_TENSORSTORE / AMIO_FORCE_NCZARR build args to change this.
#
# Build the runtime image:
#   docker build -t amio:latest .
# Build & run the test stage only:
#   docker build --target build -t amio:build .
# ===================================================================

# ===================================================================
# Stage 1: deps - dependency stack
# ===================================================================
FROM ubuntu:24.04 AS deps

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8

# Core build tools, compilers, and system-packaged dependencies.
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

# ecbuild (ECMWF CMake macros - required by eckit).
RUN git clone --depth 1 --branch 3.8.5 https://github.com/ecmwf/ecbuild.git /tmp/ecbuild-src \
    && mkdir /tmp/ecbuild-build && cd /tmp/ecbuild-build \
    && cmake /tmp/ecbuild-src -GNinja -DCMAKE_INSTALL_PREFIX=/opt/ecbuild \
    && ninja install \
    && rm -rf /tmp/ecbuild-src /tmp/ecbuild-build

ENV PATH="/opt/ecbuild/bin:${PATH}"

# eckit (ECMWF toolkit - built with ecbuild).
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

# kokkos/mdspan (header-only).
RUN git clone --depth 1 --branch mdspan-0.6.0 https://github.com/kokkos/mdspan.git /tmp/mdspan-src \
    && mkdir /tmp/mdspan-build && cd /tmp/mdspan-build \
    && cmake /tmp/mdspan-src -GNinja \
    -DCMAKE_INSTALL_PREFIX=/opt/mdspan \
    -DMDSPAN_ENABLE_TESTS=OFF \
    -DMDSPAN_ENABLE_BENCHMARKS=OFF \
    && ninja install \
    && rm -rf /tmp/mdspan-src /tmp/mdspan-build

# JasPer (JPEG2000 library - required by nceplibs-g2c for GRIB2).
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

# nceplibs-g2c (GRIB2 C library).
RUN git clone --depth 1 --branch v1.9.0 https://github.com/NOAA-EMC/NCEPLIBS-g2c.git /tmp/g2c-src \
    && mkdir /tmp/g2c-build && cd /tmp/g2c-build \
    && cmake /tmp/g2c-src -GNinja \
    -DCMAKE_INSTALL_PREFIX=/opt/g2c \
    -DCMAKE_PREFIX_PATH=/opt/jasper \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    && ninja -j$(nproc) && ninja install \
    && rm -rf /tmp/g2c-src /tmp/g2c-build

# Catch2 v3 (test framework).
RUN git clone --depth 1 --branch v3.5.2 https://github.com/catchorg/Catch2.git /tmp/catch2-src \
    && mkdir /tmp/catch2-build && cd /tmp/catch2-build \
    && cmake /tmp/catch2-src -GNinja \
    -DCMAKE_INSTALL_PREFIX=/opt/catch2 \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    && ninja -j$(nproc) && ninja install \
    && rm -rf /tmp/catch2-src /tmp/catch2-build

# RapidCheck (property-based testing).
RUN git clone --depth 1 https://github.com/emil-e/rapidcheck.git /tmp/rc-src \
    && mkdir /tmp/rc-build && cd /tmp/rc-build \
    && cmake /tmp/rc-src -GNinja \
    -DCMAKE_INSTALL_PREFIX=/opt/rapidcheck \
    -DCMAKE_BUILD_TYPE=Release \
    -DRC_ENABLE_CATCH=OFF \
    -DRC_ENABLE_TESTS=OFF \
    && ninja -j$(nproc) && ninja install \
    && rm -rf /tmp/rc-src /tmp/rc-build

# HELM Micro-libraries (configuration and MPI and logging)
RUN git clone --depth 1 https://github.com/bbakerNOAA/HELM-Project.git /tmp/helm-src \
    && for project in conf logs halo; do \
      echo ${project} \
      && mkdir /tmp/helm-${project}-build \
      && cmake -S /tmp/helm-src/libs/${project} -B /tmp/helm-${project}-build -GNinja \
         -DCMAKE_INSTALL_PREFIX=/opt/helm \
         -DCMAKE_BUILD_TYPE=Release \
      && cmake --build /tmp/helm-${project}-build \
      && cmake --install /tmp/helm-${project}-build --prefix /opt/helm \
      && rm -rf /tmp/helm-${project}-build ; \
    done \
    && rm -rf /tmp/helm-src \
    && ls -R /opt/helm

ENV CMAKE_PREFIX_PATH="/opt/ecbuild;/opt/eckit;/opt/mdspan;/opt/jasper;/opt/g2c;/opt/catch2;/opt/rapidcheck;/opt/helm"
ENV LD_LIBRARY_PATH="/opt/eckit/lib:/opt/jasper/lib:/opt/g2c/lib:/opt/helm/lib"

# ===================================================================
# Stage 2: build - configure, compile, test, and install AMIO
# ===================================================================
FROM deps AS build

# Build configuration (override with --build-arg).
ARG AMIO_BUILD_TYPE=Release
ARG AMIO_HAS_TENSORSTORE=OFF
ARG AMIO_FORCE_NCZARR=ON
ARG AMIO_BUILD_TESTING=ON
ARG AMIO_BUILD_EXAMPLES=ON
ARG RUN_TESTS=1

WORKDIR /src
COPY . /src

RUN cmake -S /src -B /src/build -GNinja \
    -DCMAKE_BUILD_TYPE=${AMIO_BUILD_TYPE} \
    -DCMAKE_INSTALL_PREFIX=/opt/amio \
    -DAMIO_HAS_ECKIT=ON \
    -DAMIO_HAS_NETCDF=ON \
    -DAMIO_HAS_G2C=ON \
    -DAMIO_HAS_TENSORSTORE=${AMIO_HAS_TENSORSTORE} \
    -DAMIO_FORCE_NCZARR=${AMIO_FORCE_NCZARR} \
    -DAMIO_BUILD_TESTING=${AMIO_BUILD_TESTING} \
    -DAMIO_BUILD_EXAMPLES=${AMIO_BUILD_EXAMPLES}

RUN cmake --build /src/build -j"$(nproc)"

# Run the test suite during the image build unless RUN_TESTS=0.
RUN if [ "${RUN_TESTS}" = "1" ] && [ "${AMIO_BUILD_TESTING}" = "ON" ]; then \
        ctest --test-dir /src/build --output-on-failure; \
    else \
        echo "Skipping tests (RUN_TESTS=${RUN_TESTS}, AMIO_BUILD_TESTING=${AMIO_BUILD_TESTING})"; \
    fi

RUN cmake --install /src/build

# ===================================================================
# Stage 3: runtime - slim image with only the installed library
# ===================================================================
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8

# Shared runtime libraries required to load libamio.so (no -dev packages,
# no compilers). netCDF/HDF5-MPI and codec libs are pulled in because the
# NetCDF and NCZarr-fallback drivers link them; g2c/jasper come from the
# build stage's /opt prefixes.
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgfortran5 \
    libopenmpi3 openmpi-bin \
    libhdf5-103-1 libhdf5-hl-100 \
    libnetcdf19 libnetcdf-c++4 \
    libblosc1 libzstd1 libaec0 \
    libcurl4 zlib1g \
    libpng16-16 libjpeg-turbo8 libxml2 \
    && rm -rf /var/lib/apt/lists/*

# AMIO install tree (libamio.so, headers, Fortran .mod, CMake package).
COPY --from=build /opt/amio /opt/amio

# Source-built shared libraries that libamio.so depends on at load time.
COPY --from=build /opt/eckit/lib /opt/eckit/lib
COPY --from=build /opt/jasper/lib /opt/jasper/lib
COPY --from=build /opt/g2c/lib /opt/g2c/lib

ENV LD_LIBRARY_PATH="/opt/amio/lib:/opt/eckit/lib:/opt/jasper/lib:/opt/g2c/lib"
ENV CMAKE_PREFIX_PATH="/opt/amio"

# Register the library paths with the dynamic linker cache too.
RUN printf '%s\n' \
    /opt/amio/lib \
    /opt/eckit/lib \
    /opt/jasper/lib \
    /opt/g2c/lib > /etc/ld.so.conf.d/amio.conf \
    && ldconfig

WORKDIR /work
CMD ["bash"]
