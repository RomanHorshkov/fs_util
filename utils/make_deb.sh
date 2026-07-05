#!/usr/bin/env bash
set -euo pipefail

# Compatibility wrapper. Prefer using:
#   ./utils/build_deb.sh

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}" >/dev/null 2>&1 || true; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"${SCRIPT_DIR}/build_deb.sh" "$@"
