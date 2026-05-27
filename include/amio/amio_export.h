/*
 * amio_export.h -- AMIO public-symbol export macro (C99 surface).
 *
 * Provides the `AMIO_API` macro that decorates every `extern "C"`
 * AMIO_C_API function declaration in the public headers.  The macro
 * resolves to:
 *
 *   - on ELF platforms (Linux / macOS) under GCC / Clang:
 *         __attribute__((visibility("default")))
 *     so the function is exported even though the project compiles
 *     with `-fvisibility=hidden` (see CMakeLists.txt
 *     CMAKE_C_VISIBILITY_PRESET / CMAKE_CXX_VISIBILITY_PRESET).
 *
 *   - on Windows under MSVC:
 *         __declspec(dllexport)  when AMIO_BUILDING_LIBRARY is defined
 *         __declspec(dllimport)  for consumers
 *     so a future Windows port can import AMIO from a libamio.dll
 *     without modifying the public headers.  AMIO_BUILDING_LIBRARY
 *     is set by the build system (CMakeLists.txt
 *     `target_compile_definitions(amio_core PRIVATE
 *      AMIO_BUILDING_LIBRARY=1)`) and is never set by consumers.
 *
 *   - everywhere else (or when AMIO_STATIC_BUILD is set): empty.
 *
 * The macro is the only mechanism that determines which symbols
 * appear in the dynamic symbol table of `libamio.so`.  Combined with
 * the project-wide hidden-visibility default, this makes the
 * `nm -D libamio.so` symbol-mangling check (task 1.4) meaningful:
 * only `amio_*` C entry points are exported.
 *
 * Header-isolation contract (R10.1, R10.2, R13.6):
 *   - Includes nothing.
 *   - No C++ types, no third-party headers, no std:: symbols.
 *   - Compiles cleanly under
 *         gcc -std=c99 -pedantic -Werror -c amio_export.h
 *
 * Validates: R10.1, R10.2, R10.3, R10.5, R10.8, R13.6
 */

#ifndef AMIO_AMIO_EXPORT_H
#define AMIO_AMIO_EXPORT_H

/*
 * AMIO_API
 *
 * Decorates every `extern "C"` AMIO_C_API entry point.  Used in the
 * public headers as:
 *
 *     #ifdef __cplusplus
 *     extern "C" {
 *     #endif
 *
 *     AMIO_API const char *amio_strerror(int err);
 *
 *     #ifdef __cplusplus
 *     }
 *     #endif
 *
 * Consumers MUST NOT define AMIO_API themselves; the macro is fully
 * controlled by the AMIO build system and the platform detection
 * below.
 */
#if defined(AMIO_STATIC_BUILD)
#  define AMIO_API
#elif defined(_WIN32) || defined(__CYGWIN__)
#  if defined(AMIO_BUILDING_LIBRARY) || defined(AMIO_BUILDING_DLL)
#    if defined(__GNUC__)
#      define AMIO_API __attribute__((dllexport))
#    else
#      define AMIO_API __declspec(dllexport)
#    endif
#  else
#    if defined(__GNUC__)
#      define AMIO_API __attribute__((dllimport))
#    else
#      define AMIO_API __declspec(dllimport)
#    endif
#  endif
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#  define AMIO_API __attribute__((visibility("default")))
#elif defined(__clang__)
#  define AMIO_API __attribute__((visibility("default")))
#else
#  define AMIO_API
#endif

#endif  /* AMIO_AMIO_EXPORT_H */
