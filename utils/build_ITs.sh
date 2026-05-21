#!/usr/bin/env bash


# Build integration-test executables against whatever fsutil library profiles
# are already present under build/.
#
# This script does NOT run tests.
# It only produces the test executables in build/ITs/<profile>/.


# -e: stop immediately if a command fails.
# -u: stop if an unset variable is used.
# -o pipefail: fail a pipeline if any command in the pipeline fails.
set -euo pipefail

# Print a short, human-meaningful description of a produced test executable.
print_test_artifact_report() {
    local artifact_path="$1"

    printf '    artifact: %s\n' "${artifact_path}"
    printf '      size:   %s bytes\n' "$(wc -c < "${artifact_path}")"
    printf '      type:   %s\n' "$(file -b "${artifact_path}")"
}

# Return the canonical base profile name for a built library directory.
#
# Examples:
#   debug       -> debug
#   sanitize    -> sanitize
#   release_cov -> release
#
# That allows derived build directories such as release_cov to reuse the base
# profile's normal linker flags and then layer extras on top.
base_profile_name_for_built_profile() {
    local built_profile="$1"

    case "${built_profile}" in
        *_cov) printf '%s\n' "${built_profile%_cov}" ;;
        *)     printf '%s\n' "${built_profile}" ;;
    esac
}

# Build the full linker-flag array needed by the test executable for a given
# built library directory.
#
# For normal profiles this is just the profile's own LDFLAGS.
# For derived coverage builds such as release_cov, we add the shared coverage
# link instrumentation on top of the base profile's LDFLAGS.
populate_test_link_flags_for_built_profile() {
    local built_profile="$1"
    local output_array_name="$2"
    local base_profile
    local base_profile_upper
    local base_ldflags_var

    base_profile="$(base_profile_name_for_built_profile "${built_profile}")"
    base_profile_upper="${base_profile^^}"
    base_ldflags_var="LDFLAGS_${base_profile_upper}"

    if ! declare -p "${base_ldflags_var}" >/dev/null 2>&1; then
        printf 'unknown base profile for built directory: %s\n' "${built_profile}" >&2
        return 1
    fi

    declare -n base_ldflags_ref="${base_ldflags_var}"
    declare -n output_array_ref="${output_array_name}"

    output_array_ref=(
      "${base_ldflags_ref[@]}"
    )

    if [[ "${built_profile}" == *_cov ]]; then
        output_array_ref+=(
          "${LDFLAGS_INSTRUMENT_COVERAGE[@]}"
        )
    fi
}

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

# Load the canonical GCC build-profile definitions from the shared compilation
# directory so derived builds such as release_cov can reuse the same flag
# policy.
PROFILE_FILE="${ROOT_DIR}/../compilation/gcc_build_profiles.sh"
source "${PROFILE_FILE}"

LIB_BUILD_DIR="${ROOT_DIR}/build"
TEST_BUILD_DIR="${ROOT_DIR}/build/ITs"
mkdir -p "${TEST_BUILD_DIR}"

printf 'building integration-test executables in %s\n' "${TEST_BUILD_DIR}"

if ! pkg-config --exists cmocka; then
    printf 'cmocka not found (pkg-config --exists cmocka failed)\n' >&2
    exit 1
fi

read -r -a CMOCKA_CFLAGS <<< "$(pkg-config --cflags cmocka)"
read -r -a CMOCKA_LIBS <<< "$(pkg-config --libs cmocka)"

# Preserve the existing test-source compile policy exactly as it was in the old
# script.
#
# This means:
#   - strict warning flags still apply to the integration-test source file;
#   - --coverage still applies to the test executable build;
#   - the behavioral change here is architectural, not a silent flag-policy
#     rewrite.
WARN_FLAGS=(
  -Werror
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wformat=2
  -Wconversion
  -Wnull-dereference
  -Wdouble-promotion
  -Wduplicated-cond
  -Wduplicated-branches
  -Wlogical-op
)

CFLAGS=(
  -std=c11
  -D_GNU_SOURCE
  -O0
  -g
  --coverage
  -I.
  -Itests/ITs
  "${WARN_FLAGS[@]}"
)

# Discover built library profiles from the filesystem instead of trusting only a
# hardcoded list. This is what allows derived builds such as release_cov to be
# picked up automatically.
mapfile -t built_library_dirs < <(
    find "${LIB_BUILD_DIR}" -mindepth 1 -maxdepth 1 -type d \
        ! -name 'ITs' \
        | sort
)

built_profile_count=0

for profile_dir in "${built_library_dirs[@]}"; do
    built_profile="${profile_dir##*/}"
    shared_lib_path="${profile_dir}/libfsutil.so"
    static_lib_path="${profile_dir}/libfsutil.a"

    if [[ ! -f "${shared_lib_path}" && ! -f "${static_lib_path}" ]]; then
        continue
    fi

    built_profile_count=$((built_profile_count + 1))

    profile_test_dir="${TEST_BUILD_DIR}/${built_profile}"
    mkdir -p "${profile_test_dir}"
    find "${profile_test_dir}" -mindepth 1 -maxdepth 1 -type f -delete

    populate_test_link_flags_for_built_profile "${built_profile}" test_link_flags

    printf '\n[%s]\n' "${built_profile}"
    printf '  compiling test object:   %s\n' "${profile_test_dir}/integration_test.o"

    gcc "${CFLAGS[@]}" "${CMOCKA_CFLAGS[@]}" \
        -c tests/ITs/integration_test.c \
        -o "${profile_test_dir}/integration_test.o"

    if [[ -f "${shared_lib_path}" ]]; then
        printf '  linking shared test:     %s\n' "${profile_test_dir}/integration_test.shared"
        gcc --coverage -O0 -g "${test_link_flags[@]}" \
            "${profile_test_dir}/integration_test.o" \
            -o "${profile_test_dir}/integration_test.shared" \
            -L"${profile_dir}" \
            -Wl,-rpath,"${profile_dir}" \
            -lfsutil \
            "${CMOCKA_LIBS[@]}"
    fi

    if [[ -f "${static_lib_path}" ]]; then
        printf '  linking static test:     %s\n' "${profile_test_dir}/integration_test.static"
        gcc --coverage -O0 -g "${test_link_flags[@]}" \
            "${profile_test_dir}/integration_test.o" \
            "${static_lib_path}" \
            -o "${profile_test_dir}/integration_test.static" \
            "${CMOCKA_LIBS[@]}"
    fi

    printf '  output summary:\n'
    if [[ -f "${profile_test_dir}/integration_test.shared" ]]; then
        print_test_artifact_report "${profile_test_dir}/integration_test.shared"
    fi
    if [[ -f "${profile_test_dir}/integration_test.static" ]]; then
        print_test_artifact_report "${profile_test_dir}/integration_test.static"
    fi
done

if (( built_profile_count == 0 )); then
    printf 'no built libraries were found under %s\n' "${LIB_BUILD_DIR}" >&2
    printf 'run ./utils/build_libs.sh first\n' >&2
    exit 1
fi

printf '\nartifacts:\n'
find "${TEST_BUILD_DIR}" -mindepth 2 -maxdepth 2 -type f -executable | sort
