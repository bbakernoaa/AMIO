/*
 * amio_types.h -- AMIO public ABI types (C99 surface).
 *
 * Declares the data-type tags, shape descriptor, and opaque handle
 * typedefs that every AMIO_C_API entry point consumes or returns.
 *
 * Header-isolation contract (R10.1, R10.2, R13.6):
 *   - Includes only <stdint.h>, <stddef.h>, <stdbool.h>, and the
 *     companion mdspan forward-declaration header.
 *   - No C++ types, no template, namespace, reference, or std::
 *     symbol.  No third-party (eckit / TensorStore / netCDF /
 *     HDF5 / nceplibs-g2c) header.
 *   - All declarations wrapped in `extern "C"` for C++ inclusion
 *     (R10.3).
 *   - Compiles cleanly under
 *         gcc -std=c99 -pedantic -Werror -c amio_types.h
 *
 * Validates: R10.1, R10.2, R10.3, R10.5, R12.5, R13.6
 */

#ifndef AMIO_AMIO_TYPES_H
#define AMIO_AMIO_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "amio/amio_mdspan_fwd.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum supported rank for any AMIO Memory_View shape descriptor.
 * Anchored by R2.1 ("a shape descriptor of 1 to 7 dimensions") and
 * fixed for the library lifetime.  Hosts MUST NOT pass `rank > 7`
 * or `rank < 1`; doing so yields AMIO_ERR_INVALID_INPUT (R2.10).
 */
#define AMIO_MAX_RANK 7

/*
 * amio_dtype_t -- element-type tag carried alongside every payload
 * crossing the FFI boundary.  Integer values are FROZEN; new tags
 * are appended only.  The set below is the full Phase 1 surface.
 *
 * Width and signedness must agree with the corresponding Fortran
 * iso_c_binding kinds declared in amio_mod (R10.4, R1.12):
 *   F32 <-> c_float            F64 <-> c_double
 *   I8  <-> c_int8_t           I16 <-> c_int16_t
 *   I32 <-> c_int32_t          I64 <-> c_int64_t
 *   U8  <-> c_int8_t (unsigned variant)
 *   U16 <-> c_int16_t (unsigned variant)
 *   U32 <-> c_int32_t (unsigned variant)
 *   U64 <-> c_int64_t (unsigned variant)
 */
typedef enum amio_dtype_e {
    AMIO_DTYPE_F32 = 0,
    AMIO_DTYPE_F64 = 1,
    AMIO_DTYPE_I8  = 2,
    AMIO_DTYPE_I16 = 3,
    AMIO_DTYPE_I32 = 4,
    AMIO_DTYPE_I64 = 5,
    AMIO_DTYPE_U8  = 6,
    AMIO_DTYPE_U16 = 7,
    AMIO_DTYPE_U32 = 8,
    AMIO_DTYPE_U64 = 9
    /* Extension space reserved; do not renumber existing tags. */
} amio_dtype_t;

/*
 * amio_shape_t -- N-dimensional shape descriptor.
 *
 *   rank      :  number of valid leading entries in extents/strides
 *                in [1, AMIO_MAX_RANK].  Values outside this range
 *                are rejected by the C-Boundary with
 *                AMIO_ERR_INVALID_INPUT (R2.10).
 *   extents   :  per-dimension extent in elements, row-major.
 *                Entries beyond `rank` MUST be zero.  Each in-range
 *                extent MUST be strictly positive.
 *   strides   :  per-dimension stride in *elements* (not bytes).
 *                A stride entry of 0 marks the dimension as
 *                contiguous and asks AMIO to derive the stride
 *                from `extents` row-major.  Entries beyond `rank`
 *                MUST be zero.
 *
 * The element size in bytes is derived from `amio_dtype_t`; the
 * total payload byte count is therefore
 *     sizeof(dtype) * product(extents[0..rank-1])
 * and is bounded by the AMIO manifest's per-buffer capacity (1 byte
 * to 1 GiB, R1.3, R1.5).
 */
typedef struct amio_shape_s {
    int32_t rank;
    int64_t extents[AMIO_MAX_RANK];
    int64_t strides[AMIO_MAX_RANK];
} amio_shape_t;

/*
 * Opaque handle typedefs (R10.5, R10.6, R10.7).
 *
 * Every public AMIO_C_API entry point that returns or consumes a
 * stateful object expresses it as a `void *` opaque handle.  Host
 * code MUST NOT dereference any of these pointers; the C-Boundary
 * handle table validates them on every call (generation counter
 * check) and returns AMIO_ERR_INVALID_HANDLE on reuse-after-free.
 *
 *   amio_core_handle      AMIO_Core context (owner of Staging_Pool,
 *                         Worker_Pool, Backend_Factory, Prefetch_
 *                         Queue).  Returned by amio_init,
 *                         invalidated by amio_finalize.
 *   amio_dataset_handle   Open dataset (read or write).  Returned
 *                         by amio_open_*, invalidated by
 *                         amio_close_*.
 *   amio_io_handle        Pending asynchronous I/O operation
 *                         (per-write or per-prefetch).  Returned
 *                         by amio_write / amio_read, invalidated
 *                         on completion or error.
 *   amio_view_handle      Outstanding read-side Memory_View
 *                         referencing a Staging_Pool buffer.
 *                         Released by amio_release_view (R5.6,
 *                         R5.9, R5.10).
 */
typedef void *amio_core_handle;
typedef void *amio_dataset_handle;
typedef void *amio_io_handle;
typedef void *amio_view_handle;

/*
 * amio_io_kind_t -- distinguishes write and read dataset handles
 * at the FFI boundary so that misuse of a write handle on a read
 * entry point (and vice versa) is detected as
 * AMIO_ERR_INVALID_HANDLE without dereferencing the handle.
 */
typedef enum amio_io_kind_e {
    AMIO_IO_KIND_WRITE = 0,
    AMIO_IO_KIND_READ  = 1
} amio_io_kind_t;

/*
 * AMIO_MODE_* -- dataset open mode constants for amio_open_dataset.
 *
 * These are passed as the `mode` parameter to amio_open_dataset to
 * indicate whether the dataset should be opened for writing or
 * reading.
 */
#define AMIO_MODE_WRITE 0
#define AMIO_MODE_READ  1

/*
 * amio_bbox_t -- optional bounding-box / stride descriptor for
 * selective reads (R5.7).  Passed as a pointer that may be NULL
 * to request a full-payload read.  When non-NULL, every entry of
 * `extents` and `offsets` beyond `rank` MUST be zero, and the
 * bounding box MUST lie wholly inside the dataset's variable
 * shape.
 */
typedef struct amio_bbox_s {
    int32_t rank;
    int64_t offsets[AMIO_MAX_RANK];
    int64_t extents[AMIO_MAX_RANK];
    int64_t strides[AMIO_MAX_RANK];
} amio_bbox_t;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* AMIO_AMIO_TYPES_H */
