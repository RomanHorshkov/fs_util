#!/usr/bin/env bash
# =============================================================================
# build_sanitizer_tests.sh — unit + integration suites under ASan, UBSan, LSan.
#
# Uses the shared `sanitize` profile flags from the catalog. The library is
# compiled with the test seam so the fault-injection tests run sanitized too:
# an fd or allocation leaked on an error path is exactly what LSan is for.
#
# author  Roman Horshkov <github.com/RomanHorshkov>
# date    2026
# (c) 2026
# =============================================================================
set -euo pipefail

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build/sanitizers"
cd -- "${ROOT_DIR}"
source "${SCRIPT_DIR}/gcc_build_profiles.sh"

if ! pkg-config --exists cmocka; then
    printf 'cmocka not found (pkg-config --exists cmocka failed)\n' >&2
    exit 1
fi
read -r -a CMOCKA_CFLAGS <<< "$(pkg-config --cflags cmocka)"
read -r -a CMOCKA_LIBS <<< "$(pkg-config --libs cmocka)"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

SAN_CPPFLAGS=("${CPPFLAGS_SANITIZE[@]}" -D_GNU_SOURCE -DFS_UTIL_TESTING -Iapp -Itests/UTs)
SAN_CFLAGS=("${CFLAGS_SANITIZE[@]}")
SAN_LDFLAGS=("${LDFLAGS_SANITIZE[@]}")

gcc "${SAN_CPPFLAGS[@]}" "${SAN_CFLAGS[@]}" -c app/fsutil.c -o "${BUILD_DIR}/fsutil.o"

run_sanitized() {
    env ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" LSAN_OPTIONS="exitcode=1" "$@"
}

for src in tests/UTs/*.c tests/ITs/*.c; do
    name="$(basename "${src%.c}")"
    gcc "${SAN_CPPFLAGS[@]}" "${SAN_CFLAGS[@]}" "${CMOCKA_CFLAGS[@]}" -Itests/ITs -c "${src}" -o "${BUILD_DIR}/${name}.o"
    gcc "${BUILD_DIR}/${name}.o" "${BUILD_DIR}/fsutil.o" -o "${BUILD_DIR}/${name}_sanitized" "${SAN_LDFLAGS[@]}" "${CMOCKA_LIBS[@]}"
    printf '\n== %s (sanitized) ==\n' "${name}"
    run_sanitized "${BUILD_DIR}/${name}_sanitized"
done
