#!/usr/bin/env bash

# Run release_cov fsutil unit tests and emit gcovr coverage reports.

set -euo pipefail

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd -- "${ROOT_DIR}"

if ! command -v gcovr >/dev/null 2>&1; then
    printf 'gcovr not found in PATH\n' >&2
    exit 1
fi

PROFILE="release_cov"
RUN_ID="${1:-$(date -u +%Y%m%dT%H%M%SZ)}"
TEST_BUILD_DIR="${ROOT_DIR}/build/unit/${PROFILE}"
RESULT_ROOT="${ROOT_DIR}/tests/results/unit/coverage/${RUN_ID}"
SUMMARY_FILE="${RESULT_ROOT}/summary.txt"

if [[ ! -d "${TEST_BUILD_DIR}" ]]; then
    printf 'release_cov unit tests not found: %s\n' "${TEST_BUILD_DIR}" >&2
    printf 'run ./utils/build_libs.sh release_cov && ./utils/build_unit_tests.sh first\n' >&2
    exit 1
fi

find "${ROOT_DIR}/build/${PROFILE}" "${TEST_BUILD_DIR}" -type f \( -name '*.gcda' -o -name '*.gcov' \) -delete
mkdir -p "${RESULT_ROOT}/logs"
printf 'test\tvariant\trc\tlog\n' > "${SUMMARY_FILE}"

overall_rc=0
while IFS= read -r -d '' exe; do
    file="$(basename "${exe}")"
    test_name="${file%.*}"
    variant="${file##*.}"
    log_file="${RESULT_ROOT}/logs/${file}.log"

    if LD_LIBRARY_PATH="${ROOT_DIR}/build/${PROFILE}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" "${exe}" >"${log_file}" 2>&1; then
        rc=0
        printf '  %-32s %-8s OK\n' "${test_name}" "${variant}"
    else
        rc=$?
        overall_rc=1
        printf '  %-32s %-8s FAIL rc=%d\n' "${test_name}" "${variant}" "${rc}"
    fi

    printf '%s\t%s\t%s\t%s\n' "${test_name}" "${variant}" "${rc}" "${log_file}" >> "${SUMMARY_FILE}"
done < <(find "${TEST_BUILD_DIR}" -maxdepth 1 -type f -executable \( -name '*.shared' -o -name '*.static' \) -print0 | sort -z)

coverage_txt="${RESULT_ROOT}/coverage.txt"
coverage_html="${RESULT_ROOT}/coverage.html"
coverage_xml="${RESULT_ROOT}/coverage.xml"

gcovr \
    --root "${ROOT_DIR}" \
    --filter "${ROOT_DIR}/fsutil.c" \
    --object-directory "${ROOT_DIR}/build/${PROFILE}" \
    "${ROOT_DIR}/build/${PROFILE}" \
    --txt > "${coverage_txt}"

gcovr \
    --root "${ROOT_DIR}" \
    --filter "${ROOT_DIR}/fsutil.c" \
    --object-directory "${ROOT_DIR}/build/${PROFILE}" \
    "${ROOT_DIR}/build/${PROFILE}" \
    --html-details "${coverage_html}" >/dev/null

gcovr \
    --root "${ROOT_DIR}" \
    --filter "${ROOT_DIR}/fsutil.c" \
    --object-directory "${ROOT_DIR}/build/${PROFILE}" \
    "${ROOT_DIR}/build/${PROFILE}" \
    --xml "${coverage_xml}" >/dev/null

find "${ROOT_DIR}/build/${PROFILE}" "${TEST_BUILD_DIR}" -type f \( -name '*.gcda' -o -name '*.gcov' \) -delete

printf '\ncoverage:\n  %s\n' "${coverage_txt}"
printf 'results:\n  %s\n' "${RESULT_ROOT}"
exit "${overall_rc}"
