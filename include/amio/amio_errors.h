/*
 * amio_errors.h -- AMIO public error code enumeration (C99 surface).
 *
 * Implements the stable, ABI-frozen `AMIO_ERR_*` enumeration that
 * every public AMIO_C_API entry point returns.  Integer values are
 * fixed for the library lifetime (R10.8, R12.5); new codes are
 * appended only -- existing values MUST NOT be renumbered or
 * reassigned across releases.
 *
 * Header-isolation contract (R10.1, R10.2, R13.6):
 *   - Includes only <stdint.h> and the AMIO_API export-macro header.
 *   - No C++ types, no third-party (eckit / TensorStore / netCDF /
 *     HDF5 / nceplibs-g2c) headers, no std:: symbols.
 *   - All declarations wrapped in `extern "C"` for C++ inclusion
 *     (R10.3).
 *   - Compiles cleanly under
 *         gcc -std=c99 -pedantic -Werror -c amio_errors.h
 *
 * Validates: R10.1, R10.2, R10.3, R10.5, R10.8, R12.5, R13.6
 */

#ifndef AMIO_AMIO_ERRORS_H
#define AMIO_AMIO_ERRORS_H

#include <stdint.h>

#include "amio/amio_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AMIO_ERR_* -- public error code enumeration.
 *
 * The integer values below are FROZEN.  Adding a new code is
 * permitted only by appending it with the next unused integer.
 * Renumbering, removing, or reordering any existing code is an
 * ABI break and is forbidden across minor and patch releases.
 *
 * Code provenance / requirements traceability:
 *   AMIO_OK                         -- success sentinel (R10.8)
 *   AMIO_ERR_NULL_HANDLE            -- R1.9, R10.6
 *   AMIO_ERR_INVALID_HANDLE         -- R1.9, R1.10, R10.6, R10.7
 *   AMIO_ERR_MANIFEST_NOT_FOUND     -- R1.4
 *   AMIO_ERR_MANIFEST_INVALID       -- R1.5, R11.4
 *   AMIO_ERR_ALREADY_INITIALIZED    -- R1.6
 *   AMIO_ERR_FINALIZE_TIMEOUT       -- R1.8
 *   AMIO_ERR_STAGING_BACKPRESSURE   -- R2.6
 *   AMIO_ERR_INVALID_BINDING        -- R3.3
 *   AMIO_ERR_COMM_SPLIT_FAILED      -- R3.6
 *   AMIO_ERR_THREADING_UNSUPPORTED  -- R3.8
 *   AMIO_ERR_UNKNOWN_BACKEND        -- R4.6
 *   AMIO_ERR_LOSSY_CODEC_FORBIDDEN  -- R7.5, R8.4, R8.7, R11.6, R11.7
 *   AMIO_ERR_VIEWS_OUTSTANDING      -- R5.10
 *   AMIO_ERR_QUEUE_FULL             -- R6.9
 *   AMIO_ERR_TIMEOUT                -- R3.9, R5.5, R6.10
 *   AMIO_ERR_BACKEND_FAILURE        -- R6.6, R7.6, R8.9, R9.x
 *   AMIO_ERR_INVALID_INPUT          -- R2.10
 */
typedef enum amio_err_e {
    AMIO_OK                        = 0,
    AMIO_ERR_NULL_HANDLE           = 1,
    AMIO_ERR_INVALID_HANDLE        = 2,
    AMIO_ERR_MANIFEST_NOT_FOUND    = 3,
    AMIO_ERR_MANIFEST_INVALID      = 4,
    AMIO_ERR_ALREADY_INITIALIZED   = 5,
    AMIO_ERR_FINALIZE_TIMEOUT      = 6,
    AMIO_ERR_STAGING_BACKPRESSURE  = 7,
    AMIO_ERR_INVALID_BINDING       = 8,
    AMIO_ERR_COMM_SPLIT_FAILED     = 9,
    AMIO_ERR_THREADING_UNSUPPORTED = 10,
    AMIO_ERR_UNKNOWN_BACKEND       = 11,
    AMIO_ERR_LOSSY_CODEC_FORBIDDEN = 12,
    AMIO_ERR_VIEWS_OUTSTANDING     = 13,
    AMIO_ERR_QUEUE_FULL            = 14,
    AMIO_ERR_TIMEOUT               = 15,
    AMIO_ERR_BACKEND_FAILURE       = 16,
    AMIO_ERR_INVALID_INPUT         = 17
    /*
     * Extension space reserved.  Append future codes with the next
     * unused integer (18, 19, ...).  Do NOT reorder, do NOT remove,
     * do NOT renumber.
     */
} amio_err_t;

/*
 * Underlying integer type for transport across the FFI boundary.
 * The enumeration above is the canonical type, but a fixed-width
 * typedef is provided so that Fortran iso_c_binding wrappers and
 * external bindings can declare the return type independently of
 * the enum representation.
 */
typedef int32_t amio_status_t;

/*
 * amio_strerror -- map an AMIO error code integer to a stable,
 *                  human-readable, null-terminated description.
 *
 * Contract (R12.5, R12.6, R12.7, R12.8):
 *
 *   1. The return value is ALWAYS a non-null pointer to a
 *      null-terminated C string with static storage duration.
 *      Callers MUST NOT free or modify the returned string.
 *
 *   2. For any defined `AMIO_ERR_*` enumerator passed as `err`,
 *      repeated invocations return byte-equal descriptions.  The
 *      call is side-effect free with respect to AMIO_Core state
 *      (R12.6).
 *
 *   3. For any integer `err` outside the defined `AMIO_ERR_*` set
 *      (negative values, values >= the smallest unused enumerator,
 *      or otherwise reserved values), the return value is a
 *      non-null, null-terminated description of the form
 *          "AMIO_ERR_UNKNOWN(<int>)"
 *      where `<int>` is the decimal representation of `err`
 *      (R12.8).  No table mutation occurs.
 *
 *   4. The internal description table is a static `constexpr`
 *      array, so initialization completes before `main` runs and
 *      the lazy-init clause of R12.7 is satisfied trivially -- the
 *      first call returns the description directly without any
 *      first-use ordering hazard.
 *
 *   5. `err` is `int` (not `amio_err_t`) deliberately: the C
 *      standard permits passing arbitrary integer values to a
 *      function declared with an enum-typed parameter, but using
 *      `int` here makes the totality clause explicit at the type
 *      level and matches the property tested by P17.
 *
 *   6. The function is thread-safe: defined-code lookups are pure
 *      reads from a `constexpr` table; unknown-code formatting
 *      writes into a `thread_local` scratch buffer owned by the
 *      calling thread, so concurrent callers do not race on shared
 *      state.
 *
 * Validates: R12.5, R12.6, R12.7, R12.8
 */
AMIO_API const char *amio_strerror(int err);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* AMIO_AMIO_ERRORS_H */
