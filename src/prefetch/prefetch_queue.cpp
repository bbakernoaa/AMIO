// prefetch_queue.cpp -- AMIO Prefetch_Queue implementation.
//
// Implements the look-ahead prefetch queue for the read path.
// See prefetch_queue.hpp for the full interface documentation.
//
// Validates: R5.1, R5.2, R5.3, R5.4, R5.5, R5.7, R5.8

#include "prefetch/prefetch_queue.hpp"

#include "factory/backend_driver.hpp"
#include "staging/staging_pool.hpp"
#include "workers/worker_pool.hpp"

#include <algorithm>
#include <cassert>

namespace amio::detail {

PrefetchQueue::PrefetchQueue(std::size_t depth,
                             std::int64_t read_timeout_s,
                             StagingPool* pool,
                             WorkerPool* workers,
                             Backend_Driver* driver,
                             std::uint64_t dataset_id,
                             const std::string& var_name,
                             std::int64_t total_timesteps)
    : depth_(depth),
      read_timeout_s_(read_timeout_s),
      total_timesteps_(total_timesteps),
      dataset_id_(dataset_id),
      var_name_(var_name),
      pool_(pool),
      workers_(workers),
      driver_(driver) {
    // Clamp depth to valid range.
    if (depth_ < kMinDepth) depth_ = kMinDepth;
    if (depth_ > kMaxDepth) depth_ = kMaxDepth;

    // Clamp read timeout to valid range.
    if (read_timeout_s_ < kMinReadTimeoutS) read_timeout_s_ = kMinReadTimeoutS;
    if (read_timeout_s_ > kMaxReadTimeoutS) read_timeout_s_ = kMaxReadTimeoutS;
}

PrefetchQueue::~PrefetchQueue() {
    cancel_pending();
}

void PrefetchQueue::schedule_initial() {
    // Schedule min(N, M) fetches for timesteps [0, min(N, M)).
    std::int64_t count = static_cast<std::int64_t>(
        std::min(static_cast<std::int64_t>(depth_), total_timesteps_));

    for (std::int64_t t = 0; t < count; ++t) {
        schedule_fetch(t, nullptr);
    }
}

amio_status_t PrefetchQueue::get_buffer(std::int64_t timestep,
                                        const amio_bbox_t* bbox,
                                        StagingBuffer** out_buf) {
    assert(out_buf != nullptr);
    *out_buf = nullptr;

    std::unique_lock<std::mutex> lock(mu_);

    // Check if the fetch already failed.
    auto fail_it = failed_.find(timestep);
    if (fail_it != failed_.end()) {
        amio_err_t err = fail_it->second;
        failed_.erase(fail_it);
        return static_cast<amio_status_t>(err);
    }

    // Check if the buffer is already completed.
    auto comp_it = completed_.find(timestep);
    if (comp_it != completed_.end()) {
        *out_buf = comp_it->second;
        completed_.erase(comp_it);
        return AMIO_OK;
    }

    // Check if the timestep is pending -- if not, schedule it now.
    if (pending_.find(timestep) == pending_.end()) {
        // Not pending, not completed, not failed -- schedule it.
        lock.unlock();
        schedule_fetch(timestep, bbox);
        lock.lock();
    }

    // Block until the fetch completes, fails, or timeout expires.
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(read_timeout_s_);

    while (true) {
        // Check completed.
        comp_it = completed_.find(timestep);
        if (comp_it != completed_.end()) {
            *out_buf = comp_it->second;
            completed_.erase(comp_it);
            return AMIO_OK;
        }

        // Check failed.
        fail_it = failed_.find(timestep);
        if (fail_it != failed_.end()) {
            amio_err_t err = fail_it->second;
            failed_.erase(fail_it);
            return static_cast<amio_status_t>(err);
        }

        // Check cancellation.
        if (cancelled_) {
            return AMIO_ERR_BACKEND_FAILURE;
        }

        // Wait with timeout.
        auto status = cv_.wait_until(lock, deadline);
        if (status == std::cv_status::timeout) {
            // Check one more time before returning timeout.
            comp_it = completed_.find(timestep);
            if (comp_it != completed_.end()) {
                *out_buf = comp_it->second;
                completed_.erase(comp_it);
                return AMIO_OK;
            }
            fail_it = failed_.find(timestep);
            if (fail_it != failed_.end()) {
                amio_err_t err = fail_it->second;
                failed_.erase(fail_it);
                return static_cast<amio_status_t>(err);
            }
            return AMIO_ERR_TIMEOUT;
        }
    }
}

void PrefetchQueue::schedule_next(std::int64_t current_timestep) {
    std::int64_t next_t = current_timestep + static_cast<std::int64_t>(depth_);
    if (next_t < total_timesteps_) {
        // Only schedule if not already pending, completed, or failed.
        std::lock_guard<std::mutex> lock(mu_);
        if (pending_.find(next_t) == pending_.end() &&
            completed_.find(next_t) == completed_.end() &&
            failed_.find(next_t) == failed_.end()) {
            // Release lock before scheduling to avoid holding mu_
            // during potentially blocking operations.
        } else {
            return;  // Already tracked.
        }
        // We need to schedule outside the lock.
        // Mark as pending under the lock first.
        pending_.insert(next_t);
    } else {
        return;  // Out of bounds, no fetch needed.
    }

    // Perform the actual scheduling outside the lock.
    if (workers_ != nullptr) {
        // Dispatch to worker pool as a prefetch task.
        std::int64_t distance = next_t - current_timestep;
        auto self = this;
        auto timestep_to_fetch = next_t;

        workers_->submit_prefetch(
            timestep_to_fetch,
            distance,
            dataset_id_,
            [self, timestep_to_fetch]() {
                self->sync_fetch(timestep_to_fetch, nullptr);
            });
    } else {
        // Synchronous fallback: perform the fetch directly.
        // Note: pending_ was already marked above under the lock.
        sync_fetch(next_t, nullptr);
    }
}

void PrefetchQueue::mark_complete(std::int64_t timestep, StagingBuffer* buf) {
    std::lock_guard<std::mutex> lock(mu_);
    pending_.erase(timestep);
    completed_[timestep] = buf;
    cv_.notify_all();
}

void PrefetchQueue::mark_failed(std::int64_t timestep, amio_err_t error) {
    std::lock_guard<std::mutex> lock(mu_);
    pending_.erase(timestep);
    failed_[timestep] = error;
    cv_.notify_all();
}

void PrefetchQueue::cancel_pending() {
    std::lock_guard<std::mutex> lock(mu_);
    cancelled_ = true;
    pending_.clear();

    // Release completed buffers back to the pool.
    if (pool_) {
        for (auto& [ts, buf] : completed_) {
            if (buf) {
                pool_->release(buf);
            }
        }
    }
    completed_.clear();
    failed_.clear();
    cv_.notify_all();
}

std::size_t PrefetchQueue::completed_count() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return completed_.size();
}

