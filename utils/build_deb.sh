#!/usr/bin/env bash
# build_deb.sh — build/debs/: libfsutil_<ver>_<arch>.deb (runtime: libfsutil.so.<ver> and its soname link) and libfsutil-dev
# (app/fsutil.h at include/ and include/utils/, libfsutil.a, the linker link, fsutil.pc), through kyr-build's lib/deb.sh:
# X-Kyr-Tier from deps.txt, SHA256SUMS, one .buildinfo per deb, and the same bytes from the same commit anywhere.
set -euo pipefail
ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
export ROOT_DIR
LIB="fsutil"
PKG_RUNTIME="libfsutil"
PKG_DEV="libfsutil-dev"
DESCRIPTION="Strict capability-oriented filesystem helper library"
# shellcheck source=/dev/null
source "${ROOT_DIR}/utils/gcc_build_profiles.sh"
# shellcheck source=/dev/null
source "${KYR_BUILD_DIR}/lib/deb.sh"
cd "${ROOT_DIR}"
./utils/build_libs.sh release
deb_init
REAL="lib${LIB}.so.${DEB_VERSION}"

stage="$(deb_stage "${PKG_RUNTIME}" lib)"
install -m 0755 "build/release/${REAL}" "${stage}/usr/local/lib/${REAL}"
"${STRIP:-strip}" --strip-unneeded "${stage}/usr/local/lib/${REAL}"
ln -sfn "${REAL}" "${stage}/usr/local/lib/lib${LIB}.so.${DEB_MAJOR}"
./utils/check_hardening.sh "${stage}/usr/local/lib/${REAL}"
deb_control "${stage}" "${PKG_RUNTIME}" libs "${DEB_ARCH}" "${DESCRIPTION}" "Depends: libc6"
deb_ldconfig_hooks "${stage}"
deb_copyright "${stage}" "${PKG_RUNTIME}"
deb_build "${stage}" "${PKG_RUNTIME}" > /dev/null

stage="$(deb_stage "${PKG_DEV}" lib lib/pkgconfig include include/utils)"
install -m 0644 "app/${LIB}.h" "${stage}/usr/local/include/${LIB}.h"
install -m 0644 "app/${LIB}.h" "${stage}/usr/local/include/utils/${LIB}.h"
install -m 0644 "build/release/lib${LIB}.a" "${stage}/usr/local/lib/lib${LIB}.a"
ln -sfn "${REAL}" "${stage}/usr/local/lib/lib${LIB}.so"
cat > "${stage}/usr/local/lib/pkgconfig/${LIB}.pc" <<PC
prefix=/usr/local
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include
utilsincludedir=\${includedir}/utils

Name: ${LIB}
Description: ${DESCRIPTION}
Version: ${DEB_VERSION}
Libs: -L\${libdir} -l${LIB}
Cflags: -I\${includedir} -I\${utilsincludedir}
PC
chmod 0644 "${stage}/usr/local/lib/pkgconfig/${LIB}.pc"
deb_control "${stage}" "${PKG_DEV}" libdevel "${DEB_ARCH}" \
    "Development files for ${PKG_RUNTIME} (headers, static library, linker symlink, pkg-config)" \
    "Depends: ${PKG_RUNTIME} (= ${DEB_VERSION})" "Breaks: ${PKG_RUNTIME} (<< ${DEB_VERSION})" "Replaces: ${PKG_RUNTIME} (<< ${DEB_VERSION})"
deb_copyright "${stage}" "${PKG_DEV}"
deb_build "${stage}" "${PKG_DEV}" > /dev/null

deb_sha256sums
cat "${DEB_OUT_DIR}/SHA256SUMS"
