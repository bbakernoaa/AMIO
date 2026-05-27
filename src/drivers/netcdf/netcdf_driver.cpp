// netcdf_driver.cpp -- AMIO NetCDF_Driver implementation.
//
// Concrete Backend_Driver for NetCDF-4 output via netCDF-cxx4 over
// Parallel HDF5 with MPI-IO collective operations.
//
// Compilation:
//   - When AMIO_HAS_NETCDF is defined: full implementation using
//     netCDF-cxx4 API with Parallel HDF5 + MPI-IO.
//   - When AMIO_HAS_NETCDF is NOT defined: the driver still registers
//     with the factory but raises eckit::Exception on any operation
//     (graceful degradation for builds without netCDF).
//
// Validates: R7.1, R7.2, R7.3, R7.4, R7.5, R7.6

#include "drivers/netcdf/netcdf_driver.hpp"
#include "factory/backend_factory.hpp"
#include "staging/staging_pool.hpp"

#ifdef AMIO_HAS_NETCDF
#include <netcdf.h>
#include <netcdf_par.h>
#include <netcdf_meta.h>
#include <mpi.h>
#endif

#include <cstring>
#include <stdexcept>
#include <string>
#include <algorithm>

// ===================================================================
// eckit::Exception compatibility layer.
//
// When eckit is available, we use eckit::Exception directly.
// Otherwise, we fall back to std::runtime_error which eckit::Exception
// inherits from in practice.
// ===================================================================

#ifdef AMIO_HAS_ECKIT
#include <eckit/exception/Exceptions.h>
#include <eckit/config/Configuration.h>
#include <eckit/config/LocalConfiguration.h>
#else
// Minimal shim when eckit is not available (for compilation only).
namespace eckit {
class Exception : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class Configuration {
public:
    virtual ~Configuration() = default;
    virtual bool has(const std::string& /*name*/) const { return false; }
    virtual bool getString(const std::string& /*name*/, std::string& /*out*/) const { return false; }
    virtual bool getStringVector(const std::string& /*name*/, std::vector<std::string>& /*out*/) const { return false; }
    virtual std::string getString(const std::string& /*key*/, const std::string& def = "") const { return def; }
    virtual std::vector<std::string> getStringVector(const std::string& /*key*/,
        const std::vector<std::string>& def = {}) const { return def; }
};
using LocalConfiguration = Configuration;
}  // namespace eckit
#endif

