// staging_pool.cpp -- AMIO Staging_Pool buffer manager implementation.
//
// Implements the bounded buffer pool described in design.md §3
// (Staging Pool & Worker Pool).  All buffers are allocated up front
// during construction; no additional memory is allocated after that.
//
// Validates: R1.3, R1.5, R2.2, R2.6, R2.9, R3.10, R5.6, R5.9

#include "staging/staging_pool.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace amio::detail {

StagingPool::StagingPool(std::size_t buffer_count, std::size_t buffer_capacity, std::int64_t timeout_ms)
    : buffer_count_(buffer_count), buffer_capacity_(buffer_capacity), timeout_ms_(timeout_ms) {
    assert(buffer_count >= kMinBufferCount && buffer_count <= kMaxBufferCount);
    assert(buffer_capacity >= kMinBufferCapacity && buffer_capacity <= kMaxBufferCapacity);
    assert(timeout_ms >= kMinTimeoutMs && timeout_ms <= kMaxTimeoutMs);

    // Pre-allocate all buffers and their backing storage.
    buffers_.resize(buffer_count);
    storage_.reserve(buffer_count);
    free_list_.reserve(buffer_count);

    for (std::size_t i = 0; i < buffer_count; ++i) {
        auto mem = std::make_unique<std::byte[]>(buffer_capacity);
        buffers_[i].data = mem.get();
        buffers_[i].capacity_bytes = buffer_capacity;
        buffers_[i].used_bytes = 0;
        buffers_[i].ref_count = 0;
        buffers_[i].seq = 0;
        storage_.push_back(std::move(mem));

        // All buffers start on the free list.
        free_list_.push_back(i);
    }

    // Sort free list by capacity (all equal here, but the structure
    // supports heterogeneous capacities if needed in the future).
    std::sort(free_list_.begin(), free_list_.end(),
              [this](std::size_t a, std::size_t b) { return buffers_[a].capacity_bytes < buffers_[b].capacity_bytes; });
}

StagingPool::~StagingPool() {
    // storage_ unique_ptrs handle deallocation automatically.
    // buffers_ data pointers become dangling but that's fine since
    // the pool is being destroyed.
}

StagingBuffer* StagingPool::acquire(std::size_t required_bytes) {
    std::unique_lock<std::mutex> lock(mu_);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);

    while (true) {
        std::size_t idx = find_best_fit(required_bytes);
        if (idx < free_list_.size()) {
            return remove_from_free_list(idx);
        }

        // No suitable buffer available; wait for one to be released.
        auto status = cv_.wait_until(lock, deadline);
        if (status == std::cv_status::timeout) {
            // One final check after wakeup -- a spurious wakeup or
            // a release that happened just before the timeout could
            // have made a buffer available.
            idx = find_best_fit(required_bytes);
            if (idx < free_list_.size()) {
                return remove_from_free_list(idx);
            }
            // Timeout: return nullptr to signal backpressure.
            return nullptr;
        }
        // Spurious wakeup or legitimate notification -- loop back
        // and try to find a suitable buffer.
    }
}

void StagingPool::release(StagingBuffer* buf) {
    assert(buf != nullptr);

    std::lock_guard<std::mutex> lock(mu_);

    assert(buf->ref_count > 0);
    --buf->ref_count;

    if (buf->ref_count == 0) {
        // Return buffer to the free list.
        buf->used_bytes = 0;

        // Find the buffer's index in the buffers_ vector.
        auto buf_idx = static_cast<std::size_t>(buf - buffers_.data());
        assert(buf_idx < buffers_.size());

        // Insert into free list maintaining sorted order by capacity.
        auto it = std::lower_bound(free_list_.begin(), free_list_.end(), buf_idx,
                                   [this](std::size_t a, std::size_t b) { return buffers_[a].capacity_bytes < buffers_[b].capacity_bytes; });
        free_list_.insert(it, buf_idx);

        // Notify one waiting thread that a buffer is available.
        cv_.notify_one();
    }
}

void StagingPool::add_ref(StagingBuffer* buf) {
    assert(buf != nullptr);

    std::lock_guard<std::mutex> lock(mu_);
    assert(buf->ref_count > 0);
    ++buf->ref_count;
}

std::size_t StagingPool::total_buffer_count() const noexcept {
    return buffer_count_;
}

std::size_t StagingPool::free_buffer_count() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return free_list_.size();
}

std::size_t StagingPool::buffer_capacity() const noexcept {
    return buffer_capacity_;
}

std::int64_t StagingPool::timeout_ms() const noexcept {
    return timeout_ms_;
}

std::size_t StagingPool::find_best_fit(std::size_t required_bytes) const {
    // Free list is sorted by capacity (ascending).  Find the first
    // buffer whose capacity >= required_bytes (best-fit-or-larger).
    for (std::size_t i = 0; i < free_list_.size(); ++i) {
        if (buffers_[free_list_[i]].capacity_bytes >= required_bytes) {
            return i;
        }
    }
    return free_list_.size();  // sentinel: no suitable buffer found
}

StagingBuffer* StagingPool::remove_from_free_list(std::size_t free_idx) {
    assert(free_idx < free_list_.size());

    std::size_t buf_idx = free_list_[free_idx];
    free_list_.erase(free_list_.begin() + static_cast<std::ptrdiff_t>(free_idx));

    StagingBuffer* buf = &buffers_[buf_idx];
    buf->ref_count = 1;
    buf->seq += 1;
    // used_bytes is set by the caller after acquire returns.
    return buf;
}

}  // namespace amio::detail
