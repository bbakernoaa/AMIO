// backend_driver.hpp -- AMIO Backend_Driver abstract base class.
//
// This header is PRIVATE to the AMIO_Core build (`src/factory/`).
// It is never installed and must not be referenced from any public
// header under `include/amio/`.
//
// Purpose
// -------
// Backend_Driver is the polymorphic interface that all concrete
// backend drivers (NetCDF_Driver, Zarr_Driver, GRIB2_Driver)
// implement.  It declares pure virtual methods for opening,
// writing, reading, flushing, and closing datasets.  The
// Backend_Factory (eckit::Factory<Backend_Driver>) dispatches to
// concrete implementations based on a string key registered at
// static initialization time.
//
// The class operates on:
//   * eckit::Configuration for dataset/variable configuration
//   * StagingBuffer references for data payloads
//   * VarMeta descriptors for variable metadata (dtype, shape, name)
//   * BoundingBox descriptors for selective reads
//
// No concrete driver class, header, or symbol is part of the public
// API.  The factory key is the only externally visible coupling
// (R4.8).
//
// Thread safety
// -------------
// Concrete drivers are invoked exclusively from Worker_Pool threads.
// A single driver instance is NOT required to be thread-safe; the
// Worker_Pool's per-(dataset, variable) ordering mutex serializes
// calls to the same driver instance (R6.3).
//
// Validates: R4.2, R4.8

#ifndef AMIO_SRC_FACTORY_BACKEND_DRIVER_HPP
#define AMIO_SRC_FACTORY_BACKEND_DRIVER_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "amio/amio_types.h"

// Forward-declare eckit::Configuration to avoid requiring eckit
// headers in translation units that only need the Backend_Driver
// interface declaration.  When eckit is available, the full header
// is included by concrete driver implementations.
namespace eckit {
class Configuration;
}  // namespace eckit

namespace amio::detail {

// Forward-declare StagingBuffer (defined in staging/staging_pool.hpp).
struct StagingBuffer;

// ---------------------------------------------------------------
// Supporting types private to AMIO_Core
// ---------------------------------------------------------------

// DatasetId -- opaque identifier for an open dataset within
// AMIO_Core.  Assigned by the C-Boundary handle table on
// dataset open.
using DatasetId = std::uint64_t;

// VariableId -- opaque identifier for a variable within a dataset.
// Used for per-(dataset, variable) ordering in the Worker_Pool.
using VariableId = std::uint64_t;

// VarMeta -- variable metadata descriptor passed to Backend_Driver
// write and read methods.  Carries the information a driver needs
// to locate and encode/decode a variable's payload within a dataset.
//
// Fields:
//   dataset_id   - identifies the owning dataset
//   variable_id  - identifies the variable within the dataset
//   name         - human-readable variable name (used by GRIB2 for
//                  WMO code table lookup, by NetCDF/Zarr for the
//                  variable path within the file/store)
//   dtype        - element type of the payload
//   shape        - N-dimensional shape descriptor (rank, extents,
//                  strides)
//   timestep     - the temporal index for this write/read operation
//                  (-1 if not applicable)
struct VarMeta {
    DatasetId dataset_id = 0;
    VariableId variable_id = 0;
    std::string name;
    amio_dtype_t dtype = AMIO_DTYPE_F32;
    amio_shape_t shape = {};
    std::int64_t timestep = -1;
};

// BoundingBox -- multidimensional sub-region descriptor for
// selective reads (R5.7).  When passed to Backend_Driver::read,
// the driver requests only the byte ranges that intersect the
// specified region, avoiding transfer of data outside the box.
//
// Fields:
//   rank     - number of valid dimensions (must match the
//              variable's rank)
//   offsets  - per-dimension start offset (0-based, inclusive)
//   extents  - per-dimension extent (number of elements)
//   strides  - per-dimension stride (1 = contiguous along that dim)
//
// Invariants:
//   * rank in [1, AMIO_MAX_RANK]
//   * For each dim d in [0, rank):
//       offsets[d] >= 0
//       extents[d] >= 1
//       strides[d] >= 1
//       offsets[d] + (extents[d] - 1) * strides[d] < variable_extent[d]
struct BoundingBox {
    std::int32_t rank = 0;
    std::int64_t offsets[AMIO_MAX_RANK] = {};
    std::int64_t extents[AMIO_MAX_RANK] = {};
    std::int64_t strides[AMIO_MAX_RANK] = {};
};

// ---------------------------------------------------------------
// Backend_Driver -- abstract base class for all backend drivers
// ---------------------------------------------------------------

// Backend_Driver is the pure virtual interface that concrete drivers
// implement.  It is registered with eckit::Factory<Backend_Driver>
// keyed by a string (e.g., "netcdf4", "zarr3", "grib2").  New
// drivers register at static initialization via
// eckit::ConcreteBuilderT0<Backend_Driver, ConcreteDriver>("key")
// without any modification to the public API (R4.2).
//
// Lifecycle:
//   1. Factory instantiates the driver (default constructor).
//   2. open_write() or open_read() is called with the dataset
//      configuration.
//   3. write() / read() are called zero or more times.
//   4. flush() is called to ensure all pending operations are
//      durable.
//   5. close() is called to release resources.
//
// Exception contract:
//   All methods may throw eckit::Exception (or std::exception
//   subclasses) on failure.  The Worker_Pool exception cordon
//   catches these and translates them to AMIO_ERR_* codes recorded
//   against the originating handle (R12.1, R12.2).
class Backend_Driver {
   public:
    virtual ~Backend_Driver() = default;

