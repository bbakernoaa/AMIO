/*
 * amio.h -- AMIO public umbrella header (C99 surface).
 *
 * This is the single entry point that downstream consumers
 * (Fortran iso_c_binding, C, C++) include to access the AMIO_C_API.
 * It composes the four leaf headers that together carry the full
 * Phase 1 public ABI:
 *
 *   amio_export.h      AMIO_API public-symbol export macro (R10.8)
 *   amio_errors.h      stable AMIO_ERR_* enumeration + amio_strerror
 *                      declaration (R10.8, R12.5 - R12.8)
 *   amio_types.h       amio_dtype_t, amio_shape_t, amio_bbox_t,
 *                      opaque handle typedefs (R10.5, R10.6, R10.7)
 *   amio_mdspan_fwd.h  opaque forward declarations of kokkos/mdspan
 *                      shape descriptors (R10.1, R10.2)
 *
 * The function-symbol declarations live below: every public AMIO_C_API
 * entry point exported from libamio.so is declared here with C linkage
 * and default visibility (`AMIO_API`).  Implementations live in
 * `src/c_boundary/amio_api.cpp` (task 3.2) and `src/c_boundary/
 * amio_strerror.cpp` (task 3.3); function bodies for write/read/flush/
 * close/wait/release_view are stubbed in 3.2 and replaced in tasks
 * 4.x, 6.x, 9.x.
 *
 * Header-isolation contract (R10.1, R10.2, R10.3, R13.6):
 *   - This header transitively includes only:
 *       <stdint.h>, <stddef.h>, <stdbool.h>
 *     plus the leaf headers listed above (which themselves obey
 *     the same constraint).
 *   - It MUST NOT include any C++ standard library header, nor any
 *     header from eckit, TensorStore, netCDF, HDF5, nceplibs-g2c,
 *     or any other AMIO_Core dependency.
 *   - All declarations are wrapped in
 *         #ifdef __cplusplus
 *         extern "C" { ... }
 *         #endif
 *     blocks (R10.3) so a C++ host application includes the header
 *     without name-mangling its symbols.
 *   - It MUST be compilable with:
 *         gcc -std=c99 -pedantic -Werror -c amio.h
 *     and with:
 *         g++ -std=c++20 -c amio.h
 *     using only this project's `include/` directory on the
 *     compile-line `-I` set.
 *
 * Validates: R10.1, R10.2, R10.3, R10.5, R10.6, R10.7, R10.8,
 *            R12.2, R12.5, R13.6
 */

#ifndef AMIO_AMIO_H
#define AMIO_AMIO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "amio/amio_export.h"
#include "amio/amio_errors.h"
#include "amio/amio_mdspan_fwd.h"
#include "amio/amio_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AMIO public ABI version macros.
 *
 * The major version pins the integer values of amio_err_t and the
 * layout of every public ABI struct.  Bumping the major version is
 * an ABI break (R10.8); bumping minor / patch is not.
 */
#define AMIO_ABI_VERSION_MAJOR 0
#define AMIO_ABI_VERSION_MINOR 1
#define AMIO_ABI_VERSION_PATCH 0

/* -----------------------------------------------------------------
 * AMIO_C_API entry points
 *
 * Every function below has C linkage, default visibility, and
 * returns `amio_status_t` (an int32_t alias of the AMIO_ERR_*
 * enumeration).  The C-Boundary translation layer in
 * `src/c_boundary/amio_api.cpp` validates each handle via the
 * HandleTable (generation-counter checked, R10.6 / R10.7), looks
 * up the AMIO_Core context, calls into the private C++ API,
 * catches every exception class (eckit::Exception, std::exception,
 * `...`) and translates it to an AMIO_ERR_* code (R12.2).
 *
 * Bodies for `init` and `finalize` arrive in task 9.4; bodies for
 * the I/O entry points arrive in tasks 4.x / 6.x / 9.x.  In task
 * 3.2 they are stub wrappers that exercise the handle-validation
 * + exception-translation cordon and return AMIO_ERR_BACKEND_FAILURE
 * pending the full implementation.
 * ----------------------------------------------------------------- */

