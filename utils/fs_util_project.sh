#!/usr/bin/env bash
# shellcheck shell=bash
# fs_util_project.sh — fs_util's declarations for the pinned kyr-build (every other script in utils/ is its wrapper): the
# library, the suites, the debs and what the ABI gate treats as public. Declarations only.
KYR_LIB="fsutil"
KYR_LIB_SOURCES=(app/fsutil.c)
KYR_LIB_HEADER="app/fsutil.h"
KYR_LIB_MAP="utils/fsutil.map"
KYR_LIB_CPPFLAGS=(-I"${ROOT_DIR}/app")
KYR_LIB_LDLIBS=()
# The fault-injection seams (tests/UTs/fsutil_test_hooks.h), in the coverage and sanitizer runs only.
KYR_TEST_HOOKS_CPPFLAGS=(-DFS_UTIL_TESTING -I"${ROOT_DIR}/tests/UTs")

# Unit suites, one executable per source; 100% line and branch coverage of app/fsutil.c.
KYR_UT_SUITES=(ut)
KYR_SUITE_UT_SOURCES=('tests/UTs/*.c')
KYR_SUITE_UT_EACH=1
KYR_SUITE_UT_CPPFLAGS=(-D_GNU_SOURCE -I"${ROOT_DIR}/tests/UTs")
KYR_UT_FAIL_UNDER_LINE=100
KYR_UT_FAIL_UNDER_BRANCH=100
# The integration suite against every profile's archive and shared object, then the coverage run.
KYR_IT_SUITES=(it)
KYR_SUITE_IT_SOURCES=(tests/ITs/integration_test.c)
KYR_SUITE_IT_CPPFLAGS=(-D_GNU_SOURCE -I"${ROOT_DIR}/tests/ITs")
KYR_IT_PROFILES=("${GCC_BUILD_PROFILES[@]}")
KYR_IT_LINKAGES=(static shared)
KYR_IT_COVERAGE=1

# libfsutil and libfsutil-dev (the header, fsutil.pc).
KYR_PKG="libfsutil"
KYR_PKG_DESCRIPTION="Strict capability-oriented filesystem helper library"
KYR_DEB_HEADERS=(app/fsutil.h)
KYR_DEB_DEV_FIELDS=("Breaks: libfsutil (<< @VERSION@)" "Replaces: libfsutil (<< @VERSION@)")
KYR_DEB_DEV_DESCRIPTION="Development files for libfsutil (headers, static library, linker symlink, pkg-config)"
KYR_SMOKE_CPPFLAGS=(-D_GNU_SOURCE)

# The public ABI: app/fsutil.h; the linker map's node is FSUTIL_<major>.
KYR_ABI_NAME="fs_util"
KYR_ABI_INCLUDE_DIR="app"
KYR_ABI_UMBRELLA="fsutil.h"
KYR_ABI_TYPE_PREFIX="fs_"
KYR_ABI_FUNC_PREFIX="fs_"
KYR_ABI_CONST_PREFIX="FS_"
KYR_ABI_NODE="FSUTIL"

