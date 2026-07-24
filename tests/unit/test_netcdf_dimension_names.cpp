// test_netcdf_dimension_names.cpp -- regression test for order-dependent
// dimension naming in the netCDF writer.
//
// The bug: NetCDF_Driver::write defines a variable's dimensions from
// whatever dimensions exist at write time (length matching, then a
// synthetic "<var>_dim<d>" fallback); the canonical axis names only arise
// from the rank-1 lon/lat/lev/time coordinate special case.  With
// amio_worker_threads >= 2 the async write tasks complete in
// nondeterministic order, so a data variable defined before its coordinate
// lands on a synthetic dimension (observed: nox(time, lev, nox_dim2, lon)
// with a detached lat) and the coordinate is never associated.
//
// This test replays that racing interleaving deterministically -- the data
// variable is written FIRST, then the coordinates -- and asserts the file
// still comes out on the canonical named dimensions.  A coordinates-first
// file guards the ordering that already worked.
//
// Mirrors test_netcdf_describe_thread_safety's structure (driver compiled
// directly into the test binary, conf::Config::from_string, no eckit).

#include <mpi.h>
#include <netcdf.h>

#include <cstdio>
#include <string>
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

// Normally defined by amio_core (not compiled into this test binary).
MPI_Comm g_amio_parent_comm = MPI_COMM_NULL;

using amio::detail::NetCDF_Driver;
using amio::detail::StagingBuffer;
using amio::detail::VarMeta;

