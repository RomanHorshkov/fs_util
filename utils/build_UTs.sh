#!/usr/bin/env bash
# =============================================================================
# build_UTs.sh — build and run the unit tests with coverage, then gate on it.
#
# The library is compiled ONCE, instrumented, with -DFS_UTIL_TESTING so the
# syscall seam (tests/UTs/fsutil_test_hooks.h) exists; every tests/UTs/*.c
# becomes its own executable linked against that one object, so gcov counts
# accumulate across all of them. Coverage is measured on app/fsutil.c only.
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
BUILD_DIR="${ROOT_DIR}/build/UTs"
RESULT_DIR="${ROOT_DIR}/tests/results/UTs"
cd -- "${ROOT_DIR}"

if ! pkg-config --exists cmocka; then
    printf 'cmocka not found (pkg-config --exists cmocka failed)\n' >&2
    exit 1
fi
if ! command -v gcovr >/dev/null 2>&1; then
    printf 'gcovr not found in PATH\n' >&2
    exit 1
fi
read -r -a CMOCKA_CFLAGS <<< "$(pkg-config --cflags cmocka)"
read -r -a CMOCKA_LIBS <<< "$(pkg-config --libs cmocka)"

rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}" "${RESULT_DIR}"

# Atomic profile updates keep gcov counters valid even if a test forks or threads.
COVERAGE_FLAGS=(--coverage -fprofile-update=atomic)
COMMON_FLAGS=(-std=c11 -O0 -g -D_GNU_SOURCE -DFS_UTIL_TESTING -Wall -Wextra -Wpedantic -Wshadow -Werror -Iapp -Itests/UTs)

gcc "${COMMON_FLAGS[@]}" "${COVERAGE_FLAGS[@]}" -c app/fsutil.c -o "${BUILD_DIR}/fsutil.o"

declare -a EXES=()
for src in tests/UTs/*.c; do
    name="$(basename "${src%.c}")"
    gcc "${COMMON_FLAGS[@]}" "${COVERAGE_FLAGS[@]}" "${CMOCKA_CFLAGS[@]}" -c "${src}" -o "${BUILD_DIR}/${name}.o"
    gcc "${COVERAGE_FLAGS[@]}" "${BUILD_DIR}/${name}.o" "${BUILD_DIR}/fsutil.o" -o "${BUILD_DIR}/${name}" "${CMOCKA_LIBS[@]}"
    EXES+=("${BUILD_DIR}/${name}")
done

for exe in "${EXES[@]}"; do
    printf '\n== %s ==\n' "$(basename "${exe}")"
    "${exe}"
done

gcovr -r "${ROOT_DIR}" --object-directory "${BUILD_DIR}" "${BUILD_DIR}" --filter 'app/fsutil\.c' \
    --html --html-details -o "${RESULT_DIR}/UTs_coverage.html"
gcovr -r "${ROOT_DIR}" --object-directory "${BUILD_DIR}" "${BUILD_DIR}" --filter 'app/fsutil\.c' \
    --xml -o "${RESULT_DIR}/UTs_coverage.xml"
gcovr -r "${ROOT_DIR}" --object-directory "${BUILD_DIR}" "${BUILD_DIR}" --filter 'app/fsutil\.c' \
    --txt -o "${RESULT_DIR}/UTs_coverage.txt"
gcovr -r "${ROOT_DIR}" --object-directory "${BUILD_DIR}" "${BUILD_DIR}" --filter 'app/fsutil\.c' \
    --json-summary --fail-under-line 100 --fail-under-branch 100 \
    -o "${RESULT_DIR}/coverage-summary.json"

printf '\n100%% line and branch coverage gate passed; report ready: %s\n' "${RESULT_DIR}/UTs_coverage.html"
