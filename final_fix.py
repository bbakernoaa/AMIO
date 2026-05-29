import sys

with open('tests/pbt/test_p23_driver_failure_recorded.cpp', 'r') as f:
    content = f.read()

old_ctx = """    FailureTestContext() {
        std::string yaml = make_manifest_yaml("netcdf4", 8, 65536, 1, 5000);
        manifest_path = write_manifest(dir, yaml);

        amio_status_t rc = amio_init(manifest_path.c_str(), &core);
        if (rc != AMIO_OK || core == nullptr) {
            return;
        }

        // Open a dataset (uses the real factory, but we'll test
        // failure recording through the flush/close path).
        std::string ds_yaml = make_dataset_config_yaml("netcdf4", dir.file("output.nc"));
        std::string ds_path = dir.file("dataset.yaml");
        std::ofstream ofs(ds_path);
        ofs << ds_yaml;
        ofs.close();

        rc = amio_open_dataset(core, ds_path.c_str(), AMIO_MODE_WRITE, &dataset);
        if (rc != AMIO_OK || dataset == nullptr) {
            amio_finalize(core);
            core = nullptr;
            return;
        }

        valid = true;
    }

    ~FailureTestContext() {
        if (dataset) {
            amio_close_dataset(dataset);
        }
        if (core) {
            amio_finalize(core);
        }
    }"""

new_ctx = """    FailureTestContext() {
        std::string yaml = make_manifest_yaml("netcdf4", 8, 65536, 1, 5000);
        manifest_path = write_manifest(dir, yaml);

        amio_status_t rc = amio::detail::init(manifest_path.c_str(), &core);
        if (rc != AMIO_OK || core == nullptr) {
            return;
        }

        // Open a dataset (uses the real factory, but we'll test
        // failure recording through the flush/close path).
        std::string ds_yaml = make_dataset_config_yaml("netcdf4", dir.file("output.nc"));
        std::string ds_path = dir.file("dataset.yaml");
        std::ofstream ofs(ds_path);
        ofs << ds_yaml;
        ofs.close();

        void* core_payload = nullptr;
        process_handle_table().lookup(HandleTable::from_ptr(core), HandleKind::Core, &core_payload);

        rc = amio::detail::open_dataset(core_payload, ds_path.c_str(), AMIO_MODE_WRITE, &dataset);
        if (rc != AMIO_OK || dataset == nullptr) {
            amio::detail::finalize(core_payload);
            core = nullptr;
            return;
        }

        valid = true;
    }

    ~FailureTestContext() {
        if (dataset) {
            void* ds_payload = nullptr;
            process_handle_table().lookup(HandleTable::from_ptr(dataset), HandleKind::Dataset, &ds_payload);
            if (ds_payload) amio::detail::close_dataset(ds_payload);
        }
        if (core) {
            void* core_payload = nullptr;
            process_handle_table().lookup(HandleTable::from_ptr(core), HandleKind::Core, &core_payload);
            if (core_payload) amio::detail::finalize(core_payload);
        }
    }"""

content = content.replace(old_ctx, new_ctx)

old_lookup = """        // Look up the DatasetRecord to inject a failure.
        // This simulates what the worker pool does when a driver
        // throws during serialization.
        auto& table = process_handle_table();
        HandleKind kind;
        void* payload = nullptr;
        table.lookup(HandleTable::from_ptr(ctx.dataset), kind, &payload);
        if (!payload || kind != HandleKind::Dataset) {
            RC_DISCARD("invalid handle");
        }"""

new_lookup = """        // Look up the DatasetRecord to inject a failure.
        // This simulates what the worker pool does when a driver
        // throws during serialization.
        void* payload = nullptr;
        process_handle_table().lookup(HandleTable::from_ptr(ctx.dataset), HandleKind::Dataset, &payload);
        if (!payload) {
            RC_DISCARD("invalid handle");
        }"""

content = content.replace(old_lookup, new_lookup)

old_lookup_b = """        auto& table = process_handle_table();
        HandleKind kind;
        void* payload = nullptr;
        table.lookup(HandleTable::from_ptr(ctx.dataset), kind, &payload);
        if (!payload || kind != HandleKind::Dataset) {
            RC_DISCARD("invalid handle");
        }"""

new_lookup_b = """        void* payload = nullptr;
        process_handle_table().lookup(HandleTable::from_ptr(ctx.dataset), HandleKind::Dataset, &payload);
        if (!payload) {
            RC_DISCARD("invalid handle");
        }"""

content = content.replace(old_lookup_b, new_lookup_b)

old_lookup_d = """        auto& table = process_handle_table();
        HandleKind kind;
        void* payload = nullptr;
        table.lookup(HandleTable::from_ptr(ctx.dataset), kind, &payload);
        if (!payload || kind != HandleKind::Dataset) {
            RC_DISCARD("invalid handle");
        }"""

new_lookup_d = """        void* payload = nullptr;
        process_handle_table().lookup(HandleTable::from_ptr(ctx.dataset), HandleKind::Dataset, &payload);
        if (!payload) {
            RC_DISCARD("invalid handle");
        }"""

content = content.replace(old_lookup_d, new_lookup_d)

with open('tests/pbt/test_p23_driver_failure_recorded.cpp', 'w') as f:
    f.write(content)
