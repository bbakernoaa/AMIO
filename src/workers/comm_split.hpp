// comm_split.hpp -- AMIO MPI communicator split abstraction.
//
// This header is PRIVATE to the AMIO_Core build (`src/workers/`).
// It is never installed and must not be referenced from any public
// header under `include/amio/`.
//
// Purpose
// -------
// Provides an abstraction layer for splitting the MPI world
// communicator into a compute communicator and a dedicated I/O
// communicator.  When eckit::mpi is available, the implementation
// delegates to `eckit::mpi::comm("world").split(...)`.  When eckit
// or MPI is not linked, the functions still compile but return
// appropriate error codes for invalid configs and succeed for
// empty/default configs.
//
// Design contract:
//   * All Backend_Driver MPI calls are routed through the I/O
//     communicator only (R3.5).
//   * Failed split returns AMIO_ERR_COMM_SPLIT_FAILED and leaves
//     the world communicator unmodified (R3.6).
//
// Thread safety
// -------------
// split_communicator() should be called once during initialization
// (before Worker_Pool threads are spawned).  The resulting
// IOCommunicator is then shared read-only across worker threads.
//
// Validates: R3.5, R3.6

#ifndef AMIO_SRC_WORKERS_COMM_SPLIT_HPP
#define AMIO_SRC_WORKERS_COMM_SPLIT_HPP

#include <cstdint>
#include <vector>

#include "amio/amio_errors.h"

namespace amio::detail {

// CommConfig -- configuration for MPI communicator splitting.
//
// Fields:
//   io_ranks - set of MPI world ranks designated as I/O ranks.
//              An empty set means "no communicator split" (all
//              ranks participate in both compute and I/O).
//   world_size - total number of ranks in MPI_COMM_WORLD.
//                Used for validation.  A value of 0 means
//                "unknown / MPI not initialized".
struct CommConfig {
    std::vector<int> io_ranks;
    int              world_size = 0;

    // Returns true if no communicator split is requested.
    bool is_default() const noexcept {
        return io_ranks.empty();
    }
};

// IOCommunicator -- result of a successful communicator split.
//
// This is an opaque handle that Backend_Driver MPI calls should
// use instead of MPI_COMM_WORLD.  When eckit/MPI is available,
// this wraps the split communicator.  When not available, it
// represents a "no-op" communicator that signals single-rank
// operation.
//
// Fields:
//   valid       - true if the split was performed successfully
//   is_io_rank  - true if the current rank is in the I/O set
//   io_comm_id  - opaque identifier for the I/O communicator
//                 (MPI_Comm value when MPI is available, or 0)
//   compute_comm_id - opaque identifier for the compute communicator
struct IOCommunicator {
    bool     valid          = false;
    bool     is_io_rank     = false;
    int64_t  io_comm_id     = 0;
    int64_t  compute_comm_id = 0;
};

// split_communicator -- split the world communicator into compute
// and I/O communicators based on the provided configuration.
//
// Behavior:
//   * If config.is_default() is true, the function is a no-op:
//     returns AMIO_OK and sets `result.valid = true` with
//     `result.is_io_rank = true` (all ranks do I/O in this mode).
//
//   * If io_ranks is non-empty, validates that:
//     - All ranks in io_ranks are in [0, world_size)
//     - io_ranks is a proper subset of the world (not all ranks)
//     - No duplicate ranks
//     If validation fails, returns AMIO_ERR_COMM_SPLIT_FAILED and
//     leaves the world communicator unmodified.
//
//   * If validation passes and MPI is available, performs the
//     communicator split via eckit::mpi::comm("world").split(...)
//     or MPI_Comm_split.  On MPI failure, returns
//     AMIO_ERR_COMM_SPLIT_FAILED.
//
//   * If MPI is not available (standalone build), returns AMIO_OK
//     for default configs and AMIO_ERR_COMM_SPLIT_FAILED for
//     non-default configs (cannot split without MPI).
//
// Parameters:
//   config - the communicator split configuration
//   my_rank - the calling process's rank in MPI_COMM_WORLD
//             (ignored when config.is_default())
//   result - [out] populated on success with communicator info
//
// Returns:
//   AMIO_OK on success.
//   AMIO_ERR_COMM_SPLIT_FAILED if the split fails or the I/O rank
//   set is invalid.
amio_err_t split_communicator(const CommConfig& config,
                              int my_rank,
                              IOCommunicator& result);

// validate_comm_config -- check that the CommConfig is internally
// consistent without performing the actual MPI split.
//
// Returns AMIO_OK if valid, AMIO_ERR_COMM_SPLIT_FAILED if not.
amio_err_t validate_comm_config(const CommConfig& config);

// is_io_rank -- convenience check: returns true if the given rank
// is in the I/O rank set, or if the config is default (all ranks
// do I/O).
inline bool is_io_rank(const CommConfig& config, int my_rank) noexcept {
    if (config.is_default()) {
        return true;
    }
    for (int r : config.io_ranks) {
        if (r == my_rank) return true;
    }
    return false;
}

}  // namespace amio::detail

#endif  // AMIO_SRC_WORKERS_COMM_SPLIT_HPP
