#!/usr/bin/env bash

# Run discovered fsutil unit-test executables.

set -euo pipefail

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd -- "${ROOT_DIR}"

RUN_ID="${FSUTIL_UNIT_RESULTS_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
RESULT_ROOT="${ROOT_DIR}/tests/results/unit/runs/${RUN_ID}"
SUMMARY_FILE="${RESULT_ROOT}/summary.txt"
TEST_BUILD_DIR="${ROOT_DIR}/build/unit"

if [[ ! -d "${TEST_BUILD_DIR}" ]]; then
    printf 'unit test build directory not found: %s\n' "${TEST_BUILD_DIR}" >&2
    printf 'run ./utils/build_unit_tests.sh first\n' >&2
    exit 1
fi

mkdir -p "${RESULT_ROOT}"
printf 'profile\ttest\tvariant\trc\tlog\n' > "${SUMMARY_FILE}"

overall_rc=0
while IFS= read -r -d '' exe; do
    rel="${exe#${TEST_BUILD_DIR}/}"
    profile="${rel%%/*}"
    file="${rel#*/}"
    test_name="${file%.*}"
    variant="${file##*.}"
    log_file="${RESULT_ROOT}/${profile}/${file}.log"
    mkdir -p "$(dirname "${log_file}")"

    if LD_LIBRARY_PATH="${ROOT_DIR}/build/${profile}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" "${exe}" >"${log_file}" 2>&1; then
        rc=0
        printf '  %-12s %-32s %-8s OK\n' "${profile}" "${test_name}" "${variant}"
    else
        rc=$?
        overall_rc=1
        printf '  %-12s %-32s %-8s FAIL rc=%d\n' "${profile}" "${test_name}" "${variant}" "${rc}"
    fi

    printf '%s\t%s\t%s\t%s\t%s\n' "${profile}" "${test_name}" "${variant}" "${rc}" "${log_file}" >> "${SUMMARY_FILE}"
done < <(find "${TEST_BUILD_DIR}" -mindepth 2 -maxdepth 2 -type f -executable \
    \( -name '*.shared' -o -name '*.static' \) -print0 | sort -z)

printf '\nresults:\n  %s\n' "${RESULT_ROOT}"
exit "${overall_rc}"