/*
 * amio_open_dataset -- open a dataset for reading or writing through
 *                      the Backend_Factory (R4.1, R4.6, R6.4, R6.5).
 *
 * `core` is a valid, initialized AMIO_Core handle returned by
 * amio_init.  `config_path` is a NUL-terminated path to a YAML or
 * JSON dataset configuration manifest.  `mode` is either
 * AMIO_MODE_WRITE or AMIO_MODE_READ.  `out_dataset` MUST be a
 * non-NULL pointer; on success the dereferenced value is set to a
 * freshly-minted dataset handle.  On failure `*out_dataset` is set
 * to NULL.
 *
 * Returns:
 *   AMIO_OK
 *   AMIO_ERR_NULL_HANDLE            (core == NULL)
 *   AMIO_ERR_INVALID_HANDLE         (core stale / wrong kind)
 *   AMIO_ERR_INVALID_INPUT          (NULL arguments, invalid mode)
 *   AMIO_ERR_UNKNOWN_BACKEND        (R4.6)
 *   AMIO_ERR_BACKEND_FAILURE        (driver open failed)
 */
AMIO_API amio_status_t amio_open_dataset(amio_core_handle core,
                                         const char *config_path,
                                         int32_t mode,
                                         amio_dataset_handle *out_dataset);

/*
 * amio_close_dataset -- flush pending writes, surface failures, and
 *                       release Backend_Driver resources (R6.4, R6.5).
 *
 * Equivalent to calling amio_flush followed by releasing the driver.
 * After this call, the dataset handle is invalidated.
 *
 * Returns:
 *   AMIO_OK
 *   AMIO_ERR_NULL_HANDLE            (dataset == NULL)
 *   AMIO_ERR_INVALID_HANDLE         (dataset stale / wrong kind)
 *   AMIO_ERR_VIEWS_OUTSTANDING      (R5.10)
 *   AMIO_ERR_BACKEND_FAILURE        (flush or close failed)
 */
AMIO_API amio_status_t amio_close_dataset(amio_dataset_handle dataset);

/*
 * amio_init -- create an AMIO_Core context from a configuration
 *              manifest (R1.1 - R1.6, R1.11).
 *
 * `manifest_path` MUST be a non-NULL, NUL-terminated path to a YAML
 * or JSON manifest.  `out_core` MUST be a non-NULL pointer; on
 * success the dereferenced value is set to a freshly-minted opaque
 * core handle.  On failure `*out_core` is set to NULL.
 *
 * Returns:
 *   AMIO_OK
 *   AMIO_ERR_INVALID_INPUT          (NULL arguments)
 *   AMIO_ERR_MANIFEST_NOT_FOUND     (R1.4)
 *   AMIO_ERR_MANIFEST_INVALID       (R1.5)
 *   AMIO_ERR_INVALID_BINDING        (R3.3)
 *   AMIO_ERR_COMM_SPLIT_FAILED      (R3.6)
 *   AMIO_ERR_THREADING_UNSUPPORTED  (R3.8)
 *   AMIO_ERR_BACKEND_FAILURE        (any other failure)
 */
AMIO_API amio_status_t amio_init(const char *manifest_path,
                                 amio_core_handle *out_core);

/*
 * amio_finalize -- drain pending I/O, release Staging_Pool, join
 *                  Worker_Pool, and invalidate `core` (R1.7 - R1.9).
 *
 * Returns:
 *   AMIO_OK
 *   AMIO_ERR_NULL_HANDLE            (core == NULL)
 *   AMIO_ERR_INVALID_HANDLE         (stale or never-initialized)
 *   AMIO_ERR_FINALIZE_TIMEOUT       (R1.8)
 *   AMIO_ERR_BACKEND_FAILURE        (drain raised an exception)
 */
AMIO_API amio_status_t amio_finalize(amio_core_handle core);

/*
 * amio_write -- synchronously snapshot a Memory_View into a
 *               Staging_Pool buffer and enqueue an asynchronous
 *               write task (R2.1 - R2.10, R6.1).
 *
 * `dataset` is a write dataset handle obtained from amio_open_*
 * (added in task 6.3).  `var_name` is a NUL-terminated variable
 * identifier.  `host_data` is the host-side payload pointer.
 * `dtype` and `shape` describe the payload.  On success
 * `*out_io` receives the per-write opaque handle (R2.7).
 *
 * Returns:
 *   AMIO_OK
 *   AMIO_ERR_NULL_HANDLE            (dataset == NULL)
 *   AMIO_ERR_INVALID_HANDLE         (dataset stale / wrong kind)
 *   AMIO_ERR_INVALID_INPUT          (R2.10)
 *   AMIO_ERR_STAGING_BACKPRESSURE   (R2.6)
 *   AMIO_ERR_QUEUE_FULL             (R6.9)
 *   AMIO_ERR_BACKEND_FAILURE        (R6.6, R7.6, R8.9, R9.x)
 */
