import sys

with open('src/workers/worker_pool.cpp', 'r') as f:
    content = f.read()

old_code = """            // Otherwise, loop back and wait again.  This can happen
            // when a write task is not yet ready (out-of-order seq).
        }"""

new_code = """            // Otherwise, wait for a short duration if the queue is not empty but no task is ready
            // to avoid high-CPU busy waiting while scanning the queue.
            if (!write_queue_.empty() || !prefetch_queue_.empty()) {
                cv_.wait_for(lock, std::chrono::milliseconds(10));
            }
        }"""

content = content.replace(old_code, new_code)

with open('src/workers/worker_pool.cpp', 'w') as f:
    f.write(content)
