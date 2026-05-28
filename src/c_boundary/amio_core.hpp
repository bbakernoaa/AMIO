// amio_core.hpp -- Private C++ entry points called by the C-Boundary.
//
// This header is PRIVATE to the AMIO_Core build and is never installed.
// It bridges the `extern "C"` AMIO_C_API entry points (in
// `src/c_boundary/amio_api.cpp`) and the C++ implementations of
// `amio_init`, the dataset / I/O paths, and `amio_finalize` that
// land in tasks 4.x, 6.x, and 9.x.
//
// Each function below is the post-handle-validation, post-exception-
// translation entry into the C++ private API.  The C-Boundary calls
// them under a try/catch cordon (see amio_api.cpp) so they are free
// to throw `eckit::Exception`, `std::exception`, or any other type
// without the host application ever observing a C++ exception.
//
// Validates: R10.5, R10.6, R10.7, R12.2

#ifndef AMIO_SRC_C_BOUNDARY_AMIO_CORE_HPP
#define AMIO_SRC_C_BOUNDARY_AMIO_CORE_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "amio/amio_errors.h"
#include "amio/amio_types.h"
#include "c_boundary/handle_table.hpp"
#include "factory/backend_driver.hpp"
#include "prefetch/prefetch_queue.hpp"

namespace amio::detail {

// Forward declarations.
class WorkerPool;
class PrefetchQueue;
class StagingPool;
struct StagingBuffer;
struct AMIO_Core;

// ---------------------------------------------------------------
// IoRecord -- internal state for a pending asynchronous I/O op.
//
// Created by write() and tracked in the handle table as an Io
// handle.  The worker pool signals completion or failure.
// ---------------------------------------------------------------
struct IoRecord {
    std::uint64_t dataset_id = 0;
    std::uint64_t dv_seq = 0;  // per-(dataset,variable) sequence
    HandleTable::Token token = 0;
    StagingBuffer *staging_buf = nullptr;  // non-owning; pool owns the buffer
    AMIO_Core *core = nullptr;             // back-pointer for pool release

    // Completion state.
    std::atomic<bool> completed{false};
    std::atomic<bool> failed{false};
    amio_err_t failure_code = AMIO_OK;
};

// ---------------------------------------------------------------
// ViewRecord -- internal state for an outstanding read view.
//
// Created by amio_read when a prefetched buffer is returned to the
// host.  The view holds a reference to the staging buffer (via
// ref_count) and is released by amio_release_view.
// ---------------------------------------------------------------
struct ViewRecord {
    StagingBuffer *staging_buf = nullptr;  // non-owning; pool owns the buffer
    HandleTable::Token token = 0;
    AMIO_Core *core = nullptr;  // back-pointer for pool release
    std::uint64_t dataset_id = 0;
    std::int64_t timestep = -1;
};

// ---------------------------------------------------------------
// DatasetRecord -- internal state for an open dataset.
//
// Holds the Backend_Driver instance, the dataset's mode (read or
// write), and the handle table token so that close can release it.
// Also tracks pending write count for flush semantics.
// ---------------------------------------------------------------
struct DatasetRecord {
    std::unique_ptr<Backend_Driver> driver;
    std::int32_t mode = AMIO_MODE_WRITE;
    HandleTable::Token token = 0;
    std::uint64_t dataset_id = 0;

    // Back-pointer to the owning AMIO_Core (non-owning).
    AMIO_Core *core = nullptr;

    // Pending write tracking for flush/close.
    mutable std::mutex pending_mu;
    std::atomic<std::uint64_t> pending_writes{0};
    std::atomic<bool> has_failure{false};
    amio_err_t first_failure_code = AMIO_OK;

    // Next variable ID counter for ordering.
    std::atomic<std::uint64_t> next_variable_id{1};

    // ---- Read path state (task 9.2) ----

    // Prefetch queue for read-mode datasets.  Created during
    // open_dataset when mode == AMIO_MODE_READ.  Null for write
    // datasets.
    std::unique_ptr<PrefetchQueue> prefetch_queue;

    // Total timesteps in the dataset (from config, for read mode).
    std::int64_t total_timesteps = 0;

    // Read timeout in seconds (from config).
    std::int64_t read_timeout_s = 60;

    // Prefetch depth N (from config).
    std::size_t prefetch_depth = 4;

    // Outstanding view count for close-time validation (R5.10).
    std::atomic<std::uint64_t> outstanding_views{0};
};

// ---------------------------------------------------------------
// AMIO_Core -- internal context holding all subsystem references.
//
// Created by amio_init, destroyed by amio_finalize.  Holds the
// dataset records, worker pool reference, and staging pool reference.
// ---------------------------------------------------------------
struct AMIO_Core {
    HandleTable::Token core_token = 0;

    // Active dataset records keyed by dataset_id.
    mutable std::mutex datasets_mu;
    std::unordered_map<std::uint64_t, std::unique_ptr<DatasetRecord>> datasets;
    std::atomic<std::uint64_t> next_dataset_id{1};

    // Staging pool (owned by AMIO_Core, may be null in stub mode).
    StagingPool *staging_pool = nullptr;

    // Staging timeout from config (milliseconds).
    std::int64_t staging_timeout_ms = 5000;

    // Worker pool (owned by AMIO_Core, may be null in stub mode).
    // TODO: Replace with real WorkerPool* when task 9.x lands.
    WorkerPool *worker_pool = nullptr;
};

// process_handle_table() -- accessor for the singleton HandleTable
// owned by the AMIO_Core build.  All AMIO_C_API entry points share
// a single table because tokens are minted as raw `void*` values
// and the C ABI offers no place to thread per-context state.
HandleTable &process_handle_table();

// ---------------------------------------------------------------
// Private C++ entry points (one per AMIO_C_API function).
//
// `core_payload`, `dataset_payload`, `io_payload`, `view_payload`
// are the validated payload pointers extracted from the handle
// table by the C-Boundary.  They are guaranteed non-null on entry
// (the C-Boundary returns AMIO_ERR_INVALID_HANDLE before calling
// these functions otherwise).
//
// Each function returns an AMIO_ERR_* code.  Throwing is also
// permitted -- the C-Boundary catches `eckit::Exception`,
// `std::exception`, and `...` and translates them to the
// appropriate AMIO_ERR_* code (R12.2).
// ---------------------------------------------------------------

amio_status_t init(const char *manifest_path, amio_core_handle *out_core);

amio_status_t finalize(void *core_payload);

amio_status_t open_dataset(void *core_payload, const char *config_path, std::int32_t mode, amio_dataset_handle *out_dataset);

amio_status_t close_dataset(void *dataset_payload);

amio_status_t write(void *dataset_payload, const char *var_name, const void *host_data, amio_dtype_t dtype, const amio_shape_t *shape,
                    amio_io_handle *out_io);

amio_status_t read(void *dataset_payload, const char *var_name, std::int64_t timestep, const amio_bbox_t *bbox, amio_view_handle *out_view);

amio_status_t flush(void *dataset_payload, std::int64_t timeout_ms);

amio_status_t close(void *dataset_payload);

amio_status_t wait(void *io_payload, std::int64_t timeout_ms);

amio_status_t release_view(void *view_payload);

}  // namespace amio::detail

#endif  // AMIO_SRC_C_BOUNDARY_AMIO_CORE_HPP
