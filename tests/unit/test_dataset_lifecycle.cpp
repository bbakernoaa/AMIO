// test_dataset_lifecycle.cpp
//
// Unit tests for the dataset open/close lifecycle implemented in
// task 6.3.  These tests exercise the FFI surface as a host
// application would: link against `libamio.so`, include only the
// public header, never call into private C++.
//
// Tests cover:
//   - amio_open_dataset argument validation (NULL checks, invalid mode)
//   - amio_open_dataset with unknown backend → AMIO_ERR_UNKNOWN_BACKEND
//   - amio_open_dataset with valid registered backend → success
//   - amio_close_dataset on a valid dataset handle → success
//   - amio_close_dataset on a stale handle → AMIO_ERR_INVALID_HANDLE
//   - amio_flush on a dataset with no pending writes → AMIO_OK
//   - amio_close (alias) on a valid dataset handle → success
//
// Validates: R4.1, R4.6, R6.4, R6.5

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "amio/amio.h"

// Private headers for test setup (registering a mock driver).
#include "c_boundary/handle_table.hpp"
#include "factory/backend_driver.hpp"
#include "factory/backend_factory.hpp"

namespace {

struct TestResult {
    int passed = 0;
    int failed = 0;
};

TestResult g_result{};

void report_failure(const char *expr, const char *file, int line, const std::string &context) {
    std::fprintf(stderr, "FAIL %s:%d: %s   (%s)\n", file, line, expr, context.c_str());
    ++g_result.failed;
}

#define EXPECT_TRUE(cond, ctx)                                \
    do {                                                      \
        if (!(cond)) {                                        \
            report_failure(#cond, __FILE__, __LINE__, (ctx)); \
        } else {                                              \
            ++g_result.passed;                                \
        }                                                     \
    } while (0)

#define EXPECT_EQ(a, b, ctx)                                                                                             \
    do {                                                                                                                 \
        if ((a) != (b)) {                                                                                                \
            char buf[256];                                                                                               \
            std::snprintf(buf, sizeof(buf), "%s: expected %d, got %d", (ctx), static_cast<int>(b), static_cast<int>(a)); \
            report_failure(#a " == " #b, __FILE__, __LINE__, buf);                                                       \
        } else {                                                                                                         \
            ++g_result.passed;                                                                                           \
        }                                                                                                                \
    } while (0)

// ---------------------------------------------------------------
// MockDriver -- a minimal Backend_Driver for testing the lifecycle.
// ---------------------------------------------------------------
class MockDriver : public amio::detail::Backend_Driver {
   public:
    bool opened_write = false;
    bool opened_read = false;
    bool flushed = false;
    bool closed = false;

    void open_write(const eckit::Configuration &) override {
        opened_write = true;
    }
    void open_read(const eckit::Configuration &) override {
        opened_read = true;
    }
    void write(const amio::detail::StagingBuffer &, const amio::detail::VarMeta &) override {}
    void read(amio::detail::StagingBuffer &, const amio::detail::VarMeta &, std::int64_t, const std::optional<amio::detail::BoundingBox> &) override {
    }
    void flush() override {
        flushed = true;
    }
    void close() override {
        closed = true;
    }
};

// Helper: write a minimal YAML config file with a given backend key.
std::string write_config_file(const std::string &backend_key, const std::string &suffix = "") {
    std::string path = "/tmp/amio_test_config" + suffix + ".yaml";
    std::ofstream ofs(path);
    ofs << "backend: " << backend_key << "\n";
    ofs << "staging_pool:\n";
    ofs << "  buffer_count: 4\n";
    ofs << "  buffer_capacity_bytes: 1024\n";
    ofs << "worker_pool:\n";
    ofs << "  threads: 1\n";
    ofs << "prefetch:\n";
    ofs << "  depth: 4\n";
    ofs << "  read_timeout_s: 60\n";
    ofs << "staging_timeout_ms: 5000\n";
    ofs << "codec:\n";
    ofs << "  active_codec: blosc\n";
    ofs << "  lossless_allow_list:\n";
    ofs << "    - blosc\n";
    ofs.close();
    return path;
}

// ---------------------------------------------------------------
// Test: NULL argument validation for amio_open_dataset
// ---------------------------------------------------------------
void test_open_dataset_null_arguments() {
    amio_dataset_handle ds = reinterpret_cast<amio_dataset_handle>(0xDEAD);

    // NULL out_dataset
    int rc = amio_open_dataset(nullptr, "path.yaml", AMIO_MODE_WRITE, nullptr);
    EXPECT_EQ(rc, AMIO_ERR_INVALID_INPUT, "amio_open_dataset(NULL out_dataset)");

    // NULL config_path
    rc = amio_open_dataset(nullptr, nullptr, AMIO_MODE_WRITE, &ds);
    EXPECT_EQ(rc, AMIO_ERR_INVALID_INPUT, "amio_open_dataset(NULL config_path)");
    EXPECT_TRUE(ds == nullptr, "out_dataset should be zeroed on failure");

    // Invalid mode
    ds = reinterpret_cast<amio_dataset_handle>(0xDEAD);
    amio_core_handle core = nullptr;
    amio_init("dummy.yaml", &core);  // Create a core for mode test
    if (core) {
        rc = amio_open_dataset(core, "path.yaml", 99, &ds);
        EXPECT_EQ(rc, AMIO_ERR_INVALID_INPUT, "amio_open_dataset(invalid mode)");
        EXPECT_TRUE(ds == nullptr, "out_dataset should be zeroed on invalid mode");
        amio_finalize(core);
    }

    // NULL core handle
    ds = reinterpret_cast<amio_dataset_handle>(0xDEAD);
    rc = amio_open_dataset(nullptr, "path.yaml", AMIO_MODE_WRITE, &ds);
    EXPECT_EQ(rc, AMIO_ERR_NULL_HANDLE, "amio_open_dataset(NULL core)");
    EXPECT_TRUE(ds == nullptr, "out_dataset should be zeroed on NULL core");
}

// ---------------------------------------------------------------
// Test: Unknown backend returns AMIO_ERR_UNKNOWN_BACKEND
// ---------------------------------------------------------------
void test_open_dataset_unknown_backend() {
    // Write a config with an unregistered backend key.
    std::string path = write_config_file("nonexistent_backend", "_unknown");

    amio_core_handle core = nullptr;
    amio_init("dummy.yaml", &core);
    EXPECT_TRUE(core != nullptr, "amio_init should succeed");

    amio_dataset_handle ds = nullptr;
    int rc = amio_open_dataset(core, path.c_str(), AMIO_MODE_WRITE, &ds);
    EXPECT_EQ(rc, AMIO_ERR_UNKNOWN_BACKEND, "open with unknown backend");
    EXPECT_TRUE(ds == nullptr, "no handle created on unknown backend");

    amio_finalize(core);
}

// ---------------------------------------------------------------
// Test: Successful open with a registered mock backend
// ---------------------------------------------------------------
void test_open_dataset_success() {
    // Register a mock driver.
    amio::detail::BackendFactory::instance().register_driver(
        "mock_test", []() -> std::unique_ptr<amio::detail::Backend_Driver> { return std::make_unique<MockDriver>(); });

    std::string path = write_config_file("mock_test", "_success");

    amio_core_handle core = nullptr;
    amio_init("dummy.yaml", &core);
    EXPECT_TRUE(core != nullptr, "amio_init should succeed");

    // Open for write.
    amio_dataset_handle ds = nullptr;
    int rc = amio_open_dataset(core, path.c_str(), AMIO_MODE_WRITE, &ds);
    EXPECT_EQ(rc, AMIO_OK, "open_dataset with mock_test backend");
    EXPECT_TRUE(ds != nullptr, "dataset handle should be non-null");

    // Open a second dataset for read (R4.7: concurrent read+write).
    amio_dataset_handle ds2 = nullptr;
    rc = amio_open_dataset(core, path.c_str(), AMIO_MODE_READ, &ds2);
    EXPECT_EQ(rc, AMIO_OK, "open second dataset for read");
    EXPECT_TRUE(ds2 != nullptr, "second dataset handle should be non-null");
    EXPECT_TRUE(ds != ds2, "two datasets should have different handles");

    // Close both.
    rc = amio_close_dataset(ds);
    EXPECT_EQ(rc, AMIO_OK, "close_dataset write");

    rc = amio_close_dataset(ds2);
    EXPECT_EQ(rc, AMIO_OK, "close_dataset read");

    amio_finalize(core);

    // Cleanup factory.
    amio::detail::BackendFactory::instance().clear();
}

// ---------------------------------------------------------------
// Test: close_dataset on stale handle returns AMIO_ERR_INVALID_HANDLE
// ---------------------------------------------------------------
void test_close_dataset_stale_handle() {
    amio::detail::BackendFactory::instance().register_driver(
        "mock_stale", []() -> std::unique_ptr<amio::detail::Backend_Driver> { return std::make_unique<MockDriver>(); });

    std::string path = write_config_file("mock_stale", "_stale");

    amio_core_handle core = nullptr;
    amio_init("dummy.yaml", &core);
    EXPECT_TRUE(core != nullptr, "amio_init should succeed");

    amio_dataset_handle ds = nullptr;
    int rc = amio_open_dataset(core, path.c_str(), AMIO_MODE_WRITE, &ds);
    EXPECT_EQ(rc, AMIO_OK, "open_dataset");

    // Close it once.
    rc = amio_close_dataset(ds);
    EXPECT_EQ(rc, AMIO_OK, "first close");

    // Close it again — should be stale.
    rc = amio_close_dataset(ds);
    EXPECT_EQ(rc, AMIO_ERR_INVALID_HANDLE, "double close");

    amio_finalize(core);
    amio::detail::BackendFactory::instance().clear();
}

// ---------------------------------------------------------------
// Test: amio_flush on dataset with no pending writes → AMIO_OK
// ---------------------------------------------------------------
void test_flush_no_pending_writes() {
    amio::detail::BackendFactory::instance().register_driver(
        "mock_flush", []() -> std::unique_ptr<amio::detail::Backend_Driver> { return std::make_unique<MockDriver>(); });

    std::string path = write_config_file("mock_flush", "_flush");

    amio_core_handle core = nullptr;
    amio_init("dummy.yaml", &core);
    EXPECT_TRUE(core != nullptr, "amio_init should succeed");

    amio_dataset_handle ds = nullptr;
    int rc = amio_open_dataset(core, path.c_str(), AMIO_MODE_WRITE, &ds);
    EXPECT_EQ(rc, AMIO_OK, "open_dataset");

    // Flush with no pending writes.
    rc = amio_flush(ds, /*timeout_ms=*/1000);
    EXPECT_EQ(rc, AMIO_OK, "flush with no pending writes");

    rc = amio_close_dataset(ds);
    EXPECT_EQ(rc, AMIO_OK, "close after flush");

    amio_finalize(core);
    amio::detail::BackendFactory::instance().clear();
}

// ---------------------------------------------------------------
// Test: amio_close (the existing entry point) works as close_dataset
// ---------------------------------------------------------------
void test_amio_close_as_close_dataset() {
    amio::detail::BackendFactory::instance().register_driver(
        "mock_close", []() -> std::unique_ptr<amio::detail::Backend_Driver> { return std::make_unique<MockDriver>(); });

    std::string path = write_config_file("mock_close", "_close");

    amio_core_handle core = nullptr;
    amio_init("dummy.yaml", &core);
    EXPECT_TRUE(core != nullptr, "amio_init should succeed");

    amio_dataset_handle ds = nullptr;
    int rc = amio_open_dataset(core, path.c_str(), AMIO_MODE_WRITE, &ds);
    EXPECT_EQ(rc, AMIO_OK, "open_dataset");

    // Use amio_close (the existing entry point).
    rc = amio_close(ds);
    EXPECT_EQ(rc, AMIO_OK, "amio_close on dataset");

    // Verify handle is now stale.
    rc = amio_flush(ds, 0);
    EXPECT_EQ(rc, AMIO_ERR_INVALID_HANDLE, "flush on closed handle");

    amio_finalize(core);
    amio::detail::BackendFactory::instance().clear();
}

// ---------------------------------------------------------------
// Test: amio_close_dataset NULL handle
// ---------------------------------------------------------------
void test_close_dataset_null_handle() {
    int rc = amio_close_dataset(nullptr);
    EXPECT_EQ(rc, AMIO_ERR_NULL_HANDLE, "close_dataset(NULL)");
}

// ---------------------------------------------------------------
// Test: amio_open_dataset with garbage core handle
// ---------------------------------------------------------------
void test_open_dataset_garbage_core() {
    auto bogus = reinterpret_cast<amio_core_handle>(static_cast<std::uintptr_t>(0xBADBADBADBADBADull));
    amio_dataset_handle ds = nullptr;
    int rc = amio_open_dataset(bogus, "path.yaml", AMIO_MODE_WRITE, &ds);
    EXPECT_EQ(rc, AMIO_ERR_INVALID_HANDLE, "open_dataset with garbage core");
    EXPECT_TRUE(ds == nullptr, "no handle on garbage core");
}

}  // namespace

int main() {
    test_open_dataset_null_arguments();
    test_open_dataset_unknown_backend();
    test_open_dataset_success();
    test_close_dataset_stale_handle();
    test_flush_no_pending_writes();
    test_amio_close_as_close_dataset();
    test_close_dataset_null_handle();
    test_open_dataset_garbage_core();

    std::fprintf(stdout, "test_dataset_lifecycle: passed=%d failed=%d\n", g_result.passed, g_result.failed);

    return g_result.failed == 0 ? 0 : 1;
}
