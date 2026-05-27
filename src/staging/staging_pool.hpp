// staging_pool.hpp -- AMIO Staging_Pool buffer manager.
//
// This header is PRIVATE to the AMIO_Core build (`src/staging/`).
// It is never installed and must not be referenced from any public
// header under `include/amio/`.
//
// Purpose
// -------
// The Staging_Pool is a bounded set of pre-sized buffers allocated
// up front from the manifest's `(buffer_count, buffer_capacity)`.
// It provides:
//
//   * Best-fit-or-larger acquisition over a free-list keyed on
//     capacity.  When no buffer with sufficient capacity is free,
//     the calling thread waits on a condition variable up to the
//     manifest staging timeout (1 ms..60000 ms, default 5000 ms)
//     before returning AMIO_ERR_STAGING_BACKPRESSURE (R2.6).
//
//   * Buffer release on worker task completion (R3.10) and on
//     `release_view` for read paths (R5.6, R5.9).
//
//   * Memory ownership is exclusively the pool's; Memory_View
//     instances handed out are non-owning views.
//
// Thread safety
// -------------
// All public methods are safe to call concurrently from any thread.
// The implementation uses std::mutex + std::condition_variable.
//
// Validates: R1.3, R1.5, R2.2, R2.6, R2.9, R3.10, R5.6, R5.9

#ifndef AMIO_SRC_STAGING_STAGING_POOL_HPP
#define AMIO_SRC_STAGING_STAGING_POOL_HPP

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "amio/amio_errors.h"

namespace amio::detail {

// StagingBuffer -- header for a single pool-owned buffer.
//
// The `data` pointer is owned exclusively by the Staging_Pool and
// is allocated once during construction.  Memory_View instances
// returned to the host are non-owning views over `data`.
//
// Fields:
//   data           - raw byte storage, owned by the pool
//   capacity_bytes - total allocated size of `data`
//   used_bytes     - payload size for the current task (0 when free)
//   ref_count      - for read-side multi-view sharing; when it
//                    drops to 0 the buffer is returned to the pool
//   seq            - generation counter, incremented on every
//                    acquire; used to detect stale references
struct StagingBuffer {
    std::byte*  data            = nullptr;
    std::size_t capacity_bytes  = 0;
    std::size_t used_bytes      = 0;
    int         ref_count       = 0;
    std::uint64_t seq           = 0;
};

// StagingPool -- bounded buffer pool with best-fit acquisition and
// condition-variable backpressure.
//
// Construction allocates all buffers up front; no additional memory
// is allocated after construction.  Destruction frees all buffers.
class StagingPool {
public:
    // Configuration limits (from design.md / requirements).
    static constexpr std::size_t kMinBufferCount    = 1;
    static constexpr std::size_t kMaxBufferCount    = 4096;
    static constexpr std::size_t kMinBufferCapacity = 1;
    static constexpr std::size_t kMaxBufferCapacity = 1'073'741'824;  // 1 GiB

    static constexpr std::int64_t kMinTimeoutMs     = 1;
    static constexpr std::int64_t kMaxTimeoutMs     = 60'000;
    static constexpr std::int64_t kDefaultTimeoutMs = 5'000;

    // Construct a pool with `buffer_count` buffers, each of
    // `buffer_capacity` bytes, and a staging timeout of
    // `timeout_ms` milliseconds.
    //
    // Preconditions (enforced by assertions in debug builds;
    // callers are expected to validate via Config_Loader before
    // constructing):
    //   buffer_count    in [kMinBufferCount, kMaxBufferCount]
    //   buffer_capacity in [kMinBufferCapacity, kMaxBufferCapacity]
    //   timeout_ms      in [kMinTimeoutMs, kMaxTimeoutMs]
    StagingPool(std::size_t buffer_count,
                std::size_t buffer_capacity,
                std::int64_t timeout_ms = kDefaultTimeoutMs);

    // Non-copyable, non-movable (owns raw memory).
    StagingPool(const StagingPool&) = delete;
    StagingPool& operator=(const StagingPool&) = delete;
    StagingPool(StagingPool&&) = delete;
    StagingPool& operator=(StagingPool&&) = delete;

    ~StagingPool();

    // acquire -- obtain a buffer with capacity >= `required_bytes`.
    //
    // Uses best-fit-or-larger selection over the free list.  If no
    // suitable buffer is available, blocks the calling thread on a
    // condition variable for up to the configured staging timeout.
    //
    // Returns:
    //   Pointer to a StagingBuffer on success (ref_count set to 1,
    //   used_bytes set to `required_bytes`, seq incremented).
    //   nullptr on timeout (caller should return
    //   AMIO_ERR_STAGING_BACKPRESSURE).
    StagingBuffer* acquire(std::size_t required_bytes);

    // release -- return a buffer to the free list.
    //
    // Decrements ref_count.  When ref_count reaches 0, the buffer
    // is returned to the free list and waiting threads are notified.
    //
    // Precondition: `buf` must be a buffer owned by this pool with
    // ref_count > 0.
    void release(StagingBuffer* buf);

    // add_ref -- increment the reference count on a buffer.
    //
    // Used for read-side multi-view sharing: when multiple
    // Memory_Views reference the same prefetched buffer, each
    // outstanding view holds a reference.
    void add_ref(StagingBuffer* buf);

    // ----- Diagnostics / test helpers -----

    // total_buffer_count -- the number of buffers in the pool
    // (both free and in-use).
    std::size_t total_buffer_count() const noexcept;

    // free_buffer_count -- the number of buffers currently on the
    // free list (available for acquisition).
    std::size_t free_buffer_count() const noexcept;

    // buffer_capacity -- the per-buffer capacity in bytes.
    std::size_t buffer_capacity() const noexcept;

    // timeout_ms -- the configured staging timeout.
    std::int64_t timeout_ms() const noexcept;

private:
    // Find the best-fit-or-larger buffer in the free list.
    // Returns the index into free_list_, or free_list_.size() if
    // none is suitable.  Caller must hold mu_.
    std::size_t find_best_fit(std::size_t required_bytes) const;

    // Remove a buffer at the given free-list index and return it.
    // Caller must hold mu_.
    StagingBuffer* remove_from_free_list(std::size_t free_idx);

    mutable std::mutex          mu_;
    std::condition_variable     cv_;

    // All buffers owned by the pool (fixed after construction).
    std::vector<StagingBuffer>  buffers_;

    // Raw memory backing (one contiguous allocation per buffer).
    std::vector<std::unique_ptr<std::byte[]>> storage_;

    // Free list: indices into buffers_ that are currently available.
    // Kept sorted by capacity for efficient best-fit search.
    std::vector<std::size_t>    free_list_;

    // Configuration.
    std::size_t  buffer_count_;
    std::size_t  buffer_capacity_;
    std::int64_t timeout_ms_;
};

}  // namespace amio::detail

#endif  // AMIO_SRC_STAGING_STAGING_POOL_HPP
