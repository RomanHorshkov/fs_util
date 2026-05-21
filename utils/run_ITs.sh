#!/usr/bin/env bash


# Discover and run whatever integration-test executables have already been
# built under build/ITs/.
#
# This script does NOT build anything. Its only job is:
#   - discover existing test executables;
#   - run them;
#   - store their logs;
#   - produce a summary;
#   - generate coverage only if coverage data actually exists.


# -e: stop immediately if a command fails.
# -u: stop if an unset variable is used.
# -o pipefail: fail a pipeline if any command in the pipeline fails.
set -euo pipefail

# Remember where the user launched the script.
START_DIR="$(pwd -P)"
# Return to the user's launch directory.
cleanup() { cd -- "$START_DIR"; }
# Always return to START_DIR on script exit.
trap cleanup EXIT

# Determine the directory that contains this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Determine the root directory of the project (the parent of the script's
# directory).
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd -- "${ROOT_DIR}"

TEST_BUILD_DIR="${ROOT_DIR}/build/ITs"
RESULT_DIR="${ROOT_DIR}/tests/results/ITs"
SUMMARY_FILE="${RESULT_DIR}/integration_result.txt"

mkdir -p "${RESULT_DIR}"
: > "${SUMMARY_FILE}"

printf 'running integration tests discovered under %s\n' "${TEST_BUILD_DIR}"

if [[ ! -d "${TEST_BUILD_DIR}" ]]; then
    printf 'test build directory not found: %s\n' "${TEST_BUILD_DIR}" >&2
    printf 'run ./utils/build_ITs.sh first\n' >&2
    exit 1
fi

# Discover executable test artifacts only.
#
# The build step leaves objects and executables in the same profile directories,
# so we filter explicitly for the executable test names.
mapfile -t test_executables < <(
    find "${TEST_BUILD_DIR}" -mindepth 2 -maxdepth 2 -type f -executable \
        \( -name 'integration_test.shared' -o -name 'integration_test.static' \) \
        | sort
)

if ((${#test_executables[@]} == 0)); then
    printf 'no built integration-test executables were found in %s\n' "${TEST_BUILD_DIR}" >&2
    printf 'run ./utils/build_ITs.sh first\n' >&2
    exit 1
fi

overall_rc=0

for test_executable in "${test_executables[@]}"; do
    relative_path="${test_executable#${TEST_BUILD_DIR}/}"
    profile="${relative_path%%/*}"
    executable_name="${test_executable##*/}"
    variant="${executable_name#integration_test.}"

    profile_result_dir="${RESULT_DIR}/${profile}"
    log_file="${profile_result_dir}/${executable_name}.txt"
    mkdir -p "${profile_result_dir}"

    printf '\n[%s / %s]\n' "${profile}" "${variant}"
    printf '  running:  %s\n' "${test_executable}"
    printf '  log file: %s\n' "${log_file}"

    run_rc=0
    {
        printf '[%s / %s]\n' "${profile}" "${variant}"
        "${test_executable}"
    } 2>&1 | tee "${log_file}" || run_rc=$?

    {
        printf '[%s / %s]\n' "${profile}" "${variant}"
        printf 'log: %s\n' "${log_file}"
        printf 'exit code: %d\n' "${run_rc}"
        printf '\n'
        cat "${log_file}"
        printf '\n\n'
    } >> "${SUMMARY_FILE}"

    if ((run_rc == 0)); then
        printf '  result: PASS\n'
    else
        printf '  result: FAIL (exit code %d)\n' "${run_rc}"
        overall_rc="${run_rc}"
    fi
done

# Coverage is optional.
#
# The old all-in-one script always rebuilt fsutil.c with --coverage, so reports
# were guaranteed. The new workflow instead links tests against prebuilt
# libraries. Coverage reports therefore exist only when a coverage-instrumented
# library variant such as release_cov was built and exercised.
coverage_artifact_count="$(
    find "${ROOT_DIR}/build" -type f \( -name '*.gcno' -o -name '*.gcda' \) \
        ! -path "${ROOT_DIR}/build/ITs/*" \
        | wc -l
)"

if (( coverage_artifact_count > 0 )); then
    if ! command -v gcovr >/dev/null 2>&1; then
        printf '\n[coverage] coverage artifacts were found, but gcovr is not installed\n' >&2
    else
        printf '\n[coverage] generating reports via gcovr...\n'

        gcovr -r "${ROOT_DIR}" \
            --object-directory "${ROOT_DIR}/build" \
            --exclude 'tests/' \
            --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
            --html --html-details \
            -o "${RESULT_DIR}/ITs_all_coverage.html"

        gcovr -r "${ROOT_DIR}" \
            --object-directory "${ROOT_DIR}/build" \
            --exclude 'tests/' \
            --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
            --xml \
            -o "${RESULT_DIR}/ITs_all_coverage.xml"

        gcovr -r "${ROOT_DIR}" \
            --object-directory "${ROOT_DIR}/build" \
            --exclude 'tests/' \
            --gcov-ignore-parse-errors negative_hits.warn_once_per_file \
            --json-summary \
            -o "${RESULT_DIR}/coverage-summary.json"

        printf '[coverage] report ready: %s\n' "${RESULT_DIR}/ITs_all_coverage.html"
    fi
else
    printf '\n[coverage] skipped: no coverage-instrumented build artifacts were found under %s\n' "${ROOT_DIR}/build"
    printf '[coverage] build a coverage-enabled library variant first if you want reports\n'
fi

printf '\nsummary file: %s\n' "${SUMMARY_FILE}"

exit "${overall_rc}"
