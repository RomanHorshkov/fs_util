#!/usr/bin/env bash
# =============================================================================
# build_UTs_release.sh — run the contract unit tests against the RELEASE archive.
#
# No seam, no instrumentation: tests/UTs/*.c compiled at -O2 and linked against
# build/release/libfsutil.a, i.e. the exact object code that gets packaged. The
# fault-injection tests are compiled out (they need FS_UTIL_TESTING), so this
# run proves the shipped library honours every documented contract.
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
BUILD_DIR="${ROOT_DIR}/build/UTs_release"
cd -- "${ROOT_DIR}"

if ! pkg-config --exists cmocka; then
    printf 'cmocka not found (pkg-config --exists cmocka failed)\n' >&2
    exit 1
fi
read -r -a CMOCKA_CFLAGS <<< "$(pkg-config --cflags cmocka)"
read -r -a CMOCKA_LIBS <<< "$(pkg-config --libs cmocka)"

"${SCRIPT_DIR}/build_libs.sh" release
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

UT_CFLAGS=(-std=c11 -O2 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror -Iapp -Itests/UTs)
for src in tests/UTs/*.c; do
    name="$(basename "${src%.c}")"
    gcc "${UT_CFLAGS[@]}" "${CMOCKA_CFLAGS[@]}" -c "${src}" -o "${BUILD_DIR}/${name}.o"
    gcc "${BUILD_DIR}/${name}.o" build/release/libfsutil.a -o "${BUILD_DIR}/${name}" "${CMOCKA_LIBS[@]}"
    printf '\n== %s (release archive) ==\n' "${name}"
    "${BUILD_DIR}/${name}"
done
