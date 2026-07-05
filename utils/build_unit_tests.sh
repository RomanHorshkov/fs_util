#!/usr/bin/env bash

# Build fsutil unit-test executables against already-built library profiles.

set -euo pipefail

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROFILE_FILE="${SCRIPT_DIR}/gcc_build_profiles.sh"
LIB_BUILD_DIR="${ROOT_DIR}/build"
TEST_BUILD_DIR="${ROOT_DIR}/build/unit"
CC_BIN="${CC:-gcc}"

cd -- "${ROOT_DIR}"

if [[ ! -f "${PROFILE_FILE}" ]]; then
    printf 'gcc profile file not found: %s\n' "${PROFILE_FILE}" >&2
    exit 1
fi
source "${PROFILE_FILE}"

if ! pkg-config --exists cmocka; then
    printf 'cmocka not found (pkg-config --exists cmocka failed)\n' >&2
    exit 1
fi
read -r -a CMOCKA_CFLAGS <<< "$(pkg-config --cflags cmocka)"
read -r -a CMOCKA_LIBS <<< "$(pkg-config --libs cmocka)"

base_profile_name_for_built_profile() {
    local built_profile="$1"
    case "${built_profile}" in
        *_cov) printf '%s\n' "${built_profile%_cov}" ;;
        *)     printf '%s\n' "${built_profile}" ;;
    esac
}

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
    output_array_ref=("${base_ldflags_ref[@]}")

    if [[ "${built_profile}" == *_cov ]]; then
        output_array_ref+=("${LDFLAGS_INSTRUMENT_COVERAGE[@]}")
    fi
}

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
  -Itests/unit
  "${WARN_FLAGS[@]}"
)

mkdir -p "${TEST_BUILD_DIR}"
mapfile -t UNIT_SOURCES < <(find tests/unit -type f -name '*.c' | sort)
if ((${#UNIT_SOURCES[@]} == 0)); then
    printf 'no unit test sources found under tests/unit\n' >&2
    exit 1
fi

mapfile -t BUILT_LIBRARY_DIRS < <(find "${LIB_BUILD_DIR}" -mindepth 1 -maxdepth 1 -type d ! -name ITs ! -name unit ! -name deb ! -name debs | sort)
BUILT_PROFILE_COUNT=0

for profile_dir in "${BUILT_LIBRARY_DIRS[@]}"; do
    built_profile="${profile_dir##*/}"
    shared_lib_path="${profile_dir}/libfsutil.so"
    static_lib_path="${profile_dir}/libfsutil.a"

    if [[ ! -f "${shared_lib_path}" && ! -f "${static_lib_path}" ]]; then
        continue
    fi

    BUILT_PROFILE_COUNT=$((BUILT_PROFILE_COUNT + 1))
    profile_test_dir="${TEST_BUILD_DIR}/${built_profile}"
    mkdir -p "${profile_test_dir}"
    find "${profile_test_dir}" -mindepth 1 -maxdepth 1 -type f -delete
    populate_test_link_flags_for_built_profile "${built_profile}" test_link_flags

    printf '\n[%s unit]\n' "${built_profile}"
    for source_path in "${UNIT_SOURCES[@]}"; do
        test_name="$(basename "${source_path}" .c)"
        object_path="${profile_test_dir}/${test_name}.o"

        printf '  compiling: %s\n' "${object_path}"
        "${CC_BIN}" "${CFLAGS[@]}" "${CMOCKA_CFLAGS[@]}" -c "${source_path}" -o "${object_path}"

        if [[ -f "${shared_lib_path}" ]]; then
            printf '  linking shared: %s\n' "${profile_test_dir}/${test_name}.shared"
            "${CC_BIN}" --coverage -O0 -g "${test_link_flags[@]}" "${object_path}" -o "${profile_test_dir}/${test_name}.shared" \
                -L"${profile_dir}" -Wl,-rpath,"${profile_dir}" -lfsutil "${CMOCKA_LIBS[@]}"
        fi

        if [[ -f "${static_lib_path}" ]]; then
            printf '  linking static: %s\n' "${profile_test_dir}/${test_name}.static"
            "${CC_BIN}" --coverage -O0 -g "${test_link_flags[@]}" "${object_path}" "${static_lib_path}" \
                -o "${profile_test_dir}/${test_name}.static" "${CMOCKA_LIBS[@]}"
        fi
    done
done

if ((BUILT_PROFILE_COUNT == 0)); then
    printf 'no built libraries were found under %s\n' "${LIB_BUILD_DIR}" >&2
    printf 'run ./utils/build_libs.sh first\n' >&2
    exit 1
fi

printf '\nartifacts:\n'
find "${TEST_BUILD_DIR}" -mindepth 2 -maxdepth 2 -type f -executable | sort
