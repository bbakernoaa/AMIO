/*
 * amio_mdspan_fwd.h -- Forward declarations of kokkos/mdspan shape
 *                      descriptors as opaque C structs.
 *
 * Internally, AMIO_Core uses `kokkos/mdspan` (a C++ class template)
 * to express N-dimensional non-owning views over `Staging_Pool`
 * buffers and host pointers.  The public C99 surface MUST NOT see
 * any of those C++ types (R10.1, R10.2, R13.6), so this header
 * declares opaque struct tags that the C-Boundary translation unit
 * (`src/c_boundary/`) is free to cast to / from concrete C++
 * mdspan instantiations behind the scenes.
 *
 * The descriptors below are the only mdspan-related symbols that
 * cross the FFI boundary -- they appear in the public ABI as
 * pointer-to-incomplete-struct types only, so the host compiler
 * never needs to see kokkos/mdspan headers, and the size and layout
 * of the underlying C++ class are not part of the AMIO ABI.
 *
 * Header-isolation contract (R10.1, R10.2, R13.6):
 *   - Includes only <stdint.h>.
 *   - All forward declarations are opaque (incomplete struct types
 *     used by pointer only).
 *   - No C++ template, namespace, class, or std:: symbol appears
 *     anywhere in this file.
 *   - All declarations wrapped in `extern "C"` for C++ inclusion
 *     (R10.3).
 *
 * Validates: R10.1, R10.2, R10.3, R10.5, R13.6
 */

#ifndef AMIO_AMIO_MDSPAN_FWD_H
#define AMIO_AMIO_MDSPAN_FWD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque forward declarations of the kokkos/mdspan shape descriptors
 * that AMIO_Core uses internally.  Host code interacts with these
 * tags exclusively through pointer-typed parameters; the underlying
 * struct definition is provided privately inside the AMIO_Core
 * build (src/c_boundary/) and is never installed.
 *
 * These tags are deliberately distinct types so that the C compiler
 * enforces type safety across the FFI boundary even though all four
 * are pointer-to-incomplete-struct at the ABI level.
 */
struct amio_mdspan_extents_descriptor;
struct amio_mdspan_layout_descriptor;
struct amio_mdspan_accessor_descriptor;
struct amio_mdspan_view_descriptor;

/*
 * Pointer typedefs for the opaque mdspan shape descriptors above.
 * These are the only mdspan-related types that may appear in
 * AMIO_C_API function signatures.
 */
typedef struct amio_mdspan_extents_descriptor  *amio_mdspan_extents_t;
typedef struct amio_mdspan_layout_descriptor   *amio_mdspan_layout_t;
typedef struct amio_mdspan_accessor_descriptor *amio_mdspan_accessor_t;
typedef struct amio_mdspan_view_descriptor     *amio_mdspan_view_t;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* AMIO_AMIO_MDSPAN_FWD_H */
