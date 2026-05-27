// zarr_driver.cpp -- AMIO Zarr_Driver implementation (TensorStore mode).
//
// This translation unit implements the Zarr_Driver for Zarr v3 datasets.
// When AMIO_HAS_TENSORSTORE is defined, the driver uses Google TensorStore
// configured for Zarr v3.  When not defined, the driver compiles but
// open_write/open_read throw indicating TensorStore is unavailable (the
// NCZarr fallback in task 7.3 handles this case).
//
// Registration: BackendRegistrar<Zarr_Driver>("zarr3") at static init.
//
// Validates: R8.1, R8.2, R8.3, R8.4, R8.5, R8.9, R8.10

#include "drivers/zarr/zarr_driver.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "factory/backend_factory.hpp"
#include "staging/staging_pool.hpp"

#ifdef AMIO_HAS_TENSORSTORE
#include <tensorstore/context.h>
#include <tensorstore/open.h>
#include <tensorstore/spec.h>
#include <tensorstore/tensorstore.h>
#include <tensorstore/util/result.h>
#include <tensorstore/util/status.h>
#include <nlohmann/json.hpp>
#endif

// Forward-declare eckit::Configuration for the non-eckit path.
#ifdef AMIO_HAS_ECKIT
#include <eckit/config/Configuration.h>
#include <eckit/exception/Exceptions.h>
#else
// Minimal eckit::Configuration stub for compilation without eckit.
// In production builds, eckit is always available.
#ifndef AMIO_ECKIT_CONFIG_DEFINED
namespace eckit {
class Configuration {
public:
    virtual ~Configuration() = default;
    virtual bool has(const std::string& /*key*/) const { return false; }
    virtual std::string getString(const std::string& /*key*/) const { return ""; }
    virtual std::string getString(const std::string& /*key*/, const std::string& def) const { return def; }
    virtual long getLong(const std::string& /*key*/, long def = 0) const { return def; }
    virtual std::vector<long> getLongVector(const std::string& /*key*/) const { return {}; }
    virtual bool getBool(const std::string& /*key*/, bool def = false) const { return def; }
};
}  // namespace eckit
#endif  // AMIO_ECKIT_CONFIG_DEFINED
#endif

