#!/usr/bin/env bash
# shellcheck shell=bash
# fs_util_project.sh — what the ABI gate (utils/check_abi.sh, kyr-build share/abi) treats as fs_util's public ABI: the
# one public header app/fsutil.h, and the prefixes of its types, functions and numeric constants. The build scripts do not read
# this file; the linker map's node is FSUTIL_<major>.
KYR_LIB="fsutil"
KYR_ABI_NAME="fs_util"
KYR_ABI_INCLUDE_DIR="app"
KYR_ABI_UMBRELLA="fsutil.h"
KYR_ABI_TYPE_PREFIX="fs_"
KYR_ABI_FUNC_PREFIX="fs_"
KYR_ABI_CONST_PREFIX="FS_"
KYR_ABI_NODE="FSUTIL"
