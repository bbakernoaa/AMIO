// grib2_driver.hpp -- AMIO GRIB2_Driver backend implementation.
//
// This header is PRIVATE to the AMIO_Core build (`src/drivers/grib2/`).
// It is never installed and must not be referenced from any public
// header under `include/amio/`.
//
// Purpose
// -------
// The GRIB2_Driver encodes/decodes model output to WMO-compliant
// GRIB2 using nceplibs-g2c.  Key design features:
//
//   * Loads nceplibs-g2c and eckit-loaded WMO code table mapping on
//     construction (bounded at 5 seconds).
//   * Translates every human-readable metadata string through the
//     WMO mapping table before encoding.
//   * Mdspan contiguity check gates fast vs slow path:
//       - is_always_contiguous() + row-major → zero-copy pass to g2c
//       - Otherwise → pack into contiguous 1D buffer, pass packed
//   * DRT restricted to {Adaptive Entropy Coding via libaec,
//     Lossless JPEG2000}; others rejected with eckit::Exception.
//   * Missing WMO key or invalid DRT → eckit::Exception, zero output
//     bytes.
//
// Conditional compilation:
//   - When AMIO_HAS_G2C is defined, the real g2c API is used.
//   - When not defined, the driver compiles but throws on open
//     indicating g2c is not available.
//
// Registered with BackendFactory via key "grib2" at static init.
//
// Thread safety
// -------------
// A single GRIB2_Driver instance is NOT thread-safe.  The Worker_Pool
// serializes calls via the per-(dataset, variable) ordering mutex.
//
// Validates: R9.1, R9.2, R9.3, R9.4, R9.5, R9.6, R9.7, R9.8

#ifndef AMIO_SRC_DRIVERS_GRIB2_GRIB2_DRIVER_HPP
#define AMIO_SRC_DRIVERS_GRIB2_GRIB2_DRIVER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "factory/backend_driver.hpp"

namespace amio::detail {

// Forward-declare StagingPool for slow-path buffer allocation.
class StagingPool;

// WMO code table mapping: human-readable string → integer code.
// Loaded from eckit::Configuration on construction.
using WmoCodeTable = std::unordered_map<std::string, std::int64_t>;

// Allowed Data Representation Template identifiers.
// These correspond to the WMO GRIB2 DRT numbers:
//   - 42 = Grid Point Data - CCSDS (Adaptive Entropy Coding via libaec)
//   - 40 = Grid Point Data - JPEG2000 (Lossless JPEG2000)
enum class GRIB2_DRT : std::int32_t {
    AdaptiveEntropyCoding = 42,  // libaec
    LosslessJPEG2000      = 40   // JPEG2000 lossless mode
};

// Map from human-readable DRT name to enum.
// Recognized names:
//   "adaptive_entropy_coding" or "libaec" → AdaptiveEntropyCoding
//   "lossless_jpeg2000" or "jpeg2000"     → LosslessJPEG2000
GRIB2_DRT parse_drt_name(const std::string& name);

// GRIB2_Driver -- concrete Backend_Driver for GRIB2 encoding via
// nceplibs-g2c.
//
// Lifecycle:
//   1. Default-constructed by BackendFactory.
//   2. open_write() / open_read() loads g2c + WMO tables.
//   3. write() / read() encode/decode GRIB2 records.
//   4. flush() is a no-op (GRIB2 records are self-contained).
//   5. close() releases g2c resources.
class GRIB2_Driver : public Backend_Driver {
public:
    GRIB2_Driver();
    ~GRIB2_Driver() override;

    // Backend_Driver interface
    void open_write(const eckit::Configuration& config) override;
    void open_read(const eckit::Configuration& config) override;
    void write(const StagingBuffer& src, const VarMeta& meta) override;
    void read(StagingBuffer& dst,
              const VarMeta& meta,
              std::int64_t timestep,
              const std::optional<BoundingBox>& bbox) override;
    void flush() override;
    void close() override;

private:
    // Initialize g2c library and WMO code table mapping.
    // Throws eckit::Exception on failure or timeout (5s bound).
    void initialize(const eckit::Configuration& config);

    // Validate that the DRT field is present and in the allowed set.
    // Throws eckit::Exception with appropriate message on failure.
    GRIB2_DRT validate_drt(const eckit::Configuration& config) const;

    // Translate all metadata keys through WMO code table.
    // Throws eckit::Exception if any key is missing from the table.
    // Returns a map of translated integer codes.
    std::unordered_map<std::string, std::int64_t>
    translate_metadata(const eckit::Configuration& config) const;

    // Check if a buffer described by shape is contiguous and row-major.
    // Returns true if the data can be passed directly to g2c (fast path).
    static bool is_contiguous_row_major(const amio_shape_t& shape);

    // Pack non-contiguous data into a contiguous row-major buffer.
    // Returns the packed buffer as a vector of bytes.
    static std::vector<std::byte> pack_row_major(
        const std::byte* src_data,
        const amio_shape_t& shape,
        std::size_t element_size);

    // Compute total element count from shape.
    static std::size_t total_elements(const amio_shape_t& shape);

    // Compute element size in bytes from dtype.
    static std::size_t dtype_size(amio_dtype_t dtype);

    // State
    bool initialized_ = false;
    WmoCodeTable wmo_table_;
    std::string output_path_;
    GRIB2_DRT active_drt_ = GRIB2_DRT::AdaptiveEntropyCoding;

    // Initialization timeout (5 seconds per R9.1).
    static constexpr auto kInitTimeout = std::chrono::seconds(5);
};

}  // namespace amio::detail

#endif  // AMIO_SRC_DRIVERS_GRIB2_GRIB2_DRIVER_HPP