AMIO_API amio_status_t amio_write(amio_dataset_handle dataset,
                                  const char *var_name,
                                  const void *host_data,
                                  amio_dtype_t dtype,
                                  const amio_shape_t *shape,
                                  amio_io_handle *out_io);

/*
 * amio_read -- return a Memory_View backed by a prefetched
 *              Staging_Pool buffer for `(var_name, timestep)`
 *              (R5.3 - R5.5, R5.7).
 *
 * `bbox` may be NULL to request a full-payload read; when non-NULL
 * the driver is asked for only the intersecting byte ranges (R5.7).
 * On success `*out_view` receives the opaque view handle that the
 * caller MUST release with amio_release_view (R5.6, R5.9).
 *
 * Returns:
 *   AMIO_OK
 *   AMIO_ERR_NULL_HANDLE            (dataset == NULL)
 *   AMIO_ERR_INVALID_HANDLE         (dataset stale / wrong kind)
 *   AMIO_ERR_INVALID_INPUT          (R2.10)
 *   AMIO_ERR_TIMEOUT                (R5.5)
 *   AMIO_ERR_BACKEND_FAILURE        (R5.8)
 */
AMIO_API amio_status_t amio_read(amio_dataset_handle dataset,
                                 const char *var_name,
                                 int64_t timestep,
                                 const amio_bbox_t *bbox,
                                 amio_view_handle *out_view);

/*
 * amio_flush -- block until every write task previously submitted
 *               against `dataset` has either completed or recorded
 *               a failure (R6.4, R6.10).
 *
 * `timeout_ms` in [0, 86_400_000]; 0 indicates an indefinite wait.
 *
 * Returns:
 *   AMIO_OK
 *   AMIO_ERR_NULL_HANDLE            (dataset == NULL)
 *   AMIO_ERR_INVALID_HANDLE         (dataset stale / wrong kind)
 *   AMIO_ERR_TIMEOUT                (R6.10)
 *   AMIO_ERR_BACKEND_FAILURE        (R6.6 - some task failed)
 */
AMIO_API amio_status_t amio_flush(amio_dataset_handle dataset,
                                  int64_t timeout_ms);

/*
 * amio_close -- flush all pending write tasks for `dataset`,
 *               surface any failures, and release the underlying
 *               Backend_Driver resources (R5.9 - R5.10, R6.5).
 *
 * Returns:
 *   AMIO_OK
 *   AMIO_ERR_NULL_HANDLE            (dataset == NULL)
 *   AMIO_ERR_INVALID_HANDLE         (dataset stale / wrong kind)
 *   AMIO_ERR_VIEWS_OUTSTANDING      (R5.10)
 *   AMIO_ERR_BACKEND_FAILURE        (R6.6)
 */
AMIO_API amio_status_t amio_close(amio_dataset_handle dataset);

/*
 * amio_wait -- block until the asynchronous I/O operation
 *              identified by `io` completes, fails, or the
 *              timeout elapses (R3.9).
 *
 * `timeout_ms` in [0, 86_400_000]; 0 indicates an indefinite wait.
 *
 * Returns:
 *   AMIO_OK                         (operation completed)
 *   AMIO_ERR_NULL_HANDLE            (io == NULL)
 *   AMIO_ERR_INVALID_HANDLE         (io stale / wrong kind)
 *   AMIO_ERR_TIMEOUT                (R3.9)
 *   AMIO_ERR_BACKEND_FAILURE        (operation failed)
 */
AMIO_API amio_status_t amio_wait(amio_io_handle io, int64_t timeout_ms);

/*
 * amio_release_view -- drop a reference to a Staging_Pool buffer
 *                      previously returned by amio_read (R5.6, R5.9).
 *
 * Once the last reference is dropped the buffer returns to the
 * Staging_Pool free list.
 *
 * Returns:
 *   AMIO_OK
 *   AMIO_ERR_NULL_HANDLE            (view == NULL)
 *   AMIO_ERR_INVALID_HANDLE         (view stale / wrong kind / already released)
 */
AMIO_API amio_status_t amio_release_view(amio_view_handle view);

/*
 * Note: `amio_strerror` is declared in `include/amio/amio_errors.h`
 * (its natural home alongside the AMIO_ERR_* enumeration it
 * describes).  Including this umbrella header transitively pulls
 * that declaration into the consumer's translation unit.  R12.5 -
 * R12.8 (table contract, defined codes byte-equal across calls,
 * undefined codes -> "AMIO_ERR_UNKNOWN(<int>)", lazy-init satisfied
 * trivially by `constexpr`) are validated by task 3.3's unit tests
 * and by property test P17 (task 13.18).
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* AMIO_AMIO_H */
