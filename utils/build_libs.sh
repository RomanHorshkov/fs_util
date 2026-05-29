#!/usr/bin/env bash
# =============================================================================
# fsutil library builder
# =============================================================================
#
# This script builds the fsutil library artifacts from fsutil.c.
#
# It deliberately does one job only:
#   - compile fsutil.c with the selected GCC profile flags;
#   - produce a static archive:  build/<profile>/libfsutil.a;
#   - produce a shared library:  build/<profile>/libfsutil.so;
#   - optionally produce a coverage-instrumented release variant:
#       build/release_cov/libfsutil.a
#       build/release_cov/libfsutil.so
#
# It does NOT build tests.
# It does NOT run tests.
# It does NOT call the other utils/*.sh scripts.
#
# The source of truth for build policy is:
#   utils/gcc_build_profiles.sh
#
# That profile file owns the actual flag arrays:
#   CPPFLAGS_RELEASE, CFLAGS_RELEASE, LDFLAGS_RELEASE
#   CPPFLAGS_DEBUG,   CFLAGS_DEBUG,   LDFLAGS_DEBUG
#   CFLAGS_SHARED,    LDFLAGS_SHARED
#   CFLAGS_INSTRUMENT_COVERAGE, LDFLAGS_INSTRUMENT_COVERAGE
# and so on.
#
# This file is the builder. The profile file is the policy.
# =============================================================================

# -e: stop when a command fails.
# -u: stop when an unset variable is used.
# -o pipefail: if any command in a pipeline fails, the whole pipeline fails.
set -euo pipefail

# Tools can be overridden by the caller if needed:
#   CC=clang ./utils/build_libs.sh release
#   AR=gcc-ar RANLIB=gcc-ranlib ./utils/build_libs.sh native
#
# For normal GCC builds, these defaults are fine.
CC="${CC:-gcc}"
AR="${AR:-ar}"
RANLIB="${RANLIB:-ranlib}"

# Project-local static archives to merge into libfsutil.a.
#
# A .a file is not a fully linked executable. It is an archive of object files.
# That means it can contain:
#   - fsutil.o from this project;
#   - object files extracted from other project-local .a archives.
#
# It should NOT try to stuff libc, libasan, libtsan, or random system shared
# libraries into itself. Those runtime libraries are resolved later by the final
# executable or shared-library link step.
#
# fsutil currently has no project-local static library dependencies, so this is
# empty. If you later add a local archive that must be embedded inside every
# libfsutil.a, add its path here, for example:
#   STATIC_ARCHIVE_DEPS=("third_party/something/libsomething.a")
STATIC_ARCHIVE_DEPS=()

# Libraries or linker arguments needed only when linking libfsutil.so.
#
# fsutil currently does not need extra libraries. If that changes, add the link
# arguments here, for example:
#   SHARED_LINK_LIBS=(-lm)
SHARED_LINK_LIBS=()

# Keep track of what this script actually produced so the final artifact list is
# truthful instead of merely listing what we hoped would exist.
BUILT_ARTIFACTS=()

# Print an error message and exit. This is easier to read than repeating
# "printf ... >&2; exit 1" everywhere.
die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

# Print a command dependency error if an external tool is not available.
require_tool() {
    local tool="$1"

    if ! command -v "${tool}" >/dev/null 2>&1; then
        die "required tool not found: ${tool}"
    fi
}

# Bash arrays are how gcc_build_profiles.sh stores flags. This helper checks
# that an array name exists before we try to use it. If a profile name is wrong,
# this gives a clear error instead of a weird nameref failure later.
require_array() {
    local array_name="$1"

    if ! declare -p "${array_name}" >/dev/null 2>&1; then
        die "required flag array is not defined: ${array_name}"
    fi
}

# Print a short report about one generated file.
#
# wc tells us the byte size.
# file tells us what kind of binary/archive it is when the file utility exists.
print_artifact_report() {
    local artifact_path="$1"

    printf '    artifact: %s\n' "${artifact_path}"
    printf '      size:   %s bytes\n' "$(wc -c < "${artifact_path}")"

    if command -v file >/dev/null 2>&1; then
        printf '      type:   %s\n' "$(file -b "${artifact_path}")"
    else
        printf '      type:   file(1) not available\n'
    fi
}

