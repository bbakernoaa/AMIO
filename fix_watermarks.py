import sys

with open('src/workers/worker_pool.cpp', 'r') as f:
    content = f.read()

# Fix first instance
content = content.replace('return write_queue_.size() < backpressure_.low_watermark', 'return write_queue_.size() <= backpressure_.low_watermark')

with open('src/workers/worker_pool.cpp', 'w') as f:
    f.write(content)
