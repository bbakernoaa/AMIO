// comm_split.cpp -- AMIO MPI communicator split implementation.
//
// When eckit::mpi is available (AMIO_HAS_ECKIT and MPI linked),
// delegates to eckit::mpi::comm("world").split(...).  When MPI is
// available directly (AMIO_HAS_MPI), uses MPI_Comm_split.  When
// neither is available, the functions still compile: default configs
// succeed, non-default configs return AMIO_ERR_COMM_SPLIT_FAILED.
//
// Validates: R3.5, R3.6

#include "workers/comm_split.hpp"

#include <algorithm>
#include <set>

// MPI availability detection.
// In a full build, AMIO_HAS_MPI would be set by CMake when MPI is found.
// For standalone compilation without MPI, the functions degrade gracefully.
#if defined(AMIO_HAS_ECKIT) && defined(AMIO_HAS_MPI)
#include <eckit/mpi/Comm.h>
#define AMIO_CAN_SPLIT 1
#elif defined(AMIO_HAS_MPI)
#include <mpi.h>
#define AMIO_CAN_SPLIT 1
#else
#define AMIO_CAN_SPLIT 0
#endif

namespace amio::detail {

amio_err_t validate_comm_config(const CommConfig& config) {
    if (config.is_default()) {
        return AMIO_OK;
    }

    // Must have a known world size to validate ranks.
    if (config.world_size <= 0) {
        return AMIO_ERR_COMM_SPLIT_FAILED;
    }

    // io_ranks must not be empty (already checked by is_default).
    // io_ranks must be a proper subset: not all ranks can be I/O.
    if (static_cast<int>(config.io_ranks.size()) >= config.world_size) {
        return AMIO_ERR_COMM_SPLIT_FAILED;
    }

    // Check for duplicates and out-of-range values.
    std::set<int> seen;
    for (int rank : config.io_ranks) {
        if (rank < 0 || rank >= config.world_size) {
            return AMIO_ERR_COMM_SPLIT_FAILED;
        }
        if (!seen.insert(rank).second) {
            // Duplicate rank.
            return AMIO_ERR_COMM_SPLIT_FAILED;
        }
    }

    return AMIO_OK;
}

amio_err_t split_communicator(const CommConfig& config, int my_rank, IOCommunicator& result) {
    // Default: no split requested.  All ranks participate in I/O.
    if (config.is_default()) {
        result.valid = true;
        result.is_io_rank = true;
        result.io_comm_id = 0;  // Represents "world" / no split
        result.compute_comm_id = 0;
        return AMIO_OK;
    }

    // Validate the configuration.
    amio_err_t validation = validate_comm_config(config);
    if (validation != AMIO_OK) {
        return validation;
    }

    // Validate my_rank is within world_size.
    if (my_rank < 0 || my_rank >= config.world_size) {
        return AMIO_ERR_COMM_SPLIT_FAILED;
    }

    // Determine if this rank is in the I/O set.
    bool is_io = std::find(config.io_ranks.begin(), config.io_ranks.end(), my_rank) != config.io_ranks.end();

#if AMIO_CAN_SPLIT

#if defined(AMIO_HAS_ECKIT)
    // Use eckit::mpi for the split.
    // Color: 0 = I/O ranks, 1 = compute ranks.
    int color = is_io ? 0 : 1;
    try {
        eckit::mpi::Comm& world = eckit::mpi::comm("world");
        eckit::mpi::Comm& split_comm = world.split(color, my_rank);

        result.valid = true;
        result.is_io_rank = is_io;
        // Store the communicator identifier.  In eckit, the Comm
        // object is managed by the library; we store a sentinel.
        result.io_comm_id = is_io ? 1 : 0;
        result.compute_comm_id = is_io ? 0 : 1;
        return AMIO_OK;
    } catch (...) {
        // eckit split failed -- leave world communicator unmodified.
        return AMIO_ERR_COMM_SPLIT_FAILED;
    }

#else  // AMIO_HAS_MPI but not eckit
    // Use raw MPI_Comm_split.
    int color = is_io ? 0 : 1;
    MPI_Comm new_comm = MPI_COMM_NULL;
    int rc = MPI_Comm_split(MPI_COMM_WORLD, color, my_rank, &new_comm);
    if (rc != MPI_SUCCESS || new_comm == MPI_COMM_NULL) {
        return AMIO_ERR_COMM_SPLIT_FAILED;
    }

    result.valid = true;
    result.is_io_rank = is_io;
    result.io_comm_id = static_cast<int64_t>(new_comm);
    result.compute_comm_id = static_cast<int64_t>(new_comm);
    return AMIO_OK;
#endif

#else  // !AMIO_CAN_SPLIT
    // MPI is not available.  Non-default configs cannot be split.
    // Return error, leaving world communicator unmodified (R3.6).
    (void)is_io;
    return AMIO_ERR_COMM_SPLIT_FAILED;
#endif
}

}  // namespace amio::detail