# Remove every flag from input_flags that also appears in blocked_flags.
#
# Why this exists:
#   The newer gcc_build_profiles.sh can contain executable-only flags inside a
#   profile, for example:
#     CFLAGS_EXE_HARDENING=(-fPIE)
#     LDFLAGS_EXE_HARDENING=(-pie -Wl,-z,relro -Wl,-z,now)
#
#   Those are correct for executable builds, but this script builds libraries.
#   Shared libraries need:
#     CFLAGS_SHARED=(-fPIC)
#     LDFLAGS_SHARED=(-shared)
#
#   So we keep the useful profile flags, remove executable-only policy, then add
#   the library-specific shared flags where appropriate.
#
# Bash note:
#   local -n creates a nameref. It lets this function receive array variable
#   names and write the output array chosen by the caller.
filter_flags() {
    local -n input_flags="$1"
    local -n blocked_flags="$2"
    local -n output_flags="$3"
    local flag
    local blocked_flag
    local blocked

    output_flags=()

    for flag in "${input_flags[@]}"; do
        blocked=0

        for blocked_flag in "${blocked_flags[@]}"; do
            if [[ "${flag}" == "${blocked_flag}" ]]; then
                blocked=1
                break
            fi
        done

        # Do not use "(( blocked == 0 )) && ..." with set -e. When the test is
        # false, Bash returns status 1, and set -e can abort the script.
        if (( blocked == 0 )); then
            output_flags+=("${flag}")
        fi
    done

    return 0
}

# Return success if the requested profile appears in GCC_BUILD_PROFILES.
profile_is_known() {
    local requested_profile="$1"
    local known_profile

    for known_profile in "${GCC_BUILD_PROFILES[@]}"; do
        if [[ "${requested_profile}" == "${known_profile}" ]]; then
            return 0
        fi
    done

    return 1
}

