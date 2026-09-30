#!/usr/bin/env bash
# Verify that every shared library required by AMIO resolves on the host.
set -euo pipefail

LIBAMIO="${1:-}"
if [[ -z "$LIBAMIO" || ! -f "$LIBAMIO" ]]; then
    echo "SKIP: AMIO shared library not found (pass path as argument)"
    exit 0
fi

if [[ "${OSTYPE:-}" == darwin* ]]; then
    echo "SKIP: runtime dependency inspection is unavailable on macOS"
    exit 0
fi

if ! command -v ldd >/dev/null 2>&1; then
    echo "SKIP: ldd is unavailable on this host"
    exit 0
fi

if ! dependencies="$(ldd "$LIBAMIO" 2>&1)"; then
    printf '%s\n' "$dependencies"
    echo "FAIL: unable to inspect AMIO shared library dependencies"
    exit 1
fi

if grep -Fq 'not found' <<<"$dependencies"; then
    printf '%s\n' "$dependencies"
    echo "FAIL: AMIO has unresolved shared library dependencies"
    exit 1
fi

echo "PASS: all AMIO shared library dependencies resolve"