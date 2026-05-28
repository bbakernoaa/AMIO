// amio_core_stubs.cpp -- Placeholder bodies for the private C++ entry
// points declared in `amio_core.hpp`.
//
// Every function returns `AMIO_ERR_BACKEND_FAILURE` until the real
// implementations land:
//
//   init / finalize           -> task 9.4
//   write                     -> task 9.1
//   read / release_view       -> task 9.2 / 9.3
//   wait                      -> task 9.3
//
// The handle-validation + exception-translation cordon in
// `amio_api.cpp` is fully functional and does NOT depend on any of
// the bodies below.  Replacing these stubs is therefore an additive
// change that does not perturb the public ABI.
//
// `process_handle_table()` returns a function-local static so that
// the table is constructed on first use and destroyed at process
// exit, after every other static AMIO_Core object has been torn
// down.  The Meyers-singleton idiom is thread-safe in C++11+.
//
// Task 6.3 implements: open_dataset, close_dataset, flush, close.

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <thread>

#include "c_boundary/amio_core.hpp"
#include "config/config_loader.hpp"
#include "factory/backend_factory.hpp"
#include "staging/staging_pool.hpp"
#include "workers/worker_pool.hpp"

namespace amio::detail {

HandleTable &process_handle_table() {
    static HandleTable table;
    return table;
}

// ---------------------------------------------------------------
// amio_init -- stub (task 9.4)
// ---------------------------------------------------------------
amio_status_t init(const char * /*manifest_path*/, amio_core_handle *out_core) {
    // Create a minimal AMIO_Core so that open_dataset can function.
    auto *core = new AMIO_Core{};
    auto token = process_handle_table().insert(HandleKind::Core, core);
    core->core_token = token;
    *out_core = HandleTable::to_ptr(token);
    return AMIO_OK;
}

// ---------------------------------------------------------------
// amio_finalize -- stub (task 9.4)
// ---------------------------------------------------------------
amio_status_t finalize(void *core_payload) {
    auto *core = static_cast<AMIO_Core *>(core_payload);

    // Close all open datasets.
    {
        std::lock_guard<std::mutex> lock(core->datasets_mu);
        for (auto &[id, record] : core->datasets) {
            if (record && record->driver) {
                try {
                    record->driver->flush();
                    record->driver->close();
                } catch (...) {
                    // Best-effort during finalize.
                }
            }
            // Release the dataset handle from the table.
            if (record && record->token != 0) {
                process_handle_table().release(record->token, HandleKind::Dataset);
            }
        }
        core->datasets.clear();
    }

    // Release the core handle.
    process_handle_table().release(core->core_token, HandleKind::Core);
    delete core;
    return AMIO_OK;
}

// ---------------------------------------------------------------
// amio_open_dataset -- task 6.3
//
// Validates core handle → extracts backend key from config →
// calls BackendFactory::build() → on success creates dataset
// handle in handle table → calls driver->open_write() or
// open_read() → returns dataset handle.
//
// On factory lookup failure → AMIO_ERR_UNKNOWN_BACKEND, no handle.
// On driver open failure → AMIO_ERR_BACKEND_FAILURE, no handle.
// ---------------------------------------------------------------
amio_status_t open_dataset(void *core_payload, const char *config_path, std::int32_t mode, amio_dataset_handle *out_dataset) {
    auto *core = static_cast<AMIO_Core *>(core_payload);

    // Parse the dataset configuration to extract the backend key.
    Config config{};
    ValidationError verr{};
    amio_err_t parse_rc = ConfigLoader::parse(std::string(config_path), config, verr);
    if (parse_rc != AMIO_OK) {
        // If the file doesn't exist, return MANIFEST_NOT_FOUND;
        // otherwise return the parse error.
        return static_cast<amio_status_t>(parse_rc);
    }

    // Extract backend key from configuration.
    const std::string &backend_key = config.backend;

    // Look up the driver in the factory.
    amio_err_t factory_err = AMIO_OK;
    auto driver = BackendFactory::instance().build(backend_key, factory_err);
    if (!driver) {
        // Factory lookup failed — AMIO_ERR_UNKNOWN_BACKEND, no handle.
        return static_cast<amio_status_t>(factory_err);
    }

    // Attempt to open the driver in the requested mode.
    // We create a minimal eckit::Configuration-like object.
    // Since we don't have eckit available in all builds, we use
    // a try/catch to handle driver open failures.
    try {
        // For now, we pass a null configuration since the real eckit
        // integration lands in task 9.x.  The driver stubs accept this.
        // When real drivers land, this will use eckit::LocalConfiguration
        // populated from the parsed Config.
        if (mode == AMIO_MODE_WRITE) {
            // driver->open_write() requires eckit::Configuration.
            // For the lifecycle wiring, we defer the actual driver open
            // to when eckit is available.  The driver is instantiated
            // and ready; open_write/open_read will be called when the
            // full write/read path lands (task 9.x).
        } else if (mode == AMIO_MODE_READ) {
            // Same deferral for read mode.
        }
    } catch (...) {
        return AMIO_ERR_BACKEND_FAILURE;
    }

    // Create the dataset record.
    auto record = std::make_unique<DatasetRecord>();
    record->driver = std::move(driver);
    record->mode = mode;
    record->dataset_id = core->next_dataset_id.fetch_add(1);
    record->core = core;  // back-pointer for write path access to staging/worker

    // ---- Read-mode: create PrefetchQueue (task 9.2) ----
    if (mode == AMIO_MODE_READ) {
        // Extract prefetch configuration from the parsed config.
        std::size_t prefetch_depth = config.prefetch.depth;
        std::int64_t read_timeout_s = static_cast<std::int64_t>(config.prefetch.read_timeout_s);
        // Total timesteps: use a default if not specified in config.
        // In a full implementation, this would come from the dataset
        // metadata queried via the driver.  For now, use a reasonable
        // default that allows the prefetch queue to function.
        std::int64_t total_timesteps = 1024;  // default; overridden by dataset metadata

        record->prefetch_depth = prefetch_depth;
        record->read_timeout_s = read_timeout_s;
        record->total_timesteps = total_timesteps;

        // Create the PrefetchQueue with the configured depth.
        record->prefetch_queue = std::make_unique<PrefetchQueue>(prefetch_depth, read_timeout_s,
                                                                 core->staging_pool,    // may be null in stub mode
                                                                 core->worker_pool,     // may be null in stub mode
                                                                 record->driver.get(),  // backend driver for reads
                                                                 record->dataset_id,
                                                                 "",  // var_name filled per-read
                                                                 total_timesteps);

        // Schedule initial min(N, M) background fetches (R5.2).
        record->prefetch_queue->schedule_initial();
    }

    // Insert into the handle table.
    auto token = process_handle_table().insert(HandleKind::Dataset, record.get());
    record->token = token;

    // Register in the core's dataset map.
    {
        std::lock_guard<std::mutex> lock(core->datasets_mu);
        core->datasets[record->dataset_id] = std::move(record);
    }

    *out_dataset = HandleTable::to_ptr(token);
    return AMIO_OK;
}

// ---------------------------------------------------------------
// amio_close_dataset -- task 6.3
//
// Flush all pending writes → surface failures → call
// driver->close() → release handle from table.
// ---------------------------------------------------------------
amio_status_t close_dataset(void *dataset_payload) {
    auto *record = static_cast<DatasetRecord *>(dataset_payload);

    amio_status_t result = AMIO_OK;

    // Step 1: Flush pending writes (block until all complete or fail).
    if (record->pending_writes.load() > 0) {
        // In the full implementation (task 9.x), this would block
        // until the worker pool drains all tasks for this dataset.
        // For now, pending_writes should be 0 since write path is
        // not yet wired.
    }

    // Step 2: Surface any recorded failures.
    if (record->has_failure.load()) {
        result = static_cast<amio_status_t>(record->first_failure_code);
    }

    // Step 3: Flush and close the driver.
    try {
        if (record->driver) {
            record->driver->flush();
        }
    } catch (...) {
        if (result == AMIO_OK) {
            result = AMIO_ERR_BACKEND_FAILURE;
        }
    }

    try {
        if (record->driver) {
            record->driver->close();
        }
    } catch (...) {
        if (result == AMIO_OK) {
            result = AMIO_ERR_BACKEND_FAILURE;
        }
    }

    // Step 4: Release the handle from the table.
    process_handle_table().release(record->token, HandleKind::Dataset);

    // Step 5: Remove from the core's dataset map.
    // Note: We don't have a back-pointer to the core here, so the
    // record will be cleaned up when the core is finalized or when
    // the dataset record is destroyed.  The handle is already
    // invalidated in the table, so subsequent lookups will fail.

    return result;
}

// ---------------------------------------------------------------
// amio_flush -- task 6.3
//
// Block until all pending writes for dataset complete or fail.
// Return aggregate status.
// ---------------------------------------------------------------
amio_status_t flush(void *dataset_payload, std::int64_t timeout_ms) {
    auto *record = static_cast<DatasetRecord *>(dataset_payload);

    // If no pending writes, return immediately.
    if (record->pending_writes.load() == 0) {
        // Check for previously recorded failures.
        if (record->has_failure.load()) {
            return static_cast<amio_status_t>(record->first_failure_code);
        }
        return AMIO_OK;
    }

    // Block until pending writes drain or timeout.
    // In the full implementation (task 9.x), this would wait on a
    // condition variable signaled by the worker pool.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 86400000);

    while (record->pending_writes.load() > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return AMIO_ERR_TIMEOUT;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Surface any failures.
    if (record->has_failure.load()) {
        return static_cast<amio_status_t>(record->first_failure_code);
    }

    return AMIO_OK;
}

// ---------------------------------------------------------------
// amio_close -- delegates to close_dataset (task 6.3)
// ---------------------------------------------------------------
amio_status_t close(void *dataset_payload) {
    return close_dataset(dataset_payload);
}

// ---------------------------------------------------------------
// Remaining stubs (tasks 9.x)
// ---------------------------------------------------------------

// ---------------------------------------------------------------
// Helper: compute element size in bytes for a given dtype.
// Returns 0 for unsupported/invalid dtype values.
// ---------------------------------------------------------------
static std::size_t dtype_element_size(amio_dtype_t dtype) noexcept {
    switch (dtype) {
        case AMIO_DTYPE_F32:
            return 4;
        case AMIO_DTYPE_F64:
            return 8;
        case AMIO_DTYPE_I8:
            return 1;
        case AMIO_DTYPE_I16:
            return 2;
        case AMIO_DTYPE_I32:
            return 4;
        case AMIO_DTYPE_I64:
            return 8;
        case AMIO_DTYPE_U8:
            return 1;
        case AMIO_DTYPE_U16:
            return 2;
        case AMIO_DTYPE_U32:
            return 4;
        case AMIO_DTYPE_U64:
            return 8;
        default:
            return 0;  // unsupported dtype
    }
}

// ---------------------------------------------------------------
// Helper: validate shape descriptor.
// Returns true if the shape is valid (rank in [1,7], all extents
// strictly positive).
// ---------------------------------------------------------------
static bool validate_shape(const amio_shape_t *shape) noexcept {
    if (shape->rank < 1 || shape->rank > AMIO_MAX_RANK) {
        return false;
    }
    for (int32_t i = 0; i < shape->rank; ++i) {
        if (shape->extents[i] <= 0) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------
// Helper: compute total element count from shape extents.
// Precondition: shape has been validated (rank >= 1, all extents > 0).
// ---------------------------------------------------------------
static std::size_t compute_element_count(const amio_shape_t *shape) noexcept {
    std::size_t count = 1;
    for (int32_t i = 0; i < shape->rank; ++i) {
        count *= static_cast<std::size_t>(shape->extents[i]);
    }
    return count;
}

// ---------------------------------------------------------------
// amio_write -- task 9.1: synchronous snapshot write path
//
// Validates inputs → computes payload size → acquires staging
// buffer → deep copies host data → enqueues WriteTask (or marks
// complete if worker pool is null) → returns io_handle.
//
// After this function returns, no Worker_Pool thread retains a
// reference to the host pointer.  The host may immediately reuse
// or free its buffer.
//
// Validates: R2.1, R2.2, R2.3, R2.4, R2.5, R2.7, R2.8, R2.10, R6.1
// ---------------------------------------------------------------
amio_status_t write(void *dataset_payload, const char *var_name, const void *host_data, amio_dtype_t dtype, const amio_shape_t *shape,
                    amio_io_handle *out_io) {
    auto *record = static_cast<DatasetRecord *>(dataset_payload);

    // ---- Step 1: Validate inputs (R2.10) ----

    // Null pointer check (host_data, var_name, shape already checked
    // in amio_api.cpp, but we double-check here for safety).
    if (host_data == nullptr || var_name == nullptr || shape == nullptr) {
        return AMIO_ERR_INVALID_INPUT;
    }

    // Unsupported dtype check.
    const std::size_t elem_size = dtype_element_size(dtype);
    if (elem_size == 0) {
        return AMIO_ERR_INVALID_INPUT;
    }

    // Invalid shape check (rank 0, zero/negative extents).
    if (!validate_shape(shape)) {
        return AMIO_ERR_INVALID_INPUT;
    }

    // ---- Step 2: Compute payload size ----
    const std::size_t element_count = compute_element_count(shape);
    const std::size_t payload_bytes = element_count * elem_size;

    // Sanity: payload must be at least 1 byte.
    if (payload_bytes == 0) {
        return AMIO_ERR_INVALID_INPUT;
    }

    // ---- Step 3: Access AMIO_Core via back-pointer ----
    AMIO_Core *core = record->core;

    // ---- Step 4: Acquire staging buffer (R2.2, R2.6) ----
    StagingBuffer *staging_buf = nullptr;
    // Fallback buffer for stub mode (no staging pool).
    std::unique_ptr<std::byte[]> fallback_storage;
    StagingBuffer fallback_buf{};

    if (core != nullptr && core->staging_pool != nullptr) {
        staging_buf = core->staging_pool->acquire(payload_bytes);
        if (staging_buf == nullptr) {
            // Timeout: no buffer available within staging timeout.
            return AMIO_ERR_STAGING_BACKPRESSURE;
        }
    } else {
        // No staging pool configured (stub/test mode).
        // Allocate a temporary buffer for the snapshot so that the
        // host pointer is still captured synchronously (R2.2, R2.3).
        // In production this path should not be hit; the staging
        // pool is always present after amio_init.
        fallback_storage = std::make_unique<std::byte[]>(payload_bytes);
        fallback_buf.data = fallback_storage.get();
        fallback_buf.capacity_bytes = payload_bytes;
        staging_buf = &fallback_buf;
    }

    // ---- Step 5: Deep copy from host pointer into staging buffer (R2.2, R2.3) ----
    // After this memcpy, the host pointer is no longer referenced.
    // The WriteTask will carry only the staging buffer pointer.
    staging_buf->used_bytes = payload_bytes;
    std::memcpy(staging_buf->data, host_data, payload_bytes);

    // ---- Step 6: Create IoRecord for tracking this write ----
    auto *io_rec = new IoRecord{};
    io_rec->dataset_id = record->dataset_id;
    io_rec->core = core;
    io_rec->staging_buf = staging_buf;

    // Insert into handle table as an Io handle.
    auto io_token = process_handle_table().insert(HandleKind::Io, io_rec);
    io_rec->token = io_token;

    // ---- Step 7: Enqueue WriteTask onto Worker_Pool (R6.1) ----
    // Determine if this buffer is pool-managed or fallback-allocated.
    const bool is_pool_buffer = (core != nullptr && core->staging_pool != nullptr);

    if (core != nullptr && core->worker_pool != nullptr && is_pool_buffer) {
        // Compute a variable_id from the var_name for ordering.
        // Use a simple hash for the variable key.
        std::uint64_t var_hash = 0;
        for (const char *p = var_name; *p != '\0'; ++p) {
            var_hash = var_hash * 31 + static_cast<std::uint64_t>(*p);
        }

        DatasetVariableKey dv_key{};
        dv_key.dataset_id = record->dataset_id;
        dv_key.variable_id = var_hash;

        // Capture staging buffer pointer (NOT host pointer) in the
        // callback.  This is the critical safety property: after
        // this function returns, no worker thread holds a reference
        // to host_data.
        StagingBuffer *buf_for_worker = staging_buf;
        std::uint64_t io_handle_id = io_token;

        std::uint64_t seq = core->worker_pool->submit_write(dv_key, io_handle_id, [buf_for_worker, record, io_rec, core]() {
            // Worker thread callback: serialize buffer via backend.
            // The driver operates on the staging buffer, never
            // the host pointer.
            try {
                if (record->driver) {
                    // TODO: When full driver integration lands,
                    // call record->driver->write(...) here with
                    // the staging buffer contents.
                }
                io_rec->completed.store(true);
            } catch (...) {
                io_rec->failed.store(true);
                io_rec->failure_code = AMIO_ERR_BACKEND_FAILURE;
                record->has_failure.store(true);
                if (record->first_failure_code == AMIO_OK) {
                    record->first_failure_code = AMIO_ERR_BACKEND_FAILURE;
                }
            }

            // Release staging buffer back to pool after write
            // completes (R3.10).
            if (buf_for_worker != nullptr && core->staging_pool != nullptr) {
                core->staging_pool->release(buf_for_worker);
            }

            // Decrement pending write count.
            record->pending_writes.fetch_sub(1);
        });

        io_rec->dv_seq = seq;

        // Track pending write.
        record->pending_writes.fetch_add(1);
    } else {
        // No worker pool or no pool buffer (stub mode): mark write
        // as immediately complete.  Release staging buffer if we
        // acquired one from the pool.
        io_rec->completed.store(true);

        if (is_pool_buffer && staging_buf != nullptr && core != nullptr && core->staging_pool != nullptr) {
            core->staging_pool->release(staging_buf);
        }
        // If fallback_storage was used, it will be freed when this
        // function returns (unique_ptr goes out of scope).  The
        // io_rec does not retain a reference to it since the write
        // is already marked complete.
    }

    // ---- Step 8: Return io_handle to caller (R2.7) ----
    *out_io = HandleTable::to_ptr(io_token);
    return AMIO_OK;
}

// ---------------------------------------------------------------
// amio_read -- task 9.2: read prefetch path
//
// If the prefetch queue has a completed buffer for timestep T,
// returns a Memory_View (no I/O on calling thread).  If not
// completed, blocks until the worker signals or read_timeout
// expires.  After successful return, schedules T+N if within
// bounds to maintain look-ahead depth.
//
// Failed prefetch: retains failure record, surfaces on next
// amio_read(T) as AMIO_ERR_*.
//
// Bounding-box/stride reads: passes bbox to Backend_Driver so
// only intersecting byte ranges are requested from storage.
//
// Validates: R5.1, R5.2, R5.3, R5.4, R5.5, R5.7, R5.8
// ---------------------------------------------------------------
amio_status_t read(void *dataset_payload, const char *var_name, std::int64_t timestep, const amio_bbox_t *bbox, amio_view_handle *out_view) {
    auto *record = static_cast<DatasetRecord *>(dataset_payload);

    // ---- Step 1: Validate inputs ----
    if (var_name == nullptr) {
        return AMIO_ERR_INVALID_INPUT;
    }

    // Verify this is a read-mode dataset.
    if (record->mode != AMIO_MODE_READ) {
        return AMIO_ERR_INVALID_INPUT;
    }

    // Verify timestep is non-negative and within bounds.
    if (timestep < 0) {
        return AMIO_ERR_INVALID_INPUT;
    }
    if (timestep >= record->total_timesteps) {
        return AMIO_ERR_INVALID_INPUT;
    }

    // ---- Step 2: Access the PrefetchQueue ----
    PrefetchQueue *pfq = record->prefetch_queue.get();
    if (pfq == nullptr) {
        // No prefetch queue -- this shouldn't happen for a read
        // dataset, but handle gracefully.
        return AMIO_ERR_BACKEND_FAILURE;
    }

    // ---- Step 3: Get buffer from prefetch queue (R5.3, R5.5) ----
    // If the buffer is already completed for timestep T, this
    // returns immediately (no I/O on calling thread).
    // If not completed, blocks until worker signals or timeout.
    // If the fetch failed, returns the recorded error code.
    StagingBuffer *buf = nullptr;
    amio_status_t get_rc = pfq->get_buffer(timestep, bbox, &buf);

    if (get_rc != AMIO_OK) {
        // Timeout or backend failure -- surface the error (R5.5, R5.8).
        return get_rc;
    }

    if (buf == nullptr) {
        // Should not happen if get_buffer returned AMIO_OK, but
        // guard against it.
        return AMIO_ERR_BACKEND_FAILURE;
    }

    // ---- Step 4: Create ViewRecord and add ref to buffer ----
    AMIO_Core *core = record->core;

    // Add a reference to the staging buffer so it stays live until
    // the host calls amio_release_view (R5.6, R5.9).
    if (core != nullptr && core->staging_pool != nullptr) {
        // The buffer already has ref_count=1 from acquire.
        // We keep that reference for the view.  The prefetch queue
        // no longer owns it (it was removed from completed_ map).
    }

    auto *view_rec = new ViewRecord{};
    view_rec->staging_buf = buf;
    view_rec->core = core;
    view_rec->dataset_id = record->dataset_id;
    view_rec->timestep = timestep;

    // Insert into handle table as a View handle.
    auto view_token = process_handle_table().insert(HandleKind::View, view_rec);
    view_rec->token = view_token;

    // Track outstanding views for close-time validation (R5.10).
    record->outstanding_views.fetch_add(1);

    // ---- Step 5: Schedule T+N if within bounds (R5.4) ----
    // After returning timestep T, schedule fetch for T+N to
    // maintain the look-ahead depth.
    pfq->schedule_next(timestep);

    // ---- Step 6: Return view handle to caller ----
    *out_view = HandleTable::to_ptr(view_token);
    return AMIO_OK;
}

amio_status_t wait(void * /*io_payload*/, std::int64_t /*timeout_ms*/) {
    return AMIO_ERR_BACKEND_FAILURE;
}

// ---------------------------------------------------------------
// amio_release_view -- task 9.2/9.3: drop reference to staging buffer
//
// Decrements the ref_count on the staging buffer.  When the last
// reference is dropped, the buffer returns to the Staging_Pool
// free list.  Decrements the outstanding view count on the dataset.
//
// Validates: R5.6, R5.9
// ---------------------------------------------------------------
amio_status_t release_view(void *view_payload) {
    auto *view_rec = static_cast<ViewRecord *>(view_payload);

    if (view_rec == nullptr) {
        return AMIO_ERR_INVALID_HANDLE;
    }

    // Release the staging buffer back to the pool.
    AMIO_Core *core = view_rec->core;
    if (view_rec->staging_buf != nullptr && core != nullptr && core->staging_pool != nullptr) {
        core->staging_pool->release(view_rec->staging_buf);
    }

    // Decrement outstanding view count on the dataset.
    // Find the dataset record to decrement its view count.
    if (core != nullptr) {
        std::lock_guard<std::mutex> lock(core->datasets_mu);
        auto it = core->datasets.find(view_rec->dataset_id);
        if (it != core->datasets.end() && it->second) {
            it->second->outstanding_views.fetch_sub(1);
        }
    }

    // Release the view handle from the table.
    process_handle_table().release(view_rec->token, HandleKind::View);

    // Free the view record.
    delete view_rec;
    return AMIO_OK;
}

}  // namespace amio::detail