namespace {

constexpr int NT = 1;
constexpr int NZ = 1;
constexpr int NY = 6;
constexpr int NX = 8;

int g_failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

VarMeta rank1_meta(const std::string &name, int64_t len) {
    VarMeta meta{};
    meta.name = name;
    meta.dtype = AMIO_DTYPE_F64;
    meta.shape.rank = 1;
    meta.shape.extents[0] = len;
    return meta;
}

void write_var(NetCDF_Driver &writer, const VarMeta &meta, std::vector<double> &values) {
    StagingBuffer buf{};
    buf.data = reinterpret_cast<std::byte *>(values.data());
    buf.capacity_bytes = values.size() * sizeof(double);
    buf.used_bytes = values.size() * sizeof(double);
    writer.write(buf, meta);
}

// Write one file: the rank-4 data variable and the four coordinate
// variables, in either order.
void write_file(const std::string &path, bool data_variable_first) {
    conf::Config cfg = conf::Config::from_string(std::string("path: ") + path +
                                                 "\n"
                                                 "data_model: classic\n");
    NetCDF_Driver writer;
    writer.open_write(cfg);

    VarMeta field = {};
    field.name = "nox";
    field.dtype = AMIO_DTYPE_F64;
    field.shape.rank = 4;
    field.shape.extents[0] = NT;
    field.shape.extents[1] = NZ;
    field.shape.extents[2] = NY;
    field.shape.extents[3] = NX;
    std::vector<double> field_values(static_cast<std::size_t>(NT) * NZ * NY * NX, 1.5);

    std::vector<double> lon(NX, 0.0), lat(NY, 0.0), lev(NZ, 0.0), time_value(NT, 0.0);

    auto write_coordinates = [&]() {
        write_var(writer, rank1_meta("lon", NX), lon);
        write_var(writer, rank1_meta("lat", NY), lat);
        write_var(writer, rank1_meta("lev", NZ), lev);
        write_var(writer, rank1_meta("time", NT), time_value);
    };

    if (data_variable_first) {
        write_var(writer, field, field_values);
        write_coordinates();
    } else {
        write_coordinates();
        write_var(writer, field, field_values);
    }
    writer.flush();
    writer.close();
}

// Assert the postconditions with the raw netCDF C API: nox on exactly
// (time, lev, lat, lon), no synthetic *_dim* dimensions, every coordinate
// variable bound to its same-named dimension, time the record dimension.
void check_file(const std::string &path, const std::string &label) {
    int ncid = -1;
    expect(nc_open(path.c_str(), NC_NOWRITE, &ncid) == NC_NOERR, label + ": nc_open");

    int varid = -1;
    expect(nc_inq_varid(ncid, "nox", &varid) == NC_NOERR, label + ": nox present");
    int ndims = 0;
    int dimids[NC_MAX_VAR_DIMS] = {0};
    expect(nc_inq_varndims(ncid, varid, &ndims) == NC_NOERR && ndims == 4,
           label + ": nox is rank 4");
    expect(nc_inq_vardimid(ncid, varid, dimids) == NC_NOERR, label + ": nox dimids");
    const char *expected[4] = {"time", "lev", "lat", "lon"};
    for (int d = 0; d < 4; ++d) {
        char name[NC_MAX_NAME + 1] = {0};
        expect(nc_inq_dimname(ncid, dimids[d], name) == NC_NOERR,
               label + ": dim name " + std::to_string(d));
        expect(std::string(name) == expected[d],
               label + ": nox dim " + std::to_string(d) + " is '" + name + "', expected '" +
                   expected[d] + "'");
    }

    int ndims_total = 0;
    expect(nc_inq_ndims(ncid, &ndims_total) == NC_NOERR, label + ": nc_inq_ndims");
    for (int i = 0; i < ndims_total; ++i) {
        char name[NC_MAX_NAME + 1] = {0};
        expect(nc_inq_dimname(ncid, i, name) == NC_NOERR, label + ": dim listing");
        expect(std::string(name).find("_dim") == std::string::npos,
               label + ": synthetic dimension '" + name + "' present");
    }

    for (const char *coordinate : {"lon", "lat", "lev", "time"}) {
        int coord_varid = -1;
        expect(nc_inq_varid(ncid, coordinate, &coord_varid) == NC_NOERR,
               label + ": coordinate variable " + coordinate + " present");
        int coord_ndims = 0;
        int coord_dimid = -1;
        if (nc_inq_varndims(ncid, coord_varid, &coord_ndims) == NC_NOERR && coord_ndims == 1 &&
            nc_inq_vardimid(ncid, coord_varid, &coord_dimid) == NC_NOERR) {
            char name[NC_MAX_NAME + 1] = {0};
            expect(nc_inq_dimname(ncid, coord_dimid, name) == NC_NOERR &&
                       std::string(name) == coordinate,
                   label + ": " + coordinate + " bound to its own dimension");
        } else {
            expect(false, label + ": " + coordinate + " is rank 1");
        }
    }

    int unlimited_dimid = -1;
    expect(nc_inq_unlimdim(ncid, &unlimited_dimid) == NC_NOERR && unlimited_dimid >= 0,
           label + ": has a record dimension");
    if (unlimited_dimid >= 0) {
        char name[NC_MAX_NAME + 1] = {0};
        expect(nc_inq_dimname(ncid, unlimited_dimid, name) == NC_NOERR &&
                   std::string(name) == "time",
               label + ": time is the record dimension");
    }

    nc_close(ncid);
}

}  // namespace

int main() {
    int mpi_already = 0;
    MPI_Initialized(&mpi_already);
    if (!mpi_already) {
        int provided = 0;
        MPI_Init_thread(nullptr, nullptr, MPI_THREAD_MULTIPLE, &provided);
    }

    const char *DATA_FIRST_PATH = "/tmp/amio_test_dimension_names_data_first.nc";
    const char *COORDS_FIRST_PATH = "/tmp/amio_test_dimension_names_coords_first.nc";
    std::remove(DATA_FIRST_PATH);
    std::remove(COORDS_FIRST_PATH);

    // The racing interleaving, replayed deterministically.
    write_file(DATA_FIRST_PATH, /*data_variable_first=*/true);
    check_file(DATA_FIRST_PATH, "data-first");

    // The conventional order must keep working.
    write_file(COORDS_FIRST_PATH, /*data_variable_first=*/false);
    check_file(COORDS_FIRST_PATH, "coords-first");

    std::remove(DATA_FIRST_PATH);
    std::remove(COORDS_FIRST_PATH);

    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d dimension-naming expectation(s) violated\n", g_failures);
        return 1;
    }
    std::printf("PASS: variables land on canonical dimensions in either write order\n");
    return 0;
}
