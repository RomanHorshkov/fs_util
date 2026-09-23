#!/usr/bin/env bash
# =============================================================================
# build_deb.sh — package the release-profile libfsutil artifacts into debs
#
# author  Roman Horshkov <github.com/RomanHorshkov>
# date    2026
# (c) 2026
# =============================================================================
#
#   libfsutil_<ver>_<arch>.deb      runtime: libfsutil.so.<ver> + soname symlink
#   libfsutil-dev_<ver>_<arch>.deb  development: fsutil.h (also under include/utils),
#                                   libfsutil.a, libfsutil.so linker symlink,
#                                   pkgconfig/fsutil.pc; depends on the exact runtime
#
# plus a SHA256SUMS manifest covering both, in build/debs/. The dev package
# replaces the files older single-package libfsutil versions shipped.
# =============================================================================
set -euo pipefail

ROOT_DIR="${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
LIB="fsutil"
PKG_RUNTIME="libfsutil"
PKG_DEV="libfsutil-dev"
DESCRIPTION="Strict capability-oriented filesystem helper library"
STRIP="${STRIP:-strip}"

die() { printf '%s: %s\n' "${BASH_SOURCE[0]}" "$1" >&2; exit 1; }

cd "$ROOT_DIR"

VER="$(tr -d '[:space:]' < VERSION)"
[[ "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "VERSION '${VER}' does not match ^[0-9]+\\.[0-9]+\\.[0-9]+\$"

./utils/build_libs.sh release

ARCH="$(dpkg --print-architecture)"
IFS='.' read -r MAJOR MINOR PATCH <<< "$VER"

COPYRIGHT_SRC="${ROOT_DIR}/debian/copyright"
[[ -f "${COPYRIGHT_SRC}" ]] || die "missing ${COPYRIGHT_SRC} — third-party notices must ship in the deb"

OUT_DIR="${OUT_DIR:-${ROOT_DIR}/build/debs}"
rm -rf "$OUT_DIR"
install -d -m 0755 "$OUT_DIR"

stage_dirs() {
    local stage="$1"; shift
    rm -rf "$stage"
    install -d -m 0755 "$stage" "$stage/DEBIAN" "$stage/usr" "$stage/usr/local" \
        "$stage/usr/share" "$stage/usr/share/doc"
    local d
    for d in "$@"; do install -d -m 0755 "$stage/usr/local/$d"; done
}

write_ldconfig_hooks() {
    local stage="$1" hook
    for hook in postinst postrm; do
        printf '#!/bin/sh\nset -e\nldconfig\nexit 0\n' > "$stage/DEBIAN/$hook"
        chmod 0755 "$stage/DEBIAN/$hook"
    done
}

# --- runtime package -----------------------------------------------------------
STAGE_RT="${ROOT_DIR}/build/pkgroot/${PKG_RUNTIME}"
stage_dirs "$STAGE_RT" lib
LIB_RT="$STAGE_RT/usr/local/lib"

install -m 0755 "build/release/lib${LIB}.so.$VER" "$LIB_RT/lib${LIB}.so.$VER"
"$STRIP" --strip-unneeded "$LIB_RT/lib${LIB}.so.$VER"
ln -sf "lib${LIB}.so.$VER" "$LIB_RT/lib${LIB}.so.$MAJOR"

"${ROOT_DIR}/utils/check_hardening.sh" "$LIB_RT/lib${LIB}.so.$VER"

cat > "$STAGE_RT/DEBIAN/control" <<EOF
Package: $PKG_RUNTIME
Version: $VER
Section: libs
Priority: optional
Architecture: $ARCH
Depends: libc6
Maintainer: Roman Horshkov <https://github.com/RomanHorshkov>
Description: $DESCRIPTION
EOF
write_ldconfig_hooks "$STAGE_RT"

install -d -m 0755 "${STAGE_RT}/usr/share/doc/${PKG_RUNTIME}"
install -m 0644 "${COPYRIGHT_SRC}" "${STAGE_RT}/usr/share/doc/${PKG_RUNTIME}/copyright"
DEB_RT="${PKG_RUNTIME}_${VER}_${ARCH}.deb"
fakeroot dpkg-deb --build "$STAGE_RT" "$OUT_DIR/$DEB_RT"

# --- development package --------------------------------------------------------
STAGE_DEV="${ROOT_DIR}/build/pkgroot/${PKG_DEV}"
stage_dirs "$STAGE_DEV" lib lib/pkgconfig include include/utils
LIB_DEV="$STAGE_DEV/usr/local/lib"

install -m 0644 "app/${LIB}.h" "$STAGE_DEV/usr/local/include/${LIB}.h"
install -m 0644 "app/${LIB}.h" "$STAGE_DEV/usr/local/include/utils/${LIB}.h"
install -m 0644 "build/release/lib${LIB}.a" "$LIB_DEV/lib${LIB}.a"
ln -sf "lib${LIB}.so.$VER" "$LIB_DEV/lib${LIB}.so"

cat > "$LIB_DEV/pkgconfig/${LIB}.pc" <<EOF
prefix=/usr/local
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include
utilsincludedir=\${includedir}/utils

Name: ${LIB}
Description: ${DESCRIPTION}
Version: ${VER}
Libs: -L\${libdir} -l${LIB}
Cflags: -I\${includedir} -I\${utilsincludedir}
EOF
chmod 0644 "$LIB_DEV/pkgconfig/${LIB}.pc"

cat > "$STAGE_DEV/DEBIAN/control" <<EOF
Package: $PKG_DEV
Version: $VER
Section: libdevel
Priority: optional
Architecture: $ARCH
Depends: $PKG_RUNTIME (= $VER)
Breaks: $PKG_RUNTIME (<< $VER)
Replaces: $PKG_RUNTIME (<< $VER)
Maintainer: Roman Horshkov <https://github.com/RomanHorshkov>
Description: Development files for $PKG_RUNTIME (headers, static library, linker symlink, pkg-config)
EOF

install -d -m 0755 "${STAGE_DEV}/usr/share/doc/${PKG_DEV}"
install -m 0644 "${COPYRIGHT_SRC}" "${STAGE_DEV}/usr/share/doc/${PKG_DEV}/copyright"
DEB_DEV="${PKG_DEV}_${VER}_${ARCH}.deb"
fakeroot dpkg-deb --build "$STAGE_DEV" "$OUT_DIR/$DEB_DEV"

(
    cd "$OUT_DIR"
    sha256sum -- *.deb > SHA256SUMS
)

printf '\nBuilt:\n  %s\n  %s\n' "$OUT_DIR/$DEB_RT" "$OUT_DIR/$DEB_DEV"
printf 'checksums: %s/SHA256SUMS\n' "$OUT_DIR"
printf 'install with: sudo apt install %s/%s %s/%s\n' "$OUT_DIR" "$DEB_RT" "$OUT_DIR" "$DEB_DEV"