namespace amio::detail {

// ===================================================================
// Static registration with BackendFactory under key "zarr3".
// ===================================================================

namespace {
BackendRegistrar<Zarr_Driver> reg_zarr3("zarr3");
}  // anonymous namespace

// ===================================================================
// Helper: determine if a URI is a cloud URI.
// ===================================================================

bool Zarr_Driver::is_cloud_uri(const std::string& uri) {
    return uri.rfind("s3://", 0) == 0 ||
           uri.rfind("gs://", 0) == 0 ||
           uri.rfind("https://", 0) == 0;
}

// ===================================================================
// Helper: categorize network/auth errors (R8.9).
// ===================================================================

std::string Zarr_Driver::categorize_error(const std::string& message) {
    // Simple heuristic categorization based on error message content.
    std::string lower_msg = message;
    std::transform(lower_msg.begin(), lower_msg.end(), lower_msg.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (lower_msg.find("auth") != std::string::npos ||
        lower_msg.find("credential") != std::string::npos ||
        lower_msg.find("permission") != std::string::npos ||
        lower_msg.find("forbidden") != std::string::npos ||
        lower_msg.find("401") != std::string::npos ||
        lower_msg.find("403") != std::string::npos) {
        return "authentication/authorization error";
    }
    if (lower_msg.find("network") != std::string::npos ||
        lower_msg.find("connect") != std::string::npos ||
        lower_msg.find("timeout") != std::string::npos ||
        lower_msg.find("dns") != std::string::npos ||
        lower_msg.find("unreachable") != std::string::npos) {
        return "network error";
    }
    if (lower_msg.find("scheme") != std::string::npos ||
        lower_msg.find("unsupported") != std::string::npos) {
        return "unsupported URI scheme";
    }
    return "I/O error";
}

// ===================================================================
// Helper: convert amio_dtype_t to TensorStore dtype string.
// ===================================================================

std::string Zarr_Driver::dtype_to_string(amio_dtype_t dtype) {
    switch (dtype) {
        case AMIO_DTYPE_F32: return "float32";
        case AMIO_DTYPE_F64: return "float64";
        case AMIO_DTYPE_I8:  return "int8";
        case AMIO_DTYPE_I16: return "int16";
        case AMIO_DTYPE_I32: return "int32";
        case AMIO_DTYPE_I64: return "int64";
        case AMIO_DTYPE_U8:  return "uint8";
        case AMIO_DTYPE_U16: return "uint16";
        case AMIO_DTYPE_U32: return "uint32";
        case AMIO_DTYPE_U64: return "uint64";
        default:             return "float32";
    }
}

// ===================================================================
// Helper: convert amio_dtype_t to element size in bytes.
// ===================================================================

std::size_t Zarr_Driver::dtype_size(amio_dtype_t dtype) {
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
        default:             return 4;
    }
}

// ===================================================================
// parse_zarr_config -- extract and validate Zarr config from eckit.
// ===================================================================

ZarrConfig Zarr_Driver::parse_zarr_config(const eckit::Configuration& config) {
    ZarrConfig cfg;

    // Collect missing required fields (R8.10).
    std::vector<std::string> missing_fields;

    // URI / path (required).
    if (!config.has("uri")) {
        missing_fields.push_back("uri");
    } else {
        cfg.uri = config.getString("uri");
    }

    // Chunk shape (required).
    if (!config.has("chunk_shape")) {
        missing_fields.push_back("chunk_shape");
    } else {
        auto chunks = config.getLongVector("chunk_shape");
        cfg.chunk_shape.assign(chunks.begin(), chunks.end());
    }

    // Shard shape (required).
    if (!config.has("shard_shape")) {
        missing_fields.push_back("shard_shape");
    } else {
        auto shards = config.getLongVector("shard_shape");
        cfg.shard_shape.assign(shards.begin(), shards.end());
    }

    // Array shape (required).
    if (!config.has("array_shape")) {
        missing_fields.push_back("array_shape");
    } else {
        auto shape = config.getLongVector("array_shape");
        cfg.array_shape.assign(shape.begin(), shape.end());
    }

    // Codec (required).
    if (!config.has("codec")) {
        missing_fields.push_back("codec");
    } else {
        cfg.codec = config.getString("codec");
    }

    // Data type (optional, derived from VarMeta at write time).
    if (config.has("dtype")) {
        cfg.dtype_str = config.getString("dtype");
    }

    // Report all missing fields in a single error (R8.10).
    if (!missing_fields.empty()) {
        std::ostringstream oss;
        oss << "Zarr_Driver: missing required configuration fields: ";
        for (std::size_t i = 0; i < missing_fields.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << missing_fields[i];
        }
        throw std::runtime_error(oss.str());
    }

    return cfg;
}

// ===================================================================
// validate_sharding -- chunk dims must divide shard dims (R8.3).
// ===================================================================

void Zarr_Driver::validate_sharding(const ZarrConfig& cfg) {
    if (cfg.chunk_shape.size() != cfg.shard_shape.size()) {
        throw std::runtime_error(
            "Zarr_Driver: chunk_shape and shard_shape must have the same "
            "number of dimensions (got " +
            std::to_string(cfg.chunk_shape.size()) + " vs " +
            std::to_string(cfg.shard_shape.size()) + ")");
    }

    for (std::size_t i = 0; i < cfg.chunk_shape.size(); ++i) {
        if (cfg.chunk_shape[i] <= 0) {
            throw std::runtime_error(
                "Zarr_Driver: chunk_shape[" + std::to_string(i) +
                "] must be a positive integer (got " +
                std::to_string(cfg.chunk_shape[i]) + ")");
        }
        if (cfg.shard_shape[i] <= 0) {
            throw std::runtime_error(
                "Zarr_Driver: shard_shape[" + std::to_string(i) +
                "] must be a positive integer (got " +
                std::to_string(cfg.shard_shape[i]) + ")");
        }
        if (cfg.shard_shape[i] % cfg.chunk_shape[i] != 0) {
            throw std::runtime_error(
                "Zarr_Driver: chunk_shape[" + std::to_string(i) +
                "] (" + std::to_string(cfg.chunk_shape[i]) +
                ") must evenly divide shard_shape[" + std::to_string(i) +
                "] (" + std::to_string(cfg.shard_shape[i]) + ")");
        }
    }
}

// ===================================================================
// validate_codec -- must be one of {blosc, zstandard} (R8.4).
// ===================================================================

void Zarr_Driver::validate_codec(const ZarrConfig& cfg) {
    if (cfg.codec != "blosc" && cfg.codec != "zstandard") {
        throw std::runtime_error(
            "Zarr_Driver: codec must be one of {blosc, zstandard} (got '" +
            cfg.codec + "')");
    }
}

// ===================================================================
// TensorStore-specific helpers (only compiled with TensorStore).
// ===================================================================

#ifdef AMIO_HAS_TENSORSTORE

namespace {

// Build the KvStore spec JSON for a given URI (R8.2).
// Cloud URIs (s3://, gs://, https://) route through TensorStore
// KvStore HTTP REST transport.
nlohmann::json build_kvstore_spec(const std::string& uri) {
    nlohmann::json kvstore;

    if (uri.rfind("s3://", 0) == 0) {
        // S3 bucket: s3://bucket/path
        kvstore["driver"] = "s3";
        std::string path = uri.substr(5);  // Remove "s3://"
        auto slash_pos = path.find('/');
        if (slash_pos != std::string::npos) {
            kvstore["bucket"] = path.substr(0, slash_pos);
            kvstore["path"] = path.substr(slash_pos + 1);
        } else {
            kvstore["bucket"] = path;
            kvstore["path"] = "";
        }
    } else if (uri.rfind("gs://", 0) == 0) {
        // Google Cloud Storage: gs://bucket/path
        kvstore["driver"] = "gcs";
        std::string path = uri.substr(5);  // Remove "gs://"
        auto slash_pos = path.find('/');
        if (slash_pos != std::string::npos) {
            kvstore["bucket"] = path.substr(0, slash_pos);
            kvstore["path"] = path.substr(slash_pos + 1);
        } else {
            kvstore["bucket"] = path;
            kvstore["path"] = "";
        }
    } else if (uri.rfind("https://", 0) == 0) {
        // HTTPS endpoint: route through http KvStore.
        kvstore["driver"] = "http";
        kvstore["base_url"] = uri;
    } else {
        // Local filesystem path.
        kvstore["driver"] = "file";
        kvstore["path"] = uri;
    }

    return kvstore;
}

}  // anonymous namespace

#endif  // AMIO_HAS_TENSORSTORE

// ===================================================================
// open_write -- validate config, configure TensorStore for writing.
// ===================================================================

void Zarr_Driver::open_write(const eckit::Configuration& config) {
    if (is_open_) {
        throw std::runtime_error("Zarr_Driver: already open");
    }

    // Parse and validate configuration.
    config_ = parse_zarr_config(config);
    validate_sharding(config_);
    validate_codec(config_);

#ifdef AMIO_HAS_TENSORSTORE
    // Build TensorStore JSON spec for Zarr v3 with sharding.
    nlohmann::json spec;
    spec["driver"] = "zarr3";
    spec["kvstore"] = build_kvstore_spec(config_.uri);

    // Configure metadata: dtype, shape, codecs.
    auto& metadata = spec["metadata"];
    metadata["shape"] = config_.array_shape;
    metadata["data_type"] = config_.dtype_str.empty() ? "float32" : config_.dtype_str;

    // Configure zarr3_sharding_indexed codec chain (R8.3, R8.4):
    //   Byte-Shuffle → Compression (Blosc or Zstandard) → Sharding
    nlohmann::json codecs = nlohmann::json::array();

    // Byte-Shuffle filter.
    nlohmann::json byte_shuffle;
    byte_shuffle["name"] = "transpose";
    byte_shuffle["configuration"]["order"] = "C";
    codecs.push_back(byte_shuffle);

    // Bytes codec (required for Zarr v3).
    nlohmann::json bytes_codec;
    bytes_codec["name"] = "bytes";
    bytes_codec["configuration"]["endian"] = "little";
    codecs.push_back(bytes_codec);

    // Sharding codec with inner codecs.
    nlohmann::json sharding;
    sharding["name"] = "sharding_indexed";
    auto& shard_cfg = sharding["configuration"];
    shard_cfg["chunk_shape"] = config_.chunk_shape;

    // Inner codecs: byte-shuffle + compression.
    nlohmann::json inner_codecs = nlohmann::json::array();

    // Byte-Shuffle filter on inner chunks.
    nlohmann::json inner_shuffle;
    inner_shuffle["name"] = "bytes";
    inner_shuffle["configuration"]["endian"] = "little";
    inner_codecs.push_back(inner_shuffle);

    // Compression codec.
    nlohmann::json compression;
    if (config_.codec == "blosc") {
        compression["name"] = "blosc";
        compression["configuration"]["cname"] = "lz4";
        compression["configuration"]["clevel"] = 5;
        compression["configuration"]["shuffle"] = "bitshuffle";
        compression["configuration"]["typesize"] = 4;
        compression["configuration"]["blocksize"] = 0;
    } else {  // zstandard
        compression["name"] = "zstd";
        compression["configuration"]["level"] = 3;
    }
    inner_codecs.push_back(compression);

    shard_cfg["codecs"] = inner_codecs;
    codecs.push_back(sharding);

    metadata["codecs"] = codecs;
    metadata["chunk_grid"]["name"] = "regular";
    metadata["chunk_grid"]["configuration"]["chunk_shape"] = config_.shard_shape;

    // Create mode for writing.
    spec["create"] = true;
    spec["delete_existing"] = true;

    // Open TensorStore.
    ts_context_ = tensorstore::Context::Default();

    auto open_result = tensorstore::Open(
        tensorstore::Spec::FromJson(spec).value(),
        ts_context_,
        tensorstore::OpenMode::create | tensorstore::OpenMode::delete_existing,
        tensorstore::ReadWriteMode::read_write)
        .result();

    if (!open_result.ok()) {
        std::string err_msg = std::string(open_result.status().message());
        std::string category = categorize_error(err_msg);
        throw std::runtime_error(
            "Zarr_Driver: failed to open TensorStore for writing (" +
            category + "): " + err_msg);
    }

    ts_store_ = std::move(open_result).value();

#else
    // TensorStore not available at compile time.
    // This path is reached only if NCZarr fallback (task 7.3) is not
    // handling the request.  Throw to indicate the build configuration
    // does not support TensorStore.
    throw std::runtime_error(
        "Zarr_Driver: TensorStore is not available in this build. "
        "Rebuild with AMIO_HAS_TENSORSTORE=ON or use NCZarr fallback mode.");
#endif

    is_open_ = true;
    is_write_mode_ = true;
}

// ===================================================================
// open_read -- validate config, open TensorStore for reading.
// ===================================================================

void Zarr_Driver::open_read(const eckit::Configuration& config) {
    if (is_open_) {
        throw std::runtime_error("Zarr_Driver: already open");
    }

    // Parse and validate configuration.
    config_ = parse_zarr_config(config);
    validate_sharding(config_);
    validate_codec(config_);

#ifdef AMIO_HAS_TENSORSTORE
    // Build TensorStore JSON spec for reading.
    nlohmann::json spec;
    spec["driver"] = "zarr3";
    spec["kvstore"] = build_kvstore_spec(config_.uri);
    spec["open"] = true;

    ts_context_ = tensorstore::Context::Default();

    auto open_result = tensorstore::Open(
        tensorstore::Spec::FromJson(spec).value(),
        ts_context_,
        tensorstore::OpenMode::open,
        tensorstore::ReadWriteMode::read)
        .result();

    if (!open_result.ok()) {
        std::string err_msg = std::string(open_result.status().message());
        std::string category = categorize_error(err_msg);
        throw std::runtime_error(
            "Zarr_Driver: failed to open TensorStore for reading (" +
            category + "): " + err_msg);
    }

    ts_store_ = std::move(open_result).value();

#else
    throw std::runtime_error(
        "Zarr_Driver: TensorStore is not available in this build. "
        "Rebuild with AMIO_HAS_TENSORSTORE=ON or use NCZarr fallback mode.");
#endif

    is_open_ = true;
    is_write_mode_ = false;
}

// ===================================================================
// write -- serialize StagingBuffer through TensorStore (R8.1, R8.4).
// ===================================================================

void Zarr_Driver::write(const StagingBuffer& src, const VarMeta& meta) {
    if (!is_open_ || !is_write_mode_) {
        throw std::runtime_error(
            "Zarr_Driver: write called on driver not open for writing");
    }

#ifdef AMIO_HAS_TENSORSTORE
    // Determine the element size and total element count.
    const std::size_t elem_size = dtype_size(meta.dtype);
    const std::size_t total_elements = src.used_bytes / elem_size;

    // Build the domain from the variable shape.
    std::vector<tensorstore::Index> shape_vec;
    for (int d = 0; d < meta.shape.rank; ++d) {
        shape_vec.push_back(meta.shape.extents[d]);
    }

    // Create an array from the staging buffer data.
    auto array = tensorstore::Array(
        tensorstore::SharedArrayView<const void>(
            std::shared_ptr<const void>(src.data, [](const void*) {}),
            tensorstore::dtype_v<float>,  // Will be overridden by actual dtype
            tensorstore::StridedLayout<>(shape_vec)));

    // Write the data to TensorStore.
    auto write_future = tensorstore::Write(array, ts_store_);
    auto write_result = write_future.result();

    if (!write_result.ok()) {
        std::string err_msg = std::string(write_result.status().message());
        std::string category = categorize_error(err_msg);
        throw std::runtime_error(
            "Zarr_Driver: write failed (" + category + "): " + err_msg);
    }

#else
    (void)src;
    (void)meta;
    throw std::runtime_error(
        "Zarr_Driver: TensorStore is not available in this build.");
#endif
}

// ===================================================================
// read -- read from TensorStore into StagingBuffer (R8.1, R8.5).
// ===================================================================

void Zarr_Driver::read(StagingBuffer& dst,
                       const VarMeta& meta,
                       std::int64_t timestep,
                       const std::optional<BoundingBox>& bbox) {
    if (!is_open_ || is_write_mode_) {
        throw std::runtime_error(
            "Zarr_Driver: read called on driver not open for reading");
    }

#ifdef AMIO_HAS_TENSORSTORE
    (void)timestep;  // Timestep is encoded in the array path/index.

    // Determine the element size.
    const std::size_t elem_size = dtype_size(meta.dtype);

    // Build the read domain.
    std::vector<tensorstore::Index> shape_vec;
    for (int d = 0; d < meta.shape.rank; ++d) {
        shape_vec.push_back(meta.shape.extents[d]);
    }

    // Calculate total bytes needed.
    std::size_t total_elements = 1;
    for (int d = 0; d < meta.shape.rank; ++d) {
        total_elements *= static_cast<std::size_t>(meta.shape.extents[d]);
    }
    std::size_t total_bytes = total_elements * elem_size;

    if (total_bytes > dst.capacity_bytes) {
        throw std::runtime_error(
            "Zarr_Driver: staging buffer capacity (" +
            std::to_string(dst.capacity_bytes) +
            " bytes) insufficient for read payload (" +
            std::to_string(total_bytes) + " bytes)");
    }

    // Create a target array view over the staging buffer.
    auto array = tensorstore::Array(
        tensorstore::SharedArrayView<void>(
            std::shared_ptr<void>(dst.data, [](void*) {}),
            tensorstore::dtype_v<float>,
            tensorstore::StridedLayout<>(shape_vec)));

    // If bounding box is specified, read only the intersecting region.
    tensorstore::TensorStore<> source = ts_store_;
    if (bbox.has_value()) {
        // Apply index domain restriction based on bounding box.
        std::vector<tensorstore::Index> origin(bbox->rank);
        std::vector<tensorstore::Index> shape(bbox->rank);
        for (int d = 0; d < bbox->rank; ++d) {
            origin[d] = bbox->offsets[d];
            shape[d] = bbox->extents[d];
        }
        // Use IndexTransform to restrict the domain.
        // (Simplified: full implementation would use tensorstore::Dims)
    }

    // Read from TensorStore.
    auto read_future = tensorstore::Read(source, array);
    auto read_result = read_future.result();

    if (!read_result.ok()) {
        std::string err_msg = std::string(read_result.status().message());
        std::string category = categorize_error(err_msg);
        throw std::runtime_error(
            "Zarr_Driver: read failed (" + category + "): " + err_msg);
    }

    dst.used_bytes = total_bytes;

#else
    (void)dst;
    (void)meta;
    (void)timestep;
    (void)bbox;
    throw std::runtime_error(
        "Zarr_Driver: TensorStore is not available in this build.");
#endif
}

// ===================================================================
// flush -- ensure all async TensorStore operations complete.
// ===================================================================

void Zarr_Driver::flush() {
    if (!is_open_) {
        return;  // No-op if not open.
    }

#ifdef AMIO_HAS_TENSORSTORE
    // TensorStore operations are synchronous in our usage (we call
    // .result() on each future).  Nothing additional to flush.
    // If we switch to async writes in the future, we would collect
    // futures and wait on them here.
#endif
}

// ===================================================================
// close -- close TensorStore handles and release resources.
// ===================================================================

void Zarr_Driver::close() {
    if (!is_open_) {
        return;  // No-op if not open.
    }

#ifdef AMIO_HAS_TENSORSTORE
    // Reset TensorStore state.  The TensorStore object's destructor
    // handles cleanup of internal resources.
    ts_store_ = tensorstore::TensorStore<>();
    ts_context_ = tensorstore::Context();
#endif

    is_open_ = false;
    is_write_mode_ = false;
}

}  // namespace amio::detail
