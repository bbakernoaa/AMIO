// test_netcdf_describe_thread_safety.cpp -- regression test for the
// describe_variable() serialization hole.
//
// The bug: every NetCDF_Driver entry point serializes netCDF/HDF5 calls
// through the file-scope g_nc_driver_mutex -- except describe_variable(),
// which issued nc_inq_* on the shared ncid_ WITHOUT the mutex.  In real
// deployments describe_variable() runs on the amio_read() caller thread
// (amio_core resolve_variable) concurrently with Worker_Pool /
// PrefetchQueue reads, so with amio_worker_threads >= 2 (and
// intermittently even at 1, where the caller thread races the single
// worker) the unserialized cross-thread netCDF/HDF5 use corrupts the
// non-thread-safe HDF5 build's global state -- observed as SIGSEGV during
// ingest or heap corruption ("double free or corruption") at shutdown.
//
// This test reproduces the race directly at driver level: one thread
// hammers full reads (nc_get_vara under the mutex) while two threads
// hammer describe_variable() on the same open driver.  WITHOUT the
// lock_guard in describe_variable() the process crashes (segfault/abort,
// which ctest reports as failure); WITH the lock the loops complete and
// every describe returns consistent metadata.
//
// Environment note: provoking the crash requires an HDF5 build without
// thread safety (e.g. Debian's parallel/openmpi flavor, the dev-container
// default).  Against a thread-safe HDF5 the pre-fix code may pass -- the
// lock is still required (it also guards the driver's own state against
// concurrent close/describe), the crash is just this test's strongest
// observable signal.
//
// Mirrors test_read_netcdf4's structure (driver compiled directly into
// the test binary) but uses conf::Config::from_string, so it needs no
// eckit and runs in environments where unit.read_netcdf4 is not built.

#include <mpi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <conf/config.hpp>

#include "drivers/netcdf/netcdf_driver.hpp"
#include "factory/backend_driver.hpp"
#include "staging/staging_pool.hpp"

// backend_factory.cpp force-registers all three backend drivers; only the
// netcdf driver is compiled into this test binary, so stub the others.
extern "C" {
void amio_register_zarr_driver() {}
void amio_register_grib2_driver() {}
}

// Normally defined by amio_core (not compiled into this test binary); the
// driver only consults it for parallel-communicator fallback.
MPI_Comm g_amio_parent_comm = MPI_COMM_NULL;

using amio::detail::NetCDF_Driver;
using amio::detail::StagingBuffer;
using amio::detail::VariableInfo;
using amio::detail::VarMeta;

namespace {

// Meaty enough that each nc_get_vara spends real time inside HDF5,
// maximizing the cross-thread overlap window.
constexpr int NY = 256;
constexpr int NX = 256;
constexpr int kDescribeIterations = 2000;
constexpr int kDescribeThreads = 2;

std::atomic<int> g_describe_failures{0};
std::atomic<int> g_read_failures{0};
std::atomic<bool> g_describers_done{false};

void describe_loop(NetCDF_Driver *reader, const std::string &var_name) {
    for (int i = 0; i < kDescribeIterations; ++i) {
        VariableInfo info = reader->describe_variable(var_name);
        if (!info.found || info.shape.rank != 2 || info.shape.extents[0] != NY || info.shape.extents[1] != NX) {
            ++g_describe_failures;
        }
        VariableInfo missing = reader->describe_variable("does_not_exist");
        if (missing.found) {
            ++g_describe_failures;
        }
    }
}

void read_loop(NetCDF_Driver *reader, const VarMeta &meta) {
    std::vector<float> out(static_cast<std::size_t>(NY) * NX, 0.0f);
    while (!g_describers_done.load(std::memory_order_relaxed)) {
        StagingBuffer dst{};
        dst.data = reinterpret_cast<std::byte *>(out.data());
        dst.capacity_bytes = out.size() * sizeof(float);
        dst.used_bytes = 0;
        try {
            reader->read(dst, meta, /*timestep=*/0, std::nullopt);
        } catch (const std::exception &) {
            ++g_read_failures;
            return;
        }
        if (dst.used_bytes != out.size() * sizeof(float)) {
            ++g_read_failures;
        }
    }
}

}  // namespace

int main() {
    const char *OUTPUT_PATH = "/tmp/amio_test_describe_thread_safety.nc";
    const std::string var_name = "temperature";

    int mpi_already = 0;
    MPI_Initialized(&mpi_already);
    if (!mpi_already) {
        int provided = 0;
        MPI_Init_thread(nullptr, nullptr, MPI_THREAD_MULTIPLE, &provided);
    }

    std::remove(OUTPUT_PATH);

    conf::Config cfg = conf::Config::from_string(std::string("path: ") + OUTPUT_PATH +
                                                 "\n"
                                                 "data_model: classic\n");

    VarMeta meta{};
    meta.name = var_name;
    meta.dtype = AMIO_DTYPE_F32;
    meta.shape.rank = 2;
    meta.shape.extents[0] = NY;
    meta.shape.extents[1] = NX;

    // ---- Write a real file for the race to read ----
    std::vector<float> source(static_cast<std::size_t>(NY) * NX);
    for (int i = 0; i < NY; ++i) {
        for (int j = 0; j < NX; ++j) {
            source[static_cast<std::size_t>(i) * NX + j] = static_cast<float>(i) * 1000.0f + static_cast<float>(j);
        }
    }
    {
        NetCDF_Driver writer;
        writer.open_write(cfg);
        StagingBuffer buf{};
        buf.data = reinterpret_cast<std::byte *>(source.data());
        buf.capacity_bytes = source.size() * sizeof(float);
        buf.used_bytes = source.size() * sizeof(float);
        writer.write(buf, meta);
        writer.flush();
        writer.close();
    }

    // ---- The race: reads under the mutex vs describe_variable ----
    NetCDF_Driver reader;
    reader.open_read(cfg);

    std::thread read_thread(read_loop, &reader, std::cref(meta));
    std::vector<std::thread> describers;
    describers.reserve(kDescribeThreads);
    for (int t = 0; t < kDescribeThreads; ++t) {
        describers.emplace_back(describe_loop, &reader, std::cref(var_name));
    }
    for (auto &thread : describers) {
        thread.join();
    }
    g_describers_done.store(true, std::memory_order_relaxed);
    read_thread.join();
    reader.close();

    const int describe_failures = g_describe_failures.load();
    const int read_failures = g_read_failures.load();
    std::printf("describe iterations: %d x %d threads, describe failures: %d, read failures: %d\n",
                kDescribeIterations, kDescribeThreads, describe_failures, read_failures);

    std::remove(OUTPUT_PATH);
    if (describe_failures != 0 || read_failures != 0) {
        std::fprintf(stderr, "FAIL: concurrent describe/read produced inconsistent results\n");
        return 1;
    }
    std::printf("PASS: describe_variable serialized correctly against concurrent reads\n");
    return 0;
}
