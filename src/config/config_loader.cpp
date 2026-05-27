// config_loader.cpp -- AMIO Config_Loader implementation.
//
// Implements manifest parsing, schema validation, and serialization
// for AMIO runtime configuration.  When AMIO_HAS_ECKIT is defined,
// delegates to eckit::YAMLConfiguration / eckit::JSONConfiguration.
// Otherwise, uses a standalone minimal YAML/JSON parser.
//
// Validates: R1.2, R1.3, R1.5, R11.3, R11.4, R11.5, R11.6, R11.7

#include "config/config_loader.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

#ifdef AMIO_HAS_ECKIT
#include <eckit/config/YAMLConfiguration.h>
#include <eckit/config/JSONConfiguration.h>
#include <eckit/filesystem/PathName.h>
#endif

namespace amio::detail {

// ===================================================================
// Static codec allow-list
// ===================================================================

const std::vector<std::string>& ConfigLoader::valid_codecs() {
    static const std::vector<std::string> codecs = {
        "blosc",
        "zstandard",
        "libaec",
        "lossless_jpeg2000"
    };
    return codecs;
}

bool ConfigLoader::is_valid_codec(const std::string& name) {
    const auto& vc = valid_codecs();
    return std::find(vc.begin(), vc.end(), name) != vc.end();
}

// ===================================================================
// Validation
// ===================================================================

amio_err_t ConfigLoader::validate(const Config& config,
                                  ValidationError& error_out) {
    // Single-pass validation: report first failing rule (R11.4).

    // staging_pool.buffer_count [1, 4096]
    if (config.staging_pool.buffer_count < kMinBufferCount ||
        config.staging_pool.buffer_count > kMaxBufferCount) {
        error_out.field_path = "staging_pool.buffer_count";
        error_out.message = "buffer_count must be in [1, 4096], got " +
                            std::to_string(config.staging_pool.buffer_count);
        return AMIO_ERR_MANIFEST_INVALID;
    }

    // staging_pool.buffer_capacity_bytes [1, 1 GiB]
    if (config.staging_pool.buffer_capacity_bytes < kMinBufferCapacity ||
        config.staging_pool.buffer_capacity_bytes > kMaxBufferCapacity) {
        error_out.field_path = "staging_pool.buffer_capacity_bytes";
        error_out.message = "buffer_capacity_bytes must be in [1, 1073741824], got " +
                            std::to_string(config.staging_pool.buffer_capacity_bytes);
        return AMIO_ERR_MANIFEST_INVALID;
    }

    // worker_pool.threads [1, 256]
    if (config.worker_pool.threads < kMinThreads ||
        config.worker_pool.threads > kMaxThreads) {
        error_out.field_path = "worker_pool.threads";
        error_out.message = "threads must be in [1, 256], got " +
                            std::to_string(config.worker_pool.threads);
        return AMIO_ERR_MANIFEST_INVALID;
    }

    // prefetch.depth [1, 1024]
    if (config.prefetch.depth < kMinPrefetchDepth ||
        config.prefetch.depth > kMaxPrefetchDepth) {
        error_out.field_path = "prefetch.depth";
        error_out.message = "depth must be in [1, 1024], got " +
                            std::to_string(config.prefetch.depth);
        return AMIO_ERR_MANIFEST_INVALID;
    }

    // prefetch.read_timeout_s [1, 3600]
    if (config.prefetch.read_timeout_s < kMinReadTimeoutS ||
        config.prefetch.read_timeout_s > kMaxReadTimeoutS) {
        error_out.field_path = "prefetch.read_timeout_s";
        error_out.message = "read_timeout_s must be in [1, 3600], got " +
                            std::to_string(config.prefetch.read_timeout_s);
        return AMIO_ERR_MANIFEST_INVALID;
    }

    // staging_timeout_ms [1, 60000]
    if (config.staging_timeout_ms < kMinStagingTimeoutMs ||
        config.staging_timeout_ms > kMaxStagingTimeoutMs) {
        error_out.field_path = "staging_timeout_ms";
        error_out.message = "staging_timeout_ms must be in [1, 60000], got " +
                            std::to_string(config.staging_timeout_ms);
        return AMIO_ERR_MANIFEST_INVALID;
    }

    // Backpressure invariant: 0 <= low < high <= capacity (when configured)
    if (config.backpressure.high_watermark > 0) {
        if (config.backpressure.low_watermark >= config.backpressure.high_watermark) {
            error_out.field_path = "backpressure.low_watermark";
            error_out.message = "low_watermark must be < high_watermark";
            return AMIO_ERR_MANIFEST_INVALID;
        }
        if (config.backpressure.high_watermark > config.backpressure.queue_capacity) {
            error_out.field_path = "backpressure.high_watermark";
            error_out.message = "high_watermark must be <= queue_capacity";
            return AMIO_ERR_MANIFEST_INVALID;
        }
    }

    // Codec allow-list enforcement (R11.6, R11.7).
    for (const auto& codec : config.codec.lossless_allow_list) {
        if (!is_valid_codec(codec)) {
            error_out.field_path = "codec.lossless_allow_list";
            error_out.message = "codec '" + codec + "' is not a recognized lossless codec";
            return AMIO_ERR_LOSSY_CODEC_FORBIDDEN;
        }
    }

    // Active codec must be on the allow-list (if specified).
    if (!config.codec.active_codec.empty()) {
        if (!is_valid_codec(config.codec.active_codec)) {
            error_out.field_path = "codec.active_codec";
            error_out.message = "active codec '" + config.codec.active_codec +
                                "' is not a recognized lossless codec";
            return AMIO_ERR_LOSSY_CODEC_FORBIDDEN;
        }
        // Also check it's on the manifest's own allow-list.
        const auto& al = config.codec.lossless_allow_list;
        if (!al.empty() &&
            std::find(al.begin(), al.end(), config.codec.active_codec) == al.end()) {
            error_out.field_path = "codec.active_codec";
            error_out.message = "active codec '" + config.codec.active_codec +
                                "' is not on the manifest's lossless_allow_list";
            return AMIO_ERR_LOSSY_CODEC_FORBIDDEN;
        }
    }

    return AMIO_OK;
}

// ===================================================================
// Standalone YAML tokenizer
//
// A minimal YAML parser that handles the subset of YAML used by
// AMIO manifests: nested mappings, scalar values, and lists.
// This is NOT a full YAML 1.2 parser -- it handles the common
// patterns used in AMIO configuration files.
// ===================================================================

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static int count_indent(const std::string& line) {
    int count = 0;
    for (char c : line) {
        if (c == ' ') ++count;
        else break;
    }
    return count;
}

std::vector<ConfigLoader::KeyValue> ConfigLoader::tokenize_yaml(
    const std::string& content) {
    std::vector<KeyValue> tokens;
    std::istringstream stream(content);
    std::string line;
    int line_num = 0;

    // Stack of prefix paths based on indentation.
    std::vector<std::pair<int, std::string>> prefix_stack;

    while (std::getline(stream, line)) {
        ++line_num;

        // Skip empty lines and comments.
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // Skip YAML document markers.
        if (trimmed == "---" || trimmed == "...") continue;

        int indent = count_indent(line);

        // Pop prefix stack entries that are at same or deeper indent.
        while (!prefix_stack.empty() && prefix_stack.back().first >= indent) {
            prefix_stack.pop_back();
        }

        // Check for list item.
        bool is_list_item = false;
        std::string work = trimmed;
        if (work.size() >= 2 && work[0] == '-' && work[1] == ' ') {
            is_list_item = true;
            work = trim(work.substr(2));
        }

        // Check for key: value pair.
        auto colon_pos = work.find(':');
        if (colon_pos != std::string::npos && !is_list_item) {
            std::string key = trim(work.substr(0, colon_pos));
            std::string value = "";
            if (colon_pos + 1 < work.size()) {
                value = trim(work.substr(colon_pos + 1));
                // Remove inline comments.
                auto comment_pos = value.find(" #");
                if (comment_pos != std::string::npos) {
                    value = trim(value.substr(0, comment_pos));
                }
                // Remove quotes.
                if (value.size() >= 2 &&
                    ((value.front() == '"' && value.back() == '"') ||
                     (value.front() == '\'' && value.back() == '\''))) {
                    value = value.substr(1, value.size() - 2);
                }
            }

            // Build full path.
            std::string prefix;
            for (const auto& [_, p] : prefix_stack) {
                prefix += p + ".";
            }

            if (value.empty()) {
                // This is a mapping key -- push onto prefix stack.
                prefix_stack.push_back({indent, key});
            } else {
                // This is a scalar value.
                KeyValue kv;
                kv.key = prefix + key;
                kv.value = value;
                kv.line = line_num;
                kv.indent = indent;
                kv.is_list_item = false;
                tokens.push_back(kv);
            }
        } else if (is_list_item) {
            // List item without a key -- use the current prefix path.
            std::string prefix;
            for (const auto& [_, p] : prefix_stack) {
                prefix += p + ".";
            }
            // Remove trailing dot.
            if (!prefix.empty() && prefix.back() == '.') {
                prefix.pop_back();
            }

            KeyValue kv;
            kv.key = prefix;
            kv.value = work;
            kv.line = line_num;
            kv.indent = indent;
            kv.is_list_item = true;
            tokens.push_back(kv);
        }
    }

    return tokens;
}

// ===================================================================
// Standalone JSON tokenizer
//
// A minimal JSON parser for AMIO manifests.  Handles nested objects,
// arrays of scalars, and string/number values.
// ===================================================================

std::vector<ConfigLoader::KeyValue> ConfigLoader::tokenize_json(
    const std::string& content) {
    std::vector<KeyValue> tokens;

    // Simple state-machine JSON parser.
    std::vector<std::string> path_stack;
    bool in_array = false;
    std::string current_array_path;

    std::istringstream stream(content);
    std::string line;
    int line_num = 0;

    while (std::getline(stream, line)) {
        ++line_num;
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;

        // Remove trailing comma.
        if (!trimmed.empty() && trimmed.back() == ',') {
            trimmed.pop_back();
            trimmed = trim(trimmed);
        }

        // Handle braces and brackets.
        if (trimmed == "{" || trimmed == "}") {
            if (trimmed == "}") {
                if (!path_stack.empty()) path_stack.pop_back();
            }
            continue;
        }
        if (trimmed == "[") {
            in_array = true;
            continue;
        }
        if (trimmed == "]") {
            in_array = false;
            current_array_path.clear();
            continue;
        }

        // Parse "key": value or "key": {
        auto quote1 = trimmed.find('"');
        if (quote1 != std::string::npos) {
            auto quote2 = trimmed.find('"', quote1 + 1);
            if (quote2 == std::string::npos) continue;

            std::string key = trimmed.substr(quote1 + 1, quote2 - quote1 - 1);
            auto colon = trimmed.find(':', quote2);
            if (colon == std::string::npos) {
                // Bare string in array.
                if (in_array && !current_array_path.empty()) {
                    KeyValue kv;
                    kv.key = current_array_path;
                    kv.value = key;
                    kv.line = line_num;
                    kv.is_list_item = true;
                    tokens.push_back(kv);
                }
                continue;
            }

            std::string value_part = trim(trimmed.substr(colon + 1));

            // Remove trailing comma from value.
            if (!value_part.empty() && value_part.back() == ',') {
                value_part.pop_back();
                value_part = trim(value_part);
            }

            // Build full path.
            std::string full_path;
            for (const auto& p : path_stack) {
                full_path += p + ".";
            }
            full_path += key;

            if (value_part == "{") {
                // Nested object.
                path_stack.push_back(key);
            } else if (value_part == "[") {
                // Array start.
                in_array = true;
                current_array_path = full_path;
            } else {
                // Scalar value.
                // Remove quotes from string values.
                if (value_part.size() >= 2 &&
                    value_part.front() == '"' && value_part.back() == '"') {
                    value_part = value_part.substr(1, value_part.size() - 2);
                }

                KeyValue kv;
                kv.key = full_path;
                kv.value = value_part;
                kv.line = line_num;
                kv.is_list_item = false;
                tokens.push_back(kv);
            }
        } else if (in_array && !current_array_path.empty()) {
            // Bare number or value in array.
            KeyValue kv;
            kv.key = current_array_path;
            kv.value = trimmed;
            kv.line = line_num;
            kv.is_list_item = true;
            tokens.push_back(kv);
        }
    }

    return tokens;
}

// ===================================================================
// Populate Config from tokens
// ===================================================================

static bool parse_size_t(const std::string& s, std::size_t& out) {
    if (s.empty()) return false;
    try {
        unsigned long long val = std::stoull(s);
        out = static_cast<std::size_t>(val);
        return true;
    } catch (...) {
        return false;
    }
}

static bool parse_int(const std::string& s, int& out) {
    if (s.empty()) return false;
    try {
        out = std::stoi(s);
        return true;
    } catch (...) {
        return false;
    }
}

amio_err_t ConfigLoader::populate_config(
    const std::vector<KeyValue>& tokens,
    Config& config_out,
    ValidationError& error_out) {

    for (const auto& kv : tokens) {
        const std::string& key = kv.key;
        const std::string& val = kv.value;

        if (key == "staging_pool.buffer_count") {
            if (!parse_size_t(val, config_out.staging_pool.buffer_count)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
        } else if (key == "staging_pool.buffer_capacity_bytes") {
            if (!parse_size_t(val, config_out.staging_pool.buffer_capacity_bytes)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
        } else if (key == "worker_pool.threads") {
            if (!parse_size_t(val, config_out.worker_pool.threads)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
        } else if (key == "worker_pool.cpu_cores" && kv.is_list_item) {
            int core;
            if (!parse_int(val, core)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value in cpu_cores list: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
            config_out.worker_pool.cpu_cores.push_back(core);
        } else if (key == "worker_pool.numa_domain") {
            int nd;
            if (!parse_int(val, nd)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
            config_out.worker_pool.numa_domain = nd;
        } else if (key == "prefetch.depth") {
            if (!parse_size_t(val, config_out.prefetch.depth)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
        } else if (key == "prefetch.read_timeout_s") {
            if (!parse_size_t(val, config_out.prefetch.read_timeout_s)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
        } else if (key == "staging_timeout_ms") {
            if (!parse_size_t(val, config_out.staging_timeout_ms)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
        } else if (key == "backpressure.low_watermark") {
            if (!parse_size_t(val, config_out.backpressure.low_watermark)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
        } else if (key == "backpressure.high_watermark") {
            if (!parse_size_t(val, config_out.backpressure.high_watermark)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
        } else if (key == "backpressure.queue_capacity") {
            if (!parse_size_t(val, config_out.backpressure.queue_capacity)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
        } else if (key == "backend") {
            config_out.backend = val;
        } else if (key == "codec.active_codec") {
            config_out.codec.active_codec = val;
        } else if (key == "codec.lossless_allow_list" && kv.is_list_item) {
            config_out.codec.lossless_allow_list.push_back(val);
        } else if (key == "io_ranks" && kv.is_list_item) {
            int rank;
            if (!parse_int(val, rank)) {
                error_out.field_path = key;
                error_out.message = "invalid integer value in io_ranks list: " + val;
                error_out.line = kv.line;
                return AMIO_ERR_MANIFEST_INVALID;
            }
            config_out.io_ranks.push_back(rank);
        }
        // Unknown keys are silently ignored (forward compatibility).
    }

    return AMIO_OK;
}

// ===================================================================
// parse -- load and validate a manifest file.
// ===================================================================

amio_err_t ConfigLoader::parse(const std::string& path,
                               Config& config_out,
                               ValidationError& error_out) {
    // Read file contents.
    std::ifstream file(path);
    if (!file.is_open()) {
        error_out.field_path = "";
        error_out.message = "cannot open manifest file: " + path;
        error_out.line = 0;
        return AMIO_ERR_MANIFEST_NOT_FOUND;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    if (content.empty()) {
        error_out.field_path = "";
        error_out.message = "manifest file is empty: " + path;
        error_out.line = 0;
        return AMIO_ERR_MANIFEST_INVALID;
    }

    // Auto-detect format from extension.
    std::string format = "yaml";
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".json") {
        format = "json";
    }

    return parse_string(content, format, config_out, error_out);
}

// ===================================================================
// parse_string -- parse a manifest from a string.
// ===================================================================

amio_err_t ConfigLoader::parse_string(const std::string& content,
                                      const std::string& format,
                                      Config& config_out,
                                      ValidationError& error_out) {
    // Reset config to defaults.
    config_out = Config{};

    // Tokenize.
    std::vector<KeyValue> tokens;
    if (format == "json") {
        tokens = tokenize_json(content);
    } else {
        tokens = tokenize_yaml(content);
    }

    // Populate config from tokens.
    amio_err_t rc = populate_config(tokens, config_out, error_out);
    if (rc != AMIO_OK) return rc;

    // Validate the populated config.
    return validate(config_out, error_out);
}

// ===================================================================
// serialize -- emit a YAML string from a Config struct.
//
// Round-trip guarantee: parse_string(serialize(config), "yaml") == config
// ===================================================================

std::string ConfigLoader::serialize(const Config& config) {
    std::ostringstream out;

    out << "staging_pool:\n";
    out << "  buffer_count: " << config.staging_pool.buffer_count << "\n";
    out << "  buffer_capacity_bytes: " << config.staging_pool.buffer_capacity_bytes << "\n";

    out << "worker_pool:\n";
    out << "  threads: " << config.worker_pool.threads << "\n";
    if (!config.worker_pool.cpu_cores.empty()) {
        out << "  cpu_cores:\n";
        for (int core : config.worker_pool.cpu_cores) {
            out << "    - " << core << "\n";
        }
    }
    if (config.worker_pool.numa_domain.has_value()) {
        out << "  numa_domain: " << config.worker_pool.numa_domain.value() << "\n";
    }

    out << "prefetch:\n";
    out << "  depth: " << config.prefetch.depth << "\n";
    out << "  read_timeout_s: " << config.prefetch.read_timeout_s << "\n";

    out << "staging_timeout_ms: " << config.staging_timeout_ms << "\n";

    out << "backpressure:\n";
    out << "  low_watermark: " << config.backpressure.low_watermark << "\n";
    out << "  high_watermark: " << config.backpressure.high_watermark << "\n";
    out << "  queue_capacity: " << config.backpressure.queue_capacity << "\n";

    out << "backend: " << config.backend << "\n";

    out << "codec:\n";
    out << "  active_codec: " << config.codec.active_codec << "\n";
    if (!config.codec.lossless_allow_list.empty()) {
        out << "  lossless_allow_list:\n";
        for (const auto& codec : config.codec.lossless_allow_list) {
            out << "    - " << codec << "\n";
        }
    }

    if (!config.io_ranks.empty()) {
        out << "io_ranks:\n";
        for (int rank : config.io_ranks) {
            out << "  - " << rank << "\n";
        }
    }

    return out.str();
}

}  // namespace amio::detail
