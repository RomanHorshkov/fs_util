#!/usr/bin/env bash

# Build the fsutil Debian package.

set -euo pipefail

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd -- "${ROOT_DIR}"

if ! command -v dpkg-deb >/dev/null 2>&1; then
    printf 'dpkg-deb not found in PATH\n' >&2
    exit 1
fi

PACKAGE_NAME="${FSUTIL_DEB_PACKAGE_NAME:-libfsutil}"
PROFILE="${FSUTIL_DEB_PROFILE:-release}"
VERSION="$(tr -d '[:space:]' < "${ROOT_DIR}/VERSION")"
ARCH="$(dpkg --print-architecture)"
STRIP_BIN="${STRIP:-strip}"
BUILD_DIR="${ROOT_DIR}/build"
PROFILE_DIR="${BUILD_DIR}/${PROFILE}"
DEB_WORK_DIR="${BUILD_DIR}/deb"
OUT_DIR="${BUILD_DIR}/debs"
STAGE_DIR="${DEB_WORK_DIR}/${PACKAGE_NAME}_${VERSION}_${ARCH}"
OUT_DEB="${OUT_DIR}/${PACKAGE_NAME}_${VERSION}_${ARCH}.deb"
SHARED_LIB="${PROFILE_DIR}/libfsutil.so"
STATIC_LIB="${PROFILE_DIR}/libfsutil.a"
MAJOR="${VERSION%%.*}"

if [[ -z "${VERSION}" ]]; then
    printf 'VERSION is empty\n' >&2
    exit 1
fi

# The deb filename, soname chain, and control file all embed VERSION verbatim.
# Refuse anything that is not strict MAJOR.MINOR.PATCH.
if ! [[ "${VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    printf 'error: VERSION must match MAJOR.MINOR.PATCH (digits only), got: %q\n' "${VERSION}" >&2
    exit 1
fi

printf '[deb] building fsutil profile: %s\n' "${PROFILE}"
"${ROOT_DIR}/utils/build_libs.sh" "${PROFILE}"

if [[ ! -f "${SHARED_LIB}" ]]; then
    printf 'missing shared library: %s\n' "${SHARED_LIB}" >&2
    exit 1
fi
if [[ ! -f "${STATIC_LIB}" ]]; then
    printf 'missing static library: %s\n' "${STATIC_LIB}" >&2
    exit 1
fi

rm -rf "${STAGE_DIR}"
mkdir -p "${STAGE_DIR}/DEBIAN" \
         "${STAGE_DIR}/usr/local/include/utils" \
         "${STAGE_DIR}/usr/local/lib" \
         "${STAGE_DIR}/usr/local/lib/pkgconfig"

install -m 0644 "${ROOT_DIR}/fsutil.h" "${STAGE_DIR}/usr/local/include/fsutil.h"
install -m 0644 "${ROOT_DIR}/fsutil.h" "${STAGE_DIR}/usr/local/include/utils/fsutil.h"

install -m 0755 "${SHARED_LIB}" "${STAGE_DIR}/usr/local/lib/libfsutil.so.${VERSION}"
if command -v "${STRIP_BIN}" >/dev/null 2>&1; then
    "${STRIP_BIN}" --strip-unneeded "${STAGE_DIR}/usr/local/lib/libfsutil.so.${VERSION}"
fi
ln -sfn "libfsutil.so.${VERSION}" "${STAGE_DIR}/usr/local/lib/libfsutil.so.${MAJOR}"
ln -sfn "libfsutil.so.${VERSION}" "${STAGE_DIR}/usr/local/lib/libfsutil.so"
install -m 0644 "${STATIC_LIB}" "${STAGE_DIR}/usr/local/lib/libfsutil.a"

cat > "${STAGE_DIR}/usr/local/lib/pkgconfig/fsutil.pc" <<PC
prefix=/usr/local
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include
utilsincludedir=\${includedir}/utils

Name: fsutil
Description: Strict capability-oriented filesystem helper library
Version: ${VERSION}
Libs: -L\${libdir} -lfsutil
Cflags: -I\${includedir} -I\${utilsincludedir}
PC

cat > "${STAGE_DIR}/DEBIAN/control" <<CTRL
Package: ${PACKAGE_NAME}
Version: ${VERSION}
Section: libs
Priority: optional
Architecture: ${ARCH}
Maintainer: ${DEB_MAINTAINER:-Roman Horshkov <124358264+RomanHorshkov@users.noreply.github.com>}
Depends: libc6
Description: fsutil capability-oriented filesystem helper library
 fsutil is a C helper library for dirfd-based filesystem operations, strict
 component validation, explicit metadata verification, and explicit durability
 barriers. It installs libraries under /usr/local/lib and headers under both
 /usr/local/include and /usr/local/include/utils.
CTRL

cat > "${STAGE_DIR}/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
ldconfig
exit 0
POSTINST
chmod 0755 "${STAGE_DIR}/DEBIAN/postinst"

cat > "${STAGE_DIR}/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
ldconfig
exit 0
POSTRM
chmod 0755 "${STAGE_DIR}/DEBIAN/postrm"

find "${STAGE_DIR}" -type d -exec chmod 0755 {} +
find "${STAGE_DIR}" -type f -name '*.h' -exec chmod 0644 {} +
find "${STAGE_DIR}" -type f -name '*.a' -exec chmod 0644 {} +
find "${STAGE_DIR}" -type f -name '*.pc' -exec chmod 0644 {} +

# Verify the staged (stripped) payload still carries the release hardening
# before it gets sealed into a package. A red check kills the build here.
"${ROOT_DIR}/utils/check_hardening.sh" \
    "${STAGE_DIR}/usr/local/lib/libfsutil.so.${VERSION}"

mkdir -p "${OUT_DIR}"
dpkg-deb --build --root-owner-group "${STAGE_DIR}" "${OUT_DEB}"

# Refresh checksums next to the deb(s) so consumers can verify what they fetch.
(cd "${OUT_DIR}" && sha256sum -- *.deb > SHA256SUMS)
printf '[deb] checksums refreshed: %s\n' "${OUT_DIR}/SHA256SUMS"

printf '\nBuilt complete\n'
printf '  package: %s\n' "${OUT_DEB}"
printf '  info:    dpkg-deb -I %s\n' "${OUT_DEB}"
printf '  list:    dpkg-deb -c %s\n' "${OUT_DEB}"
printf '  check:   dpkg --dry-run -i %s\n' "${OUT_DEB}"