    // open_write -- prepare the driver for write operations.
    //
    // The configuration contains dataset-level parameters such as
    // file path / URI, data model, compression codec, chunk/shard
    // shapes, and any driver-specific options.
    //
    // Throws eckit::Exception on failure (e.g., missing Parallel
    // HDF5 support, invalid data model, missing required fields).
    virtual void open_write(const eckit::Configuration& config) = 0;

    // open_read -- prepare the driver for read operations.
    //
    // The configuration contains dataset-level parameters such as
    // file path / URI and any driver-specific options.
    //
    // Throws eckit::Exception on failure.
    virtual void open_read(const eckit::Configuration& config) = 0;

    // write -- serialize a staging buffer payload to storage.
    //
    // Parameters:
    //   src  - the StagingBuffer containing the payload bytes
    //          (src.data[0..src.used_bytes) is the payload)
    //   meta - variable metadata (name, dtype, shape, timestep)
    //
    // The driver encodes the payload according to its format and
    // writes it to the storage layer.  On success, the data is
    // durable (or at least committed to the driver's internal
    // write pipeline).  On failure, throws eckit::Exception.
    //
    // The caller (Worker_Pool) holds the per-(dataset, variable)
    // ordering mutex during this call, ensuring writes to the same
    // (dataset, variable) pair execute in submission order (R6.3).
    virtual void write(const StagingBuffer& src, const VarMeta& meta) = 0;

    // read -- deserialize a payload from storage into a staging buffer.
    //
    // Parameters:
    //   dst  - the StagingBuffer to populate (dst.data has capacity
    //          dst.capacity_bytes; the driver sets dst.used_bytes)
    //   meta - variable metadata identifying what to read
    //   timestep - the temporal index to read
    //   bbox - optional bounding box for selective reads; when
    //          present, the driver requests only intersecting byte
    //          ranges from storage (R5.7)
    //
    // On success, dst.data[0..dst.used_bytes) contains the decoded
    // payload.  On failure, throws eckit::Exception.
    virtual void read(StagingBuffer& dst, const VarMeta& meta, std::int64_t timestep, const std::optional<BoundingBox>& bbox) = 0;

    // flush -- ensure all previously written data is durable.
    //
    // Blocks until all pending internal write operations (if any)
    // have completed.  Throws eckit::Exception on failure.
    virtual void flush() = 0;

    // close -- release all resources held by the driver.
    //
    // After close(), no further write/read/flush calls are valid.
    // Throws eckit::Exception if outstanding operations cannot be
    // completed.
    virtual void close() = 0;
};

}  // namespace amio::detail

#endif  // AMIO_SRC_FACTORY_BACKEND_DRIVER_HPP
