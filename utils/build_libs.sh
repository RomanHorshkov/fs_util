#!/usr/bin/env bash


# Build static and shared libraries.


# -e: stop immediately if a command fails.
# -u: stop if an unset variable is used.
# -o pipefail: fail a pipeline if any command in the pipeline fails.
set -euo pipefail

# Print a short, human-meaningful description of a produced artifact.
print_artifact_report() {
    local artifact_path="$1"

    printf '    artifact: %s\n' "${artifact_path}"
    printf '      size:   %s bytes\n' "$(wc -c < "${artifact_path}")"
    printf '      type:   %s\n' "$(file -b "${artifact_path}")"
}

# Remember where the user launched the script.
START_DIR="$(pwd -P)"
# Return to the user's launch directory.
cleanup() { cd -- "$START_DIR"; }
# Always return to START_DIR on script exit.
trap cleanup EXIT

# Determine the directory that contains this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Determine the root directory of the project (the parent of the script's directory).
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd -- "$ROOT_DIR"

# Load the canonical GCC build-profile definitions from this repository's local
# utils directory so the project stays standalone regardless of the caller's
# start directory or surrounding checkout layout.
PROFILE_FILE="${SCRIPT_DIR}/gcc_build_profiles.sh"

if [[ ! -f "${PROFILE_FILE}" ]]; then
    printf 'gcc profile file not found: %s\n' "${PROFILE_FILE}" >&2
    exit 1
fi

source "${PROFILE_FILE}"

# Create the build directory if it doesn't exist.
BUILD_DIR="${ROOT_DIR}/build"
mkdir -p "${BUILD_DIR}"

printf 'building fsutil libraries in %s\n' "${BUILD_DIR}"


# Build both library variants for every profile exported by the shared GCC
# profile configuration.
#
# The profile file gives us names such as:
#   - debug
#   - release
#   - native
#
# For each profile we create:
#   - build/<profile>/libfsutil.a
#   - build/<profile>/libfsutil.so
#
# The profile arrays follow a predictable naming convention:
#   CPPFLAGS_DEBUG   CFLAGS_DEBUG   LDFLAGS_DEBUG
#   CPPFLAGS_RELEASE CFLAGS_RELEASE LDFLAGS_RELEASE
#   ...
#
# The loop below converts the lowercase profile name into uppercase, rebuilds
# those array names as strings, and then uses Bash namerefs so gcc receives the
# correct arrays for the current profile.

for profile in "${GCC_BUILD_PROFILES[@]}"; do
    profile_upper="${profile^^}"
    profile_build_dir="${BUILD_DIR}/${profile}"

    cppflags_var="CPPFLAGS_${profile_upper}"
    cflags_var="CFLAGS_${profile_upper}"
    ldflags_var="LDFLAGS_${profile_upper}"

    declare -n cppflags_ref="${cppflags_var}"
    declare -n cflags_ref="${cflags_var}"
    declare -n ldflags_ref="${ldflags_var}"

    # Keep each profile's artifacts in its own directory so builds do not
    # overwrite one another and inspection stays simple.
    mkdir -p "${profile_build_dir}"

    printf '\n[%s]\n' "${profile}"
    printf '  compiling PIC object:    %s\n' "${profile_build_dir}/fsutil.pic.o"

    # Position-independent object for the shared library.
    gcc "${cppflags_ref[@]}" "${cflags_ref[@]}" -fPIC -c fsutil.c \
        -o "${profile_build_dir}/fsutil.pic.o"

    printf '  compiling static object: %s\n' "${profile_build_dir}/fsutil.o"

    # Normal object for the static library.
    gcc "${cppflags_ref[@]}" "${cflags_ref[@]}" -c fsutil.c \
        -o "${profile_build_dir}/fsutil.o"

    printf '  linking shared library:  %s\n' "${profile_build_dir}/libfsutil.so"

    # Shared library (.so): link the PIC object and apply the profile's link
    # flags, which matters for profiles such as sanitize, native, and tsan.
    gcc -shared "${ldflags_ref[@]}" \
        -o "${profile_build_dir}/libfsutil.so" \
        "${profile_build_dir}/fsutil.pic.o"

    printf '  creating static library: %s\n' "${profile_build_dir}/libfsutil.a"

    # Static library (.a): archive the non-PIC object.
    ar rcs "${profile_build_dir}/libfsutil.a" \
        "${profile_build_dir}/fsutil.o"

    # Report what was actually produced, not just that the commands ran.
    printf '  output summary:\n'
    print_artifact_report "${profile_build_dir}/libfsutil.so"
    print_artifact_report "${profile_build_dir}/libfsutil.a"
