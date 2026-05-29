import sys

with open('src/workers/worker_pool.cpp', 'r') as f:
    content = f.read()

old_code = """    // --- Try write queue ---
    if (!write_queue_.empty()) {
        WriteTask task = std::move(write_queue_.front());
        write_queue_.pop();

        DvOrderState& state = get_dv_state(task.dv_key);

        // Check if this task is the next one to execute for its
        // (dataset, variable) pair.
        if (task.dv_seq != state.exec_seq) {
            // Not ready yet -- re-enqueue and try something else.
            write_queue_.push(std::move(task));
            // Try a prefetch task instead.
            if (!prefetch_queue_.empty()) {
                PrefetchTask ptask = std::move(const_cast<PrefetchTask&>(prefetch_queue_.top()));
                prefetch_queue_.pop();

                in_flight_.fetch_add(1, std::memory_order_acq_rel);
                lock.unlock();

                // Execute prefetch callback with exception cordon
                // (R12.1, R12.2).
                if (ptask.handle_id != 0) {
                    execute_with_exception_cordon(ptask.callback, ptask.handle_id, io_comm_, outcome_registry_);
                } else {
                    try {
                        if (ptask.callback) {
                            ptask.callback();
                        }
                    }
#ifdef AMIO_HAS_ECKIT
                    catch (const eckit::Exception& e) {
                        emit_parallel_stacktrace(io_comm_, AMIO_ERR_BACKEND_FAILURE, e.what());
                    }
#endif
                    catch (const std::exception& e) {
                        emit_parallel_stacktrace(io_comm_, AMIO_ERR_BACKEND_FAILURE, e.what());
                    } catch (...) {
                        emit_parallel_stacktrace(io_comm_, AMIO_ERR_BACKEND_FAILURE, "Unknown exception (non-std)");
                    }
                }

                prefetches_completed_.fetch_add(1, std::memory_order_release);
                in_flight_.fetch_sub(1, std::memory_order_acq_rel);
                drain_cv_.notify_all();
                return true;
            }
            return false;
        }

        in_flight_.fetch_add(1, std::memory_order_acq_rel);
        lock.unlock();"""

new_code = """    // --- Try write queue ---
    if (!write_queue_.empty()) {
        std::size_t initial_size = write_queue_.size();
        for (std::size_t tried = 0; tried < initial_size; ++tried) {
            WriteTask task = std::move(write_queue_.front());
            write_queue_.pop();

            DvOrderState& state = get_dv_state(task.dv_key);

            if (task.dv_seq == state.exec_seq) {
                // Ready!
                in_flight_.fetch_add(1, std::memory_order_acq_rel);
                lock.unlock();"""

content = content.replace(old_code, new_code)

# Fix the end of the if block
old_end = """            // Advance the execution sequence counter.
            state.exec_seq++;
        }

        writes_completed_.fetch_add(1, std::memory_order_release);
        in_flight_.fetch_sub(1, std::memory_order_acq_rel);

        // Notify: drain waiters and other workers that may have
        // re-enqueued tasks waiting on this sequence.
        // Also notify backpressure waiters (R6.8): queue depth
        // decreased, so blocked writers may proceed.
        drain_cv_.notify_all();
        cv_.notify_all();
        backpressure_cv_.notify_all();
        return true;
    }"""

new_end = """            // Advance the execution sequence counter.
                    state.exec_seq++;
                }

                writes_completed_.fetch_add(1, std::memory_order_release);
                in_flight_.fetch_sub(1, std::memory_order_acq_rel);

                // Notify: drain waiters and other workers that may have
                // re-enqueued tasks waiting on this sequence.
                // Also notify backpressure waiters (R6.8): queue depth
                // decreased, so blocked writers may proceed.
                drain_cv_.notify_all();
                cv_.notify_all();
                backpressure_cv_.notify_all();
                return true;
            } else {
                // Not ready yet -- re-enqueue.
                write_queue_.push(std::move(task));
            }
        }
    }"""

content = content.replace(old_end, new_end)

with open('src/workers/worker_pool.cpp', 'w') as f:
    f.write(content)
