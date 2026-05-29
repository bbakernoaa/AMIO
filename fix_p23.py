import sys

with open('tests/pbt/test_p23_driver_failure_recorded.cpp', 'r') as f:
    content = f.read()

# Fix 1: surfaces on flush
old_p23a = """        // Now flush should surface the failure.
        amio_status_t flush_rc = amio_flush(ctx.dataset, 1000);
        RC_ASSERT(flush_rc == AMIO_ERR_BACKEND_FAILURE);"""

new_p23a = """        // Now flush should surface the failure.
        amio_status_t flush_rc = amio::detail::flush(record, 1000);
        RC_ASSERT(flush_rc == AMIO_ERR_BACKEND_FAILURE);"""

content = content.replace(old_p23a, new_p23a)

# Fix 2: surfaces consistently
old_p23b = """        for (int i = 0; i < num_flushes; ++i) {
            amio_status_t flush_rc = amio_flush(ctx.dataset, 100);
            RC_ASSERT(flush_rc == static_cast<amio_status_t>(err_code));
        }"""

new_p23b = """        for (int i = 0; i < num_flushes; ++i) {
            amio_status_t flush_rc = amio::detail::flush(record, 100);
            RC_ASSERT(flush_rc == static_cast<amio_status_t>(err_code));
        }"""

content = content.replace(old_p23b, new_p23b)

# Fix 3: error code preserved
old_p23d = """        amio_status_t flush_rc = amio_flush(ctx.dataset, 1000);

        // The exact error code should be surfaced.
        RC_ASSERT(flush_rc == AMIO_ERR_BACKEND_FAILURE);"""

new_p23d = """        amio_status_t flush_rc = amio::detail::flush(record, 1000);

        // The exact error code should be surfaced.
        RC_ASSERT(flush_rc == AMIO_ERR_BACKEND_FAILURE);"""

content = content.replace(old_p23d, new_p23d)

# Fix the amio_write and amio_flush in p23c
old_p23c = """        std::vector<uint8_t> data(byte_count, 0x77);
        amio_io_handle io = nullptr;
        amio_status_t write_rc = amio_write(ctx.dataset, "success_var", data.data(), dtype, &shape, &io);
        RC_PRE(write_rc == AMIO_OK);

        // Flush should return AMIO_OK (no failure recorded).
        amio_status_t flush_rc = amio_flush(ctx.dataset, 1000);
        RC_ASSERT(flush_rc == AMIO_OK);"""

new_p23c = """        std::vector<uint8_t> data(byte_count, 0x77);
        amio_io_handle io = nullptr;

        void* ds_payload = nullptr;
        process_handle_table().lookup(HandleTable::from_ptr(ctx.dataset), HandleKind::Dataset, &ds_payload);

        amio_status_t write_rc = amio::detail::write(ds_payload, "success_var", data.data(), dtype, &shape, &io);
        RC_PRE(write_rc == AMIO_OK);

        // Flush should return AMIO_OK (no failure recorded).
        amio_status_t flush_rc = amio::detail::flush(ds_payload, 1000);
        RC_ASSERT(flush_rc == AMIO_OK);"""

content = content.replace(old_p23c, new_p23c)

with open('tests/pbt/test_p23_driver_failure_recorded.cpp', 'w') as f:
    f.write(content)
