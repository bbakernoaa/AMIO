// backend_factory.cpp -- AMIO Backend_Factory implementation.
//
// Implements the singleton string-keyed registry for Backend_Driver
// implementations.  Mimics eckit::Factory<Backend_Driver> semantics:
//
//   * Case-sensitive exact-match lookup (R4.1).
//   * Static-init registration via BackendRegistrar<T> (R4.2).
//   * Unknown/missing/wrong-case keys → AMIO_ERR_UNKNOWN_BACKEND
//     with zero state mutation (R4.6).
//   * Thread-safe registration and lookup via std::shared_mutex.
//   * Supports concurrent read + write datasets on different drivers
//     within a single AMIO_Core (R4.7) — each build() call returns
//     an independent driver instance.
//
// Validates: R4.1, R4.2, R4.3, R4.4, R4.5, R4.6, R4.7, R4.8

#include "factory/backend_factory.hpp"

#include <algorithm>
#include <mutex>

namespace amio::detail {

// Singleton instance -- Meyers' singleton, thread-safe per C++11.
BackendFactory& BackendFactory::instance() {
    static BackendFactory factory;
    return factory;
}

bool BackendFactory::register_driver(const std::string& key,
                                     BuilderFn builder) {
    if (key.empty()) {
        return false;
    }
    std::unique_lock lock(mu_);
    registry_[key] = std::move(builder);
    return true;
}

std::unique_ptr<Backend_Driver> BackendFactory::build(
    const std::string& key,
    amio_err_t& err_out) const {

    // Empty or missing key → error, zero state mutation.
    if (key.empty()) {
        err_out = AMIO_ERR_UNKNOWN_BACKEND;
        return nullptr;
    }

    std::shared_lock lock(mu_);
    auto it = registry_.find(key);
    if (it == registry_.end()) {
        // Key not found — case-sensitive exact-match failed.
        err_out = AMIO_ERR_UNKNOWN_BACKEND;
        return nullptr;
    }

    // Found — invoke the builder to create a fresh driver instance.
    // Each call produces an independent instance, supporting
    // concurrent read + write datasets on different drivers (R4.7).
    err_out = AMIO_OK;
    return it->second();
}

bool BackendFactory::has(const std::string& key) const {
    if (key.empty()) {
        return false;
    }
    std::shared_lock lock(mu_);
    return registry_.find(key) != registry_.end();
}

std::vector<std::string> BackendFactory::registered_keys() const {
    std::shared_lock lock(mu_);
    std::vector<std::string> keys;
    keys.reserve(registry_.size());
    for (const auto& [k, _] : registry_) {
        keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

void BackendFactory::clear() {
    std::unique_lock lock(mu_);
    registry_.clear();
}

}  // namespace amio::detail
