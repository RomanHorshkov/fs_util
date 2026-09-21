#!/usr/bin/env bash


# Orchestrate the repository's existing fsutil integration pipeline stages.
#
# Local default:
#   ./utils/run_pipeline.sh
#
# Stage-oriented usage for CI:
#   ./utils/run_pipeline.sh build
#   ./utils/run_pipeline.sh build_ITs
#   ./utils/run_pipeline.sh run_ITs


set -euo pipefail

usage() {
    cat <<'EOF'
usage: ./utils/run_pipeline.sh [all|build|unit_release|unit_cov|build_ITs|run_ITs|sanitizers|package]

Default command:
  all

Environment:
  FSUTIL_RESULTS_RUN_ID
      Optional explicit run id. Use the same value across separate stage
      invocations if you want all logs and reports to land in one archived run.
EOF
}

normalize_command() {
    local raw_command="${1:-all}"

    case "${raw_command}" in
        all) printf 'all\n' ;;
        build) printf 'build\n' ;;
        unit_release|unit-release) printf 'unit_release\n' ;;
        unit_cov|unit-cov|unit) printf 'unit_cov\n' ;;
        build_ITs|build-its|build_its) printf 'build_ITs\n' ;;
        run_ITs|run-its|run_its) printf 'run_ITs\n' ;;
        sanitizers|sanitizer) printf 'sanitizers\n' ;;
        package|deb) printf 'package\n' ;;
        help|-h|--help) printf 'help\n' ;;
        *)
            printf 'unknown command: %s\n' "${raw_command}" >&2
            usage >&2
            exit 1
            ;;
    esac
}

run_stage_with_log() {
    local stage_name="$1"
    local log_file="$2"
    shift 2

    mkdir -p "${RUN_RESULT_DIR}"

    printf '[pipeline] stage:  %s\n' "${stage_name}"
    printf '[pipeline] run id: %s\n' "${RUN_ID}"
    printf '[pipeline] log:    %s\n' "${log_file}"

    "$@" 2>&1 | tee "${log_file}"
}

run_build_stage() {
    run_stage_with_log build "${RUN_RESULT_DIR}/build_libs.log" \
        "${SCRIPT_DIR}/build_libs.sh"
}

run_unit_release_stage() {
    run_stage_with_log unit_release "${RUN_RESULT_DIR}/unit_release.log" \
        "${SCRIPT_DIR}/build_UTs_release.sh"
}

run_unit_cov_stage() {
    run_stage_with_log unit_cov "${RUN_RESULT_DIR}/unit_cov.log" \
        "${SCRIPT_DIR}/build_UTs.sh"
}

run_sanitizers_stage() {
    run_stage_with_log sanitizers "${RUN_RESULT_DIR}/sanitizers.log" \
        "${SCRIPT_DIR}/build_sanitizer_tests.sh"
}

run_package_stage() {
    run_stage_with_log package "${RUN_RESULT_DIR}/package.log" \
        "${SCRIPT_DIR}/build_deb.sh"
}

run_build_its_stage() {
    run_stage_with_log build_ITs "${RUN_RESULT_DIR}/build_ITs.log" \
        "${SCRIPT_DIR}/build_ITs.sh"
}

run_run_its_stage() {
    mkdir -p "${RUN_RESULT_DIR}"

    printf '[pipeline] stage:  run_ITs\n'
    printf '[pipeline] run id: %s\n' "${RUN_ID}"

    "${SCRIPT_DIR}/run_ITs.sh"
}


START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd -- "${ROOT_DIR}"

COMMAND="$(normalize_command "${1:-all}")"

if [[ "${COMMAND}" == 'help' ]]; then
    usage
    exit 0
fi

RUN_ID="${FSUTIL_RESULTS_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
RESULT_ROOT_DIR="${ROOT_DIR}/tests/results/ITs"
RUN_RESULT_DIR="${RESULT_ROOT_DIR}/runs/${RUN_ID}"
export FSUTIL_RESULTS_RUN_ID="${RUN_ID}"

case "${COMMAND}" in
    all)
        run_build_stage
        run_unit_release_stage
        run_unit_cov_stage
        run_build_its_stage
        run_run_its_stage
        run_sanitizers_stage
        run_package_stage
        ;;
    build)
        run_build_stage
        ;;
    unit_release)
        run_unit_release_stage
        ;;
    unit_cov)
        run_unit_cov_stage
        ;;
    sanitizers)
        run_sanitizers_stage
        ;;
    package)
        run_package_stage
        ;;
    build_ITs)
        run_build_its_stage
        ;;
    run_ITs)
        run_run_its_stage
        ;;
esac
