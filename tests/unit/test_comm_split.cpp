// test_comm_split.cpp
//
// Unit tests for `amio::detail::split_communicator` and
// `amio::detail::validate_comm_config` covering the MPI communicator
// split abstraction layer.
//
// Because comm_split is private to the AMIO_Core build (its header
// lives under `src/workers/`), this test target compiles
// `comm_split.cpp` directly into the test binary.
//
// Test scope:
//
//   * Default config (no split) returns AMIO_OK with valid result
//     and is_io_rank=true (R3.5 all-ranks-do-I/O case).
//   * Empty io_ranks is treated as default.
//   * Invalid configs return AMIO_ERR_COMM_SPLIT_FAILED:
//     - Ranks out of range [0, world_size)
//     - Duplicate ranks
//     - io_ranks == all ranks (not a proper subset)
//     - world_size <= 0
//   * Non-default config without MPI returns AMIO_ERR_COMM_SPLIT_FAILED
//     (graceful degradation when MPI is not linked).
//   * validate_comm_config independently validates without performing
//     the actual split.

#include <cassert>
#include <cstdio>
#include <string>

#include "workers/comm_split.hpp"

namespace {

using amio::detail::CommConfig;
using amio::detail::IOCommunicator;
using amio::detail::is_io_rank;
using amio::detail::split_communicator;
using amio::detail::validate_comm_config;

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

// ---- Test: default config returns OK ----

void test_default_config_returns_ok() {
    CommConfig config;  // empty io_ranks
    EXPECT_TRUE(config.is_default(), "default config should report is_default");

    IOCommunicator result{};
    amio_err_t rc = split_communicator(config, 0, result);
    EXPECT_TRUE(rc == AMIO_OK, "default config should return AMIO_OK, got " + std::to_string(rc));
    EXPECT_TRUE(result.valid, "result should be valid for default config");
    EXPECT_TRUE(result.is_io_rank, "all ranks should be I/O ranks in default mode");
}

// ---- Test: explicit empty io_ranks is default ----

void test_explicit_empty_io_ranks() {
    CommConfig config;
    config.io_ranks = {};
    config.world_size = 4;

    EXPECT_TRUE(config.is_default(), "empty io_ranks should be is_default");

    IOCommunicator result{};
    amio_err_t rc = split_communicator(config, 0, result);
    EXPECT_TRUE(rc == AMIO_OK, "empty io_ranks should return AMIO_OK");
    EXPECT_TRUE(result.valid, "result should be valid");
    EXPECT_TRUE(result.is_io_rank, "should be I/O rank in default mode");
}

// ---- Test: validate_comm_config with valid config ----

void test_validate_valid_config() {
    CommConfig config;
    config.io_ranks = {0, 1};
    config.world_size = 4;

    amio_err_t rc = validate_comm_config(config);
    EXPECT_TRUE(rc == AMIO_OK, "valid config should pass validation, got " + std::to_string(rc));
}

// ---- Test: validate_comm_config with default config ----

void test_validate_default_config() {
    CommConfig config;
    amio_err_t rc = validate_comm_config(config);
    EXPECT_TRUE(rc == AMIO_OK, "default config should pass validation");
}

// ---- Test: rank out of range (negative) ----

void test_rank_out_of_range_negative() {
    CommConfig config;
    config.io_ranks = {-1, 0};
    config.world_size = 4;

    amio_err_t rc = validate_comm_config(config);
    EXPECT_TRUE(rc == AMIO_ERR_COMM_SPLIT_FAILED, "negative rank should fail validation, got " + std::to_string(rc));

    IOCommunicator result{};
    rc = split_communicator(config, 0, result);
    EXPECT_TRUE(rc == AMIO_ERR_COMM_SPLIT_FAILED, "negative rank should fail split");
    EXPECT_TRUE(!result.valid, "result should not be valid on failure");
}

// ---- Test: rank out of range (>= world_size) ----

void test_rank_out_of_range_too_large() {
    CommConfig config;
    config.io_ranks = {0, 4};  // world_size is 4, so rank 4 is invalid
    config.world_size = 4;

    amio_err_t rc = validate_comm_config(config);
    EXPECT_TRUE(rc == AMIO_ERR_COMM_SPLIT_FAILED, "rank >= world_size should fail validation, got " + std::to_string(rc));
}

// ---- Test: duplicate ranks ----

void test_duplicate_ranks() {
    CommConfig config;
    config.io_ranks = {0, 1, 1};
    config.world_size = 4;

    amio_err_t rc = validate_comm_config(config);
    EXPECT_TRUE(rc == AMIO_ERR_COMM_SPLIT_FAILED, "duplicate ranks should fail validation, got " + std::to_string(rc));
}

// ---- Test: io_ranks == all ranks (not a proper subset) ----

void test_all_ranks_as_io() {
    CommConfig config;
    config.io_ranks = {0, 1, 2, 3};
    config.world_size = 4;

    amio_err_t rc = validate_comm_config(config);
    EXPECT_TRUE(rc == AMIO_ERR_COMM_SPLIT_FAILED, "all ranks as I/O should fail (not proper subset), got " + std::to_string(rc));
}

// ---- Test: world_size <= 0 ----

void test_zero_world_size() {
    CommConfig config;
    config.io_ranks = {0};
    config.world_size = 0;

    amio_err_t rc = validate_comm_config(config);
    EXPECT_TRUE(rc == AMIO_ERR_COMM_SPLIT_FAILED, "world_size=0 should fail validation, got " + std::to_string(rc));
}

void test_negative_world_size() {
    CommConfig config;
    config.io_ranks = {0};
    config.world_size = -1;

    amio_err_t rc = validate_comm_config(config);
    EXPECT_TRUE(rc == AMIO_ERR_COMM_SPLIT_FAILED, "negative world_size should fail validation, got " + std::to_string(rc));
}

// ---- Test: non-default config without MPI returns error ----

void test_non_default_without_mpi() {
    // This test exercises the graceful degradation path.
    // Without MPI linked, a non-default config should return
    // AMIO_ERR_COMM_SPLIT_FAILED.
    CommConfig config;
    config.io_ranks = {0, 1};
    config.world_size = 4;

    IOCommunicator result{};
    amio_err_t rc = split_communicator(config, 0, result);

    // On a system without MPI (which is our test environment),
    // this should fail gracefully.
#if !defined(AMIO_HAS_MPI) && !defined(AMIO_HAS_ECKIT)
    EXPECT_TRUE(rc == AMIO_ERR_COMM_SPLIT_FAILED,
                "non-default config without MPI should return "
                "AMIO_ERR_COMM_SPLIT_FAILED, got " +
                    std::to_string(rc));
    EXPECT_TRUE(!result.valid, "result should not be valid without MPI");
#else
    // If MPI is available, the split might succeed.
    EXPECT_TRUE(rc == AMIO_OK || rc == AMIO_ERR_COMM_SPLIT_FAILED, "with MPI, split should either succeed or fail gracefully");
#endif
}

// ---- Test: invalid my_rank returns error ----

void test_invalid_my_rank() {
    CommConfig config;
    config.io_ranks = {0};
    config.world_size = 4;

    IOCommunicator result{};

    // my_rank negative
    amio_err_t rc = split_communicator(config, -1, result);
    EXPECT_TRUE(rc == AMIO_ERR_COMM_SPLIT_FAILED, "negative my_rank should fail, got " + std::to_string(rc));

    // my_rank >= world_size
    rc = split_communicator(config, 4, result);
    EXPECT_TRUE(rc == AMIO_ERR_COMM_SPLIT_FAILED, "my_rank >= world_size should fail, got " + std::to_string(rc));
}

// ---- Test: single I/O rank in multi-rank world ----

void test_single_io_rank_validation() {
    CommConfig config;
    config.io_ranks = {3};
    config.world_size = 8;

    amio_err_t rc = validate_comm_config(config);
    EXPECT_TRUE(rc == AMIO_OK, "single I/O rank in 8-rank world should validate, got " + std::to_string(rc));
}

// ---- Test: IOCommunicator default state ----

void test_io_communicator_default_state() {
    IOCommunicator comm;
    EXPECT_TRUE(!comm.valid, "default IOCommunicator should not be valid");
    EXPECT_TRUE(!comm.is_io_rank, "default should not be I/O rank");
    EXPECT_TRUE(comm.io_comm_id == 0, "default io_comm_id should be 0");
    EXPECT_TRUE(comm.compute_comm_id == 0, "default compute_comm_id should be 0");
}

// ---- Test: is_io_rank helper with default config ----

void test_is_io_rank_default_config() {
    CommConfig config;  // default: all ranks do I/O
    EXPECT_TRUE(is_io_rank(config, 0), "default config: rank 0 should be I/O rank");
    EXPECT_TRUE(is_io_rank(config, 99), "default config: any rank should be I/O rank");
}

// ---- Test: is_io_rank helper with non-default config ----

void test_is_io_rank_non_default_config() {
    CommConfig config;
    config.io_ranks = {1, 3};
    config.world_size = 4;

    EXPECT_TRUE(!is_io_rank(config, 0), "rank 0 should NOT be I/O rank");
    EXPECT_TRUE(is_io_rank(config, 1), "rank 1 should be I/O rank");
    EXPECT_TRUE(!is_io_rank(config, 2), "rank 2 should NOT be I/O rank");
    EXPECT_TRUE(is_io_rank(config, 3), "rank 3 should be I/O rank");
}

}  // namespace

int main() {
    test_default_config_returns_ok();
    test_explicit_empty_io_ranks();
    test_validate_valid_config();
    test_validate_default_config();
    test_rank_out_of_range_negative();
    test_rank_out_of_range_too_large();
    test_duplicate_ranks();
    test_all_ranks_as_io();
    test_zero_world_size();
    test_negative_world_size();
    test_non_default_without_mpi();
    test_invalid_my_rank();
    test_single_io_rank_validation();
    test_io_communicator_default_state();
    test_is_io_rank_default_config();
    test_is_io_rank_non_default_config();

    std::fprintf(stdout, "test_comm_split: passed=%d failed=%d\n", g_result.passed, g_result.failed);

    return g_result.failed == 0 ? 0 : 1;
}