std::size_t PrefetchQueue::pending_count() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
}

std::size_t PrefetchQueue::failed_count() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return failed_.size();
}

void PrefetchQueue::schedule_fetch(std::int64_t timestep,
                                   const amio_bbox_t* bbox) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (cancelled_) return;

        // Check if already tracked.
        if (pending_.count(timestep) > 0 ||
            completed_.count(timestep) > 0 ||
            failed_.count(timestep) > 0) {
            return;
        }
        pending_.insert(timestep);
    }

    if (workers_ != nullptr) {
        // Dispatch to worker pool as a prefetch task.
        auto self = this;
        auto ts = timestep;

        workers_->submit_prefetch(
            timestep,
            timestep,  // distance = timestep (from position 0)
            dataset_id_,
            [self, ts, bbox]() {
                self->sync_fetch(ts, bbox);
            });
    } else {
        // Synchronous fallback: perform the fetch directly on the
        // calling thread.
        sync_fetch(timestep, bbox);
    }
}

void PrefetchQueue::sync_fetch(std::int64_t timestep,
                               const amio_bbox_t* bbox) {
    // Check cancellation before doing work.
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (cancelled_) {
            pending_.erase(timestep);
            return;
        }
    }

    // Acquire a staging buffer.
    StagingBuffer* buf = nullptr;
    if (pool_) {
        // Use a reasonable buffer size.  In a full implementation,
        // this would be computed from the variable's shape and dtype.
        // For now, acquire the pool's default buffer capacity.
        buf = pool_->acquire(pool_->buffer_capacity());
    }

    if (!buf) {
        // Could not acquire buffer -- record as failure.
        mark_failed(timestep, AMIO_ERR_STAGING_BACKPRESSURE);
        return;
    }

    // Build VarMeta for the read.
    VarMeta meta{};
    meta.dataset_id = dataset_id_;
    meta.name = var_name_;
    meta.timestep = timestep;

    // Build optional BoundingBox from bbox parameter.
    std::optional<BoundingBox> opt_bbox;
    if (bbox != nullptr) {
        BoundingBox bb{};
        bb.rank = bbox->rank;
        for (int d = 0; d < bbox->rank && d < AMIO_MAX_RANK; ++d) {
            bb.offsets[d] = bbox->offsets[d];
            bb.extents[d] = bbox->extents[d];
            bb.strides[d] = bbox->strides[d];
        }
        opt_bbox = bb;
    }

    // Perform the read through the backend driver.
    try {
        if (driver_) {
            driver_->read(*buf, meta, timestep, opt_bbox);
        }
        mark_complete(timestep, buf);
    } catch (...) {
        // Release the buffer back to the pool on failure.
        if (pool_ && buf) {
            pool_->release(buf);
        }
        mark_failed(timestep, AMIO_ERR_BACKEND_FAILURE);
    }
}

}  // namespace amio::detail