# Create a static archive.
#
# The simple case is:
#   ar rcs libfsutil.a fsutil.o
#
# If STATIC_ARCHIVE_DEPS contains other local .a files, this function extracts
# their object members and places those members into libfsutil.a too. That gives
# you a single archive containing fsutil plus those local static dependencies.
#
# Again: this is for project-local static archives. It is not how libc or
# sanitizer runtimes are bundled. Final programs still link their runtime libs.
create_static_archive() {
    local output_archive="$1"
    local own_object="$2"
    local merge_root
    local dep_archive
    local dep_archive_path
    local dep_index=0
    local dep_dir
    local member
    local -a archive_members=("${own_object}")

    rm -f "${output_archive}"

    if ((${#STATIC_ARCHIVE_DEPS[@]} > 0)); then
        merge_root="$(mktemp -d "${BUILD_DIR}/archive-merge.XXXXXX")"

        for dep_archive in "${STATIC_ARCHIVE_DEPS[@]}"; do
            if [[ "${dep_archive}" = /* ]]; then
                dep_archive_path="${dep_archive}"
            else
                dep_archive_path="${ROOT_DIR}/${dep_archive}"
            fi

            [[ -f "${dep_archive_path}" ]] || die "static dependency archive not found: ${dep_archive}"

            dep_index=$((dep_index + 1))
            dep_dir="${merge_root}/dep_${dep_index}"
            mkdir -p "${dep_dir}"

            # ar extracts archive members into the current directory, so isolate
            # each dependency in its own directory to avoid filename collisions.
            (cd "${dep_dir}" && "${AR}" x "${dep_archive_path}")

            while IFS= read -r -d '' member; do
                archive_members+=("${member}")
            done < <(find "${dep_dir}" -type f -print0 | sort -z)
        done

        "${AR}" rcs "${output_archive}" "${archive_members[@]}"
        rm -rf "${merge_root}"
    else
        "${AR}" rcs "${output_archive}" "${own_object}"
    fi

    # ar rcs normally writes an index already. Running ranlib when available is
    # harmless and keeps older/static-linker workflows happy.
    if command -v "${RANLIB}" >/dev/null 2>&1; then
        "${RANLIB}" "${output_archive}"
    fi
}

# Build one library directory from three flag arrays.
#
# Arguments:
#   1. Display/build label, such as "release" or "release_cov".
#   2. Output directory, such as build/release.
#   3. Name of the CPPFLAGS array to use.
#   4. Name of the CFLAGS array to use.
#   5. Name of the LDFLAGS array to use.
#
# Files produced:
#   fsutil.pic.o     object compiled for libfsutil.so
#   fsutil.o         object archived into libfsutil.a
#   libfsutil.so     shared library
#   libfsutil.a      static archive
build_library_variant() {
    local label="$1"
    local output_dir="$2"
    local cppflags_array_name="$3"
    local cflags_array_name="$4"
    local ldflags_array_name="$5"

    local -n raw_cppflags_ref="${cppflags_array_name}"
    local -n raw_cflags_ref="${cflags_array_name}"
    local -n raw_ldflags_ref="${ldflags_array_name}"

    # Copy the nameref arrays into normal local arrays. This keeps the rest of
    # the function straightforward and avoids accidental mutation of the profile
    # arrays imported from gcc_build_profiles.sh.
    local -a cppflags=("${raw_cppflags_ref[@]}")
    local -a raw_cflags=("${raw_cflags_ref[@]}")
    local -a raw_ldflags=("${raw_ldflags_ref[@]}")
    local -a library_cflags=()
    local -a library_ldflags=()

    local static_object="${output_dir}/fsutil.o"
    local shared_object="${output_dir}/fsutil.pic.o"
    local static_library="${output_dir}/libfsutil.a"
    local shared_library="${output_dir}/libfsutil.so"

    # Remove executable-only build policy from this library build.
    filter_flags raw_cflags CFLAGS_EXE_HARDENING library_cflags
    filter_flags raw_ldflags LDFLAGS_EXE_HARDENING library_ldflags

    mkdir -p "${output_dir}"

    printf '\n[%s]\n' "${label}"

    printf '  compiling shared-library object: %s\n' "${shared_object}"
    "${CC}" \
        "${cppflags[@]}" \
        "${library_cflags[@]}" \
        "${CFLAGS_SHARED[@]}" \
        -c fsutil.c \
        -o "${shared_object}"

    printf '  compiling static-library object: %s\n' "${static_object}"
    "${CC}" \
        "${cppflags[@]}" \
        "${library_cflags[@]}" \
        -c fsutil.c \
        -o "${static_object}"

    printf '  linking shared library:          %s\n' "${shared_library}"
    "${CC}" \
        "${LDFLAGS_SHARED[@]}" \
        "${library_ldflags[@]}" \
        -o "${shared_library}" \
        "${shared_object}" \
        "${SHARED_LINK_LIBS[@]}"

    printf '  creating static library:         %s\n' "${static_library}"
    create_static_archive "${static_library}" "${static_object}"

    printf '  output summary:\n'
    print_artifact_report "${shared_library}"
    print_artifact_report "${static_library}"

    BUILT_ARTIFACTS+=("${shared_library}" "${static_library}")
}

# Build a normal profile from GCC_BUILD_PROFILES.
#
# Example:
#   profile="release"
#   upper="RELEASE"
#   cppflags_array="CPPFLAGS_RELEASE"
#   cflags_array="CFLAGS_RELEASE"
#   ldflags_array="LDFLAGS_RELEASE"
build_normal_profile() {
    local profile="$1"
    local upper_profile="${profile^^}"
    local cppflags_array="CPPFLAGS_${upper_profile}"
    local cflags_array="CFLAGS_${upper_profile}"
    local ldflags_array="LDFLAGS_${upper_profile}"
    local profile_build_dir="${BUILD_DIR}/${profile}"

    require_array "${cppflags_array}"
    require_array "${cflags_array}"
    require_array "${ldflags_array}"

    build_library_variant \
        "${profile}" \
        "${profile_build_dir}" \
        "${cppflags_array}" \
        "${cflags_array}" \
        "${ldflags_array}"
}

# Build the coverage profile.
#
# Coverage is not a normal profile in GCC_BUILD_PROFILES. It is a derived build:
#   release flags + coverage instrumentation flags
#
# That keeps release policy and coverage instrumentation separate. The result is
# named release_cov so it can live next to the normal release build without
# overwriting it.
build_coverage_profile() {
    local coverage_profile="release_cov"
    local coverage_build_dir="${BUILD_DIR}/${coverage_profile}"

    if ! command -v gcov >/dev/null 2>&1; then
        printf '\n[%s]\n' "${coverage_profile}"
        printf '  skipped: gcov not found\n'
        return 0
    fi

    local -a coverage_cppflags=("${CPPFLAGS_RELEASE[@]}")
    local -a coverage_cflags=(
      "${CFLAGS_RELEASE[@]}"
      "${CFLAGS_INSTRUMENT_COVERAGE[@]}"
    )
    local -a coverage_ldflags=(
      "${LDFLAGS_RELEASE[@]}"
      "${LDFLAGS_INSTRUMENT_COVERAGE[@]}"
    )

    printf '\n[%s setup]\n' "${coverage_profile}"
    printf '  base profile:          release\n'
    printf '  extra instrumentation: --coverage\n'

    build_library_variant \
        "${coverage_profile}" \
        "${coverage_build_dir}" \
        coverage_cppflags \
        coverage_cflags \
        coverage_ldflags
}

# Remember where the user launched the script so the script can safely cd into
# the repository root and still return the user to the original directory when it
# exits.
START_DIR="$(pwd -P)"
cleanup() {
    cd -- "${START_DIR}"
}
trap cleanup EXIT

# Resolve paths relative to this script, not relative to wherever the user ran
# the command from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROFILE_FILE="${SCRIPT_DIR}/gcc_build_profiles.sh"
BUILD_DIR="${ROOT_DIR}/build"

cd -- "${ROOT_DIR}"

[[ -f "${PROFILE_FILE}" ]] || die "gcc profile file not found: ${PROFILE_FILE}"
[[ -f "fsutil.c" ]] || die "source file not found: ${ROOT_DIR}/fsutil.c"

require_tool "${CC}"
require_tool "${AR}"

# Source the profile file after basic path checks. The sourced file defines all
# build flag arrays used by the functions above.
source "${PROFILE_FILE}"

require_array GCC_BUILD_PROFILES
require_array CFLAGS_SHARED
require_array LDFLAGS_SHARED
require_array CFLAGS_EXE_HARDENING
require_array LDFLAGS_EXE_HARDENING
require_array CPPFLAGS_RELEASE
require_array CFLAGS_RELEASE
require_array LDFLAGS_RELEASE
require_array CFLAGS_INSTRUMENT_COVERAGE
require_array LDFLAGS_INSTRUMENT_COVERAGE

mkdir -p "${BUILD_DIR}"

printf 'building fsutil libraries in %s\n' "${BUILD_DIR}"
printf 'using profile policy from %s\n' "${PROFILE_FILE}"
printf 'compiler: %s\n' "${CC}"
printf 'archiver: %s\n' "${AR}"

# Command-line behavior:
#   ./utils/build_libs.sh
#       Build every normal profile from GCC_BUILD_PROFILES, then build
#       release_cov when gcov is available.
#
#   ./utils/build_libs.sh release debug
#       Build only the requested normal profiles.
#
#   ./utils/build_libs.sh coverage
#   ./utils/build_libs.sh release_cov
#       Build only the coverage-derived library set.
#
#   ./utils/build_libs.sh profiles
#   ./utils/build_libs.sh all
#       Build every normal profile and coverage, same as no arguments.
if (($# == 0)); then
    REQUESTED_BUILDS=("${GCC_BUILD_PROFILES[@]}" release_cov)
else
    REQUESTED_BUILDS=("$@")
fi

for requested_build in "${REQUESTED_BUILDS[@]}"; do
    case "${requested_build}" in
        all|profiles)
            for profile in "${GCC_BUILD_PROFILES[@]}"; do
                build_normal_profile "${profile}"
            done
            build_coverage_profile
            ;;
        coverage|release_cov)
            build_coverage_profile
            ;;
        *)
            if profile_is_known "${requested_build}"; then
                build_normal_profile "${requested_build}"
            else
                die "unknown build profile: ${requested_build}"
            fi
            ;;
    esac
done

printf '\nbuilt artifacts:\n'
if ((${#BUILT_ARTIFACTS[@]} == 0)); then
    printf '  none\n'
else
    for artifact in "${BUILT_ARTIFACTS[@]}"; do
        printf '  %s\n' "${artifact}"
    done
fi
