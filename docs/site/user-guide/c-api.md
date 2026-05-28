# C API

The AMIO C API provides a flat, C99-compatible interface for writing and reading
multidimensional scientific data. All state is hidden behind opaque `void*`
handles, making the API safe to call from C, C++, and Fortran.

## Header

```c
#include <amio/amio.h>
```

This single umbrella header transitively includes:

- `amio_export.h` — `AMIO_API` visibility macro
- `amio_errors.h` — `AMIO_ERR_*` enumeration and `amio_strerror`
- `amio_types.h` — Data types, shapes, and handle typedefs

## Lifecycle

The AMIO lifecycle follows a strict sequence:

```mermaid
flowchart LR
    A[amio_init] --> B[amio_open_dataset]
    B --> C[amio_write / amio_read]
    C --> D[amio_flush]
    D --> E[amio_close_dataset]
    E --> F[amio_finalize]
```

### 1. Initialization

```c
amio_core_handle core = NULL;
amio_status_t rc = amio_init("manifest.yaml", &core);
if (rc != AMIO_OK) {
    fprintf(stderr, "Init failed: %s\n", amio_strerror(rc));
    return 1;
}
```

`amio_init` creates the AMIO_Core context which owns:

- The staging memory pool
- The worker thread pool
- The backend factory
- The prefetch queue

### 2. Open Dataset

```c
amio_dataset_handle dataset = NULL;
rc = amio_open_dataset(core, "config.yaml", AMIO_MODE_WRITE, &dataset);
```

Modes:

- `AMIO_MODE_WRITE` — Create or overwrite a file
- `AMIO_MODE_READ` — Open an existing file for reading

### 3. Write Data

```c
amio_shape_t shape = {0};
shape.rank = 3;
shape.extents[0] = 100;  // x
shape.extents[1] = 50;   // y
shape.extents[2] = 25;   // z
// strides = 0 means contiguous (AMIO derives row-major strides)

amio_io_handle io = NULL;
rc = amio_write(dataset, "temperature", data_ptr,
                AMIO_DTYPE_F32, &shape, &io);
```

!!! important "Pointer Safety"
    `amio_write` deep-copies the host data into the staging pool **synchronously**.
    After the call returns, the host buffer can be safely reused or freed.
    No worker thread retains a reference to the host pointer.

### 4. Read Data

```c
amio_view_handle view = NULL;
rc = amio_read(dataset, "temperature", timestep, NULL, &view);
// Use the view...
rc = amio_release_view(view);  // MUST release when done
```

!!! warning "View Lifetime"
    The returned `Memory_View` points into a staging pool buffer. You **must**
    call `amio_release_view` when done. Closing a dataset with outstanding
    views returns `AMIO_ERR_VIEWS_OUTSTANDING`.

### 5. Flush and Close

```c
// Wait for all pending writes (0 = wait indefinitely)
rc = amio_flush(dataset, 0);

// Close the dataset (also flushes)
rc = amio_close_dataset(dataset);

// Finalize AMIO (drains workers, frees staging pool)
rc = amio_finalize(core);
```

## Error Handling

Every AMIO function returns an `amio_status_t` (int32_t). Check against `AMIO_OK`:

```c
amio_status_t rc = amio_init("manifest.yaml", &core);
if (rc != AMIO_OK) {
    fprintf(stderr, "Error: %s (code %d)\n", amio_strerror(rc), (int)rc);
}
```

`amio_strerror` maps any error code to a human-readable string with static
storage duration. It is thread-safe and never returns NULL.

## Data Types

| Tag | C Type | Size |
|-----|--------|------|
| `AMIO_DTYPE_F32` | `float` | 4 bytes |
| `AMIO_DTYPE_F64` | `double` | 8 bytes |
| `AMIO_DTYPE_I8` | `int8_t` | 1 byte |
| `AMIO_DTYPE_I16` | `int16_t` | 2 bytes |
| `AMIO_DTYPE_I32` | `int32_t` | 4 bytes |
| `AMIO_DTYPE_I64` | `int64_t` | 8 bytes |
| `AMIO_DTYPE_U8` | `uint8_t` | 1 byte |
| `AMIO_DTYPE_U16` | `uint16_t` | 2 bytes |
| `AMIO_DTYPE_U32` | `uint32_t` | 4 bytes |
| `AMIO_DTYPE_U64` | `uint64_t` | 8 bytes |

## Shape Descriptor

The `amio_shape_t` struct describes the layout of a multidimensional array:

```c
typedef struct {
    int32_t rank;           // Number of dimensions [1, 7]
    int64_t extents[7];     // Per-dimension size in elements
    int64_t strides[7];     // Per-dimension stride (0 = contiguous)
} amio_shape_t;
```

- **rank**: Must be between 1 and `AMIO_MAX_RANK` (7)
- **extents**: Each must be strictly positive
- **strides**: Set to 0 for contiguous arrays (AMIO derives row-major strides)

## Handle Types

All handles are opaque `void*` pointers. Never dereference them directly.

| Handle | Created by | Invalidated by |
|--------|-----------|----------------|
| `amio_core_handle` | `amio_init` | `amio_finalize` |
| `amio_dataset_handle` | `amio_open_dataset` | `amio_close_dataset` |
| `amio_io_handle` | `amio_write` / `amio_read` | Completion or error |
| `amio_view_handle` | `amio_read` | `amio_release_view` |