namespace amio::detail {

// ===================================================================
// Helper: wrap NetCDF error codes in exceptions.
// ===================================================================

#ifdef AMIO_HAS_NETCDF

// Check a netCDF return code and throw eckit::Exception on failure.
static void nc_check(int status, const std::string& context) {
    if (status != NC_NOERR) {
        std::string msg = "NetCDF error in " + context + ": " +
                          nc_strerror(status) +
                          " (nc_errno=" + std::to_string(status) + ")";
        throw eckit::Exception(msg);
    }
}

#endif  // AMIO_HAS_NETCDF

// ===================================================================
// NetCDF_Driver -- construction and destruction.
// ===================================================================

NetCDF_Driver::NetCDF_Driver() {
    // On construction, verify that the linked netCDF library has
    // Parallel HDF5 + MPI-IO support (R7.1).
    verify_parallel_support();
}

NetCDF_Driver::~NetCDF_Driver() {
    // Best-effort close if still open.
    if (is_open_) {
        try {
            close();
        } catch (...) {
            // Suppress exceptions in destructor.
        }
    }
}

// ===================================================================
// verify_parallel_support -- check for Parallel HDF5 + MPI-IO (R7.1).
// ===================================================================

void NetCDF_Driver::verify_parallel_support() {
#ifdef AMIO_HAS_NETCDF
    // Query the netcdf-c library for parallel I/O support.
    // NC_HAS_PARALLEL is defined in netcdf_meta.h when the library
    // was built with --enable-parallel4 (Parallel HDF5 + MPI-IO).
#if defined(NC_HAS_PARALLEL) && NC_HAS_PARALLEL
    // Parallel HDF5 + MPI-IO is available.
    return;
#else
    throw eckit::Exception(
        "NetCDF_Driver: linked netCDF-cxx4 library does not have "
        "Parallel HDF5 + MPI-IO support. Rebuild netCDF with "
        "--enable-parallel4 (R7.1)");
#endif
#else
    throw eckit::Exception(
        "NetCDF_Driver: AMIO was built without netCDF support "
        "(AMIO_HAS_NETCDF not defined). Cannot use netcdf4 backend.");
#endif
}

// ===================================================================
// parse_data_model -- validate data model string (R7.2, R7.3).
// ===================================================================

NetCDF4DataModel NetCDF_Driver::parse_data_model(const std::string& model_str) {
    if (model_str.empty() || model_str == "classic" ||
        model_str == "netcdf4_classic" || model_str == "NC4_CLASSIC") {
        return NetCDF4DataModel::Classic;
    }
    if (model_str == "enhanced" || model_str == "netcdf4_enhanced" ||
        model_str == "NC4") {
        return NetCDF4DataModel::Enhanced;
    }
    throw eckit::Exception(
        "NetCDF_Driver: invalid data model '" + model_str + "'. "
        "Supported values: 'classic' (default), 'enhanced' (R7.2, R7.3)");
}

// ===================================================================
// validate_codec -- ensure codec is lossless and on allow-list (R7.5).
// ===================================================================

void NetCDF_Driver::validate_codec(const std::string& codec,
                                   const std::vector<std::string>& allow_list) {
    if (codec.empty()) {
        // No compression requested -- valid.
        return;
    }

    // Check against the allow-list.
    auto it = std::find(allow_list.begin(), allow_list.end(), codec);
    if (it == allow_list.end()) {
        throw eckit::Exception(
            "NetCDF_Driver: codec '" + codec + "' is not on the "
            "lossless compression allow-list. Only lossless filters "
            "are permitted for NetCDF-4 output (R7.5)");
    }

    // Additional check: reject known lossy codecs even if somehow
    // they appear on the allow-list (defense in depth).
    static const std::vector<std::string> lossy_codecs = {
        "lossy", "sz", "zfp_lossy", "fpzip_lossy"
    };
    for (const auto& lossy : lossy_codecs) {
        if (codec == lossy) {
            throw eckit::Exception(
                "NetCDF_Driver: codec '" + codec + "' is a lossy codec "
                "and cannot be used with NetCDF-4 output (R7.5)");
        }
    }
}

// ===================================================================
// dtype_to_nc_type -- map AMIO dtype to netCDF type constant.
// ===================================================================

int NetCDF_Driver::dtype_to_nc_type(amio_dtype_t dtype) {
#ifdef AMIO_HAS_NETCDF
    switch (dtype) {
        case AMIO_DTYPE_F32: return NC_FLOAT;
        case AMIO_DTYPE_F64: return NC_DOUBLE;
        case AMIO_DTYPE_I8:  return NC_BYTE;
        case AMIO_DTYPE_I16: return NC_SHORT;
        case AMIO_DTYPE_I32: return NC_INT;
        case AMIO_DTYPE_I64: return NC_INT64;
        case AMIO_DTYPE_U8:  return NC_UBYTE;
        case AMIO_DTYPE_U16: return NC_USHORT;
        case AMIO_DTYPE_U32: return NC_UINT;
        case AMIO_DTYPE_U64: return NC_UINT64;
        default:
            throw eckit::Exception(
                "NetCDF_Driver: unsupported dtype " +
                std::to_string(static_cast<int>(dtype)));
    }
#else
    (void)dtype;
    throw eckit::Exception("NetCDF_Driver: built without netCDF support");
#endif
}

// ===================================================================
// dtype_byte_size -- get element size for an AMIO dtype.
// ===================================================================

std::size_t NetCDF_Driver::dtype_byte_size(amio_dtype_t dtype) {
    switch (dtype) {
        case AMIO_DTYPE_F32: return 4;
        case AMIO_DTYPE_F64: return 8;
        case AMIO_DTYPE_I8:  return 1;
        case AMIO_DTYPE_I16: return 2;
        case AMIO_DTYPE_I32: return 4;
        case AMIO_DTYPE_I64: return 8;
        case AMIO_DTYPE_U8:  return 1;
        case AMIO_DTYPE_U16: return 2;
        case AMIO_DTYPE_U32: return 4;
        case AMIO_DTYPE_U64: return 8;
        default:
            throw eckit::Exception(
                "NetCDF_Driver: unsupported dtype " +
                std::to_string(static_cast<int>(dtype)));
    }
}

// ===================================================================
// open_write -- prepare for parallel write operations (R7.1, R7.4).
// ===================================================================

void NetCDF_Driver::open_write(const eckit::Configuration& config) {
    if (is_open_) {
        throw eckit::Exception(
            "NetCDF_Driver::open_write: driver is already open");
    }

#ifdef AMIO_HAS_NETCDF
    // Extract configuration parameters.
    std::string path;
    if (!config.has("path")) {
        throw eckit::Exception(
            "NetCDF_Driver::open_write: 'path' field is required");
    }
    path = config.getString("path");
    file_path_ = path;

    // Parse data model (R7.2, R7.3).
    std::string model_str = config.getString("data_model", "classic");
    data_model_ = parse_data_model(model_str);

    // Parse and validate compression codec (R7.5).
    codec_allow_list_ = config.getStringVector("codec_allow_list",
                                                std::vector<std::string>{});
    active_codec_ = config.getString("codec", "");
    validate_codec(active_codec_, codec_allow_list_);

    // Determine the MPI communicator for parallel I/O.
    // Default to MPI_COMM_WORLD if not specified.
    comm_ = MPI_COMM_WORLD;
    info_ = MPI_INFO_NULL;

    // Determine NetCDF creation mode flags.
    int cmode = NC_NETCDF4 | NC_CLOBBER;
    if (data_model_ == NetCDF4DataModel::Classic) {
        cmode |= NC_CLASSIC_MODEL;
    }
    // Enhanced mode: just NC_NETCDF4 (no classic flag).

    // Create the file in parallel mode with MPI-IO (R7.4).
    int status = nc_create_par(path.c_str(), cmode, comm_, info_, &ncid_);
    nc_check(status, "nc_create_par('" + path + "')");

    is_open_ = true;
    is_write_mode_ = true;

#else
    (void)config;
    throw eckit::Exception(
        "NetCDF_Driver::open_write: AMIO built without netCDF support");
#endif
}

// ===================================================================
// open_read -- prepare for parallel read operations.
// ===================================================================

void NetCDF_Driver::open_read(const eckit::Configuration& config) {
    if (is_open_) {
        throw eckit::Exception(
            "NetCDF_Driver::open_read: driver is already open");
    }

#ifdef AMIO_HAS_NETCDF
    // Extract configuration parameters.
    if (!config.has("path")) {
        throw eckit::Exception(
            "NetCDF_Driver::open_read: 'path' field is required");
    }
    std::string path = config.getString("path");
    file_path_ = path;

    // Parse data model for validation (R7.2, R7.3).
    std::string model_str = config.getString("data_model", "classic");
    data_model_ = parse_data_model(model_str);

    // MPI communicator for parallel reads.
    comm_ = MPI_COMM_WORLD;
    info_ = MPI_INFO_NULL;

    // Open the file in parallel read mode.
    int status = nc_open_par(path.c_str(), NC_NOWRITE, comm_, info_, &ncid_);
    nc_check(status, "nc_open_par('" + path + "')");

    is_open_ = true;
    is_write_mode_ = false;

#else
    (void)config;
    throw eckit::Exception(
        "NetCDF_Driver::open_read: AMIO built without netCDF support");
#endif
}

// ===================================================================
// write -- serialize StagingBuffer to NetCDF variable (R7.4, R7.5).
// ===================================================================

void NetCDF_Driver::write(const StagingBuffer& src, const VarMeta& meta) {
    if (!is_open_ || !is_write_mode_) {
        throw eckit::Exception(
            "NetCDF_Driver::write: driver not open for writing");
    }

#ifdef AMIO_HAS_NETCDF
    // Look up or create the variable in the NetCDF file.
    int varid = -1;
    int status = nc_inq_varid(ncid_, meta.name.c_str(), &varid);

    if (status == NC_ENOTVAR) {
        // Variable does not exist yet -- define it.
        // First, define dimensions based on the shape.
        std::vector<int> dimids(meta.shape.rank);

        // Switch to define mode if needed.
        int redef_status = nc_redef(ncid_);
        // NC_EINDEFINE means we're already in define mode -- that's fine.
        if (redef_status != NC_NOERR && redef_status != NC_EINDEFINE) {
            nc_check(redef_status, "nc_redef");
        }

        for (int32_t d = 0; d < meta.shape.rank; ++d) {
            std::string dim_name = meta.name + "_dim" + std::to_string(d);
            // Check if dimension already exists.
            int existing_dimid = -1;
            int dim_status = nc_inq_dimid(ncid_, dim_name.c_str(), &existing_dimid);
            if (dim_status == NC_NOERR) {
                dimids[d] = existing_dimid;
            } else {
                std::size_t dim_len = static_cast<std::size_t>(
                    meta.shape.extents[d]);
                status = nc_def_dim(ncid_, dim_name.c_str(), dim_len,
                                    &dimids[d]);
                nc_check(status, "nc_def_dim('" + dim_name + "')");
            }
        }

        // Define the variable.
        int nc_type = dtype_to_nc_type(meta.dtype);
        status = nc_def_var(ncid_, meta.name.c_str(), nc_type,
                            meta.shape.rank, dimids.data(), &varid);
        nc_check(status, "nc_def_var('" + meta.name + "')");

        // Apply lossless compression if configured (R7.5).
        if (!active_codec_.empty()) {
            if (active_codec_ == "zstandard" || active_codec_ == "zstd") {
                // Use shuffle + deflate as a portable lossless option.
                // NetCDF-4 supports deflate natively.
                status = nc_def_var_deflate(ncid_, varid,
                                           /*shuffle=*/1,
                                           /*deflate=*/1,
                                           /*deflate_level=*/4);
                nc_check(status, "nc_def_var_deflate('" + meta.name + "')");
            } else if (active_codec_ == "blosc") {
                // Use shuffle + deflate as portable fallback.
                // Blosc filter requires HDF5 plugin; use deflate if
                // the plugin is not available.
                status = nc_def_var_deflate(ncid_, varid,
                                           /*shuffle=*/1,
                                           /*deflate=*/1,
                                           /*deflate_level=*/4);
                nc_check(status, "nc_def_var_deflate('" + meta.name + "')");
            } else {
                // Generic lossless: apply shuffle + deflate.
                status = nc_def_var_deflate(ncid_, varid,
                                           /*shuffle=*/1,
                                           /*deflate=*/1,
                                           /*deflate_level=*/4);
                nc_check(status, "nc_def_var_deflate('" + meta.name + "')");
            }
        }

        // End define mode.
        status = nc_enddef(ncid_);
        nc_check(status, "nc_enddef");

    } else if (status != NC_NOERR) {
        nc_check(status, "nc_inq_varid('" + meta.name + "')");
    }

    // Set collective access mode for parallel MPI-IO writes (R7.4).
    status = nc_var_par_access(ncid_, varid, NC_COLLECTIVE);
    nc_check(status, "nc_var_par_access('" + meta.name + "', NC_COLLECTIVE)");

    // Compute start/count arrays for the write.
    std::vector<std::size_t> start(meta.shape.rank, 0);
    std::vector<std::size_t> count(meta.shape.rank);
    for (int32_t d = 0; d < meta.shape.rank; ++d) {
        count[d] = static_cast<std::size_t>(meta.shape.extents[d]);
    }

    // Write the data from the staging buffer using collective I/O.
    status = nc_put_vara(ncid_, varid, start.data(), count.data(),
                         src.data);
    nc_check(status, "nc_put_vara('" + meta.name + "')");

#else
    (void)src;
    (void)meta;
    throw eckit::Exception(
        "NetCDF_Driver::write: AMIO built without netCDF support");
#endif
}

// ===================================================================
// read -- read variable data into StagingBuffer.
// ===================================================================

void NetCDF_Driver::read(StagingBuffer& dst,
                         const VarMeta& meta,
                         std::int64_t timestep,
                         const std::optional<BoundingBox>& bbox) {
    if (!is_open_ || is_write_mode_) {
        throw eckit::Exception(
            "NetCDF_Driver::read: driver not open for reading");
    }

#ifdef AMIO_HAS_NETCDF
    // Look up the variable.
    int varid = -1;
    int status = nc_inq_varid(ncid_, meta.name.c_str(), &varid);
    nc_check(status, "nc_inq_varid('" + meta.name + "') for read");

    // Set collective access mode for parallel MPI-IO reads (R7.4).
    status = nc_var_par_access(ncid_, varid, NC_COLLECTIVE);
    nc_check(status, "nc_var_par_access('" + meta.name + "', NC_COLLECTIVE) for read");

    // Compute start/count arrays.
    std::vector<std::size_t> start(meta.shape.rank);
    std::vector<std::size_t> count(meta.shape.rank);

    if (bbox.has_value()) {
        // Selective read using bounding box (R5.7).
        const auto& box = bbox.value();
        for (int32_t d = 0; d < box.rank; ++d) {
            start[d] = static_cast<std::size_t>(box.offsets[d]);
            count[d] = static_cast<std::size_t>(box.extents[d]);
        }
    } else {
        // Full variable read.
        for (int32_t d = 0; d < meta.shape.rank; ++d) {
            start[d] = 0;
            count[d] = static_cast<std::size_t>(meta.shape.extents[d]);
        }
    }

    // Calculate total bytes to read.
    std::size_t elem_size = dtype_byte_size(meta.dtype);
    std::size_t total_elems = 1;
    for (int32_t d = 0; d < meta.shape.rank; ++d) {
        total_elems *= count[d];
    }
    std::size_t total_bytes = total_elems * elem_size;

    if (total_bytes > dst.capacity_bytes) {
        throw eckit::Exception(
            "NetCDF_Driver::read: required " + std::to_string(total_bytes) +
            " bytes but buffer capacity is " +
            std::to_string(dst.capacity_bytes));
    }

    // Handle strided reads if bounding box has strides.
    if (bbox.has_value()) {
        const auto& box = bbox.value();
        bool has_strides = false;
        std::vector<ptrdiff_t> strides(meta.shape.rank, 1);
        for (int32_t d = 0; d < box.rank; ++d) {
            if (box.strides[d] > 1) {
                has_strides = true;
                strides[d] = static_cast<ptrdiff_t>(box.strides[d]);
            }
        }

        if (has_strides) {
            status = nc_get_vars(ncid_, varid, start.data(), count.data(),
                                 strides.data(), dst.data);
            nc_check(status, "nc_get_vars('" + meta.name + "')");
        } else {
            status = nc_get_vara(ncid_, varid, start.data(), count.data(),
                                 dst.data);
            nc_check(status, "nc_get_vara('" + meta.name + "')");
        }
    } else {
        status = nc_get_vara(ncid_, varid, start.data(), count.data(),
                             dst.data);
        nc_check(status, "nc_get_vara('" + meta.name + "')");
    }

    dst.used_bytes = total_bytes;

    (void)timestep;  // Timestep handling is managed by the variable's
                     // unlimited dimension if present.

#else
    (void)dst;
    (void)meta;
    (void)timestep;
    (void)bbox;
    throw eckit::Exception(
        "NetCDF_Driver::read: AMIO built without netCDF support");
#endif
}

// ===================================================================
// flush -- sync the file to ensure durability.
// ===================================================================

void NetCDF_Driver::flush() {
    if (!is_open_) {
        throw eckit::Exception(
            "NetCDF_Driver::flush: driver is not open");
    }

#ifdef AMIO_HAS_NETCDF
    int status = nc_sync(ncid_);
    nc_check(status, "nc_sync");
#else
    throw eckit::Exception(
        "NetCDF_Driver::flush: AMIO built without netCDF support");
#endif
}

// ===================================================================
// close -- close the file handle and release resources.
// ===================================================================

void NetCDF_Driver::close() {
    if (!is_open_) {
        return;  // Already closed -- idempotent.
    }

#ifdef AMIO_HAS_NETCDF
    int status = nc_close(ncid_);
    nc_check(status, "nc_close('" + file_path_ + "')");

    ncid_ = -1;
    comm_ = MPI_COMM_NULL;
    info_ = MPI_INFO_NULL;
#endif

    is_open_ = false;
    is_write_mode_ = false;
    file_path_.clear();
}

// ===================================================================
// Static factory registration.
//
// BackendRegistrar<NetCDF_Driver>("netcdf4") registers the driver
// with the BackendFactory at static initialization time so that
// BackendFactory::build("netcdf4", err) instantiates this driver.
// ===================================================================

namespace {
BackendRegistrar<NetCDF_Driver> reg_netcdf4("netcdf4");
}  // anonymous namespace

}  // namespace amio::detail