done

# Optional coverage build
# -----------------------
# Coverage is deliberately NOT part of the canonical normal-profile loop.
#
# Instead, we build one explicit derived variant here:
#   release_cov
#
# That keeps the main profile lineup stable while still making coverage
# instrumentation available as a deliberate extra. Tests can then discover
# release_cov exactly like any other built library directory.
#
# We require both:
#   - gcov   : the underlying GCC coverage toolchain support;
#   - gcovr  : the reporting tool used later by the test runner.
#
# If either tool is missing, we skip the coverage variant cleanly.
if command -v gcov >/dev/null 2>&1 && command -v gcovr >/dev/null 2>&1; then
    coverage_profile="release_cov"
    coverage_build_dir="${BUILD_DIR}/${coverage_profile}"

    # Start from the normal release policy, then layer coverage
    # instrumentation on top of it.
    coverage_cppflags=(
      "${CPPFLAGS_RELEASE[@]}"
    )

    coverage_cflags=(
      "${CFLAGS_RELEASE[@]}"
      "${CFLAGS_INSTRUMENT_COVERAGE[@]}"
    )

    coverage_ldflags=(
      "${LDFLAGS_RELEASE[@]}"
      "${LDFLAGS_INSTRUMENT_COVERAGE[@]}"
    )

    mkdir -p "${coverage_build_dir}"

    printf '\n[%s]\n' "${coverage_profile}"
    printf '  base profile:             release\n'
    printf '  extra instrumentation:    --coverage\n'
    printf '  compiling PIC object:     %s\n' "${coverage_build_dir}/fsutil.pic.o"

    gcc "${coverage_cppflags[@]}" "${coverage_cflags[@]}" -fPIC -c fsutil.c \
        -o "${coverage_build_dir}/fsutil.pic.o"

    printf '  compiling static object:  %s\n' "${coverage_build_dir}/fsutil.o"

    gcc "${coverage_cppflags[@]}" "${coverage_cflags[@]}" -c fsutil.c \
        -o "${coverage_build_dir}/fsutil.o"

    printf '  linking shared library:   %s\n' "${coverage_build_dir}/libfsutil.so"

    gcc -shared "${coverage_ldflags[@]}" \
        -o "${coverage_build_dir}/libfsutil.so" \
        "${coverage_build_dir}/fsutil.pic.o"

    printf '  creating static library:  %s\n' "${coverage_build_dir}/libfsutil.a"

    ar rcs "${coverage_build_dir}/libfsutil.a" \
        "${coverage_build_dir}/fsutil.o"

    printf '  output summary:\n'
    print_artifact_report "${coverage_build_dir}/libfsutil.so"
    print_artifact_report "${coverage_build_dir}/libfsutil.a"
else
    printf '\n[release_cov]\n'
    printf '  skipped: gcov and/or gcovr not found\n'
fi

printf 'artifacts:\n'
for profile in "${GCC_BUILD_PROFILES[@]}"; do
    printf '  %s\n' "${BUILD_DIR}/${profile}/libfsutil.a"
    printf '  %s\n' "${BUILD_DIR}/${profile}/libfsutil.so"
done

if [[ -f "${BUILD_DIR}/release_cov/libfsutil.a" ]]; then
    printf '  %s\n' "${BUILD_DIR}/release_cov/libfsutil.a"
    printf '  %s\n' "${BUILD_DIR}/release_cov/libfsutil.so"
fi
