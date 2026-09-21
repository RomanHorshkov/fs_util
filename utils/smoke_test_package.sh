#!/usr/bin/env bash
# =============================================================================
# smoke_test_package.sh — compile and run a tiny program against ONLY the
# installed package (/usr/local/include, /usr/local/lib), proving the shipped
# .deb works standalone rather than just that the source tree builds.
#
# author  Roman Horshkov <github.com/RomanHorshkov>
# date    2026
# (c) 2026
# =============================================================================
set -euo pipefail

START_DIR="$(pwd -P)"
cleanup() { cd -- "${START_DIR}"; }
trap cleanup EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd -- "${ROOT_DIR}"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "${WORK_DIR}"; cleanup' EXIT

if [[ ! -f /usr/local/include/fsutil.h ]]; then
    printf 'smoke_test_package: /usr/local/include/fsutil.h not found — install the .deb first\n' >&2
    exit 1
fi

cat > "${WORK_DIR}/smoke.c" <<'CSRC'
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fsutil.h>

int main(void)
{
    char        root[] = "/tmp/fsutil-smoke-XXXXXX";
    fs_dir_t    top;
    fs_dir_t    data;
    fs_expect_t private_dir = fs_expect_private(0700);
    int         fd          = -1;

    assert(mkdtemp(root) != NULL);
    fs_dir_init(&top);
    fs_dir_init(&data);

    assert(fs_dir_open_abs_nofollow(root, NULL, &top) == 0);
    assert(fs_dir_walk_create(&top, "./data/store", 0700, &private_dir, &data) == 0);
    assert(fs_file_create_write_new_at(&data, "marker.tmp", 0600, NULL, &fd) == 0);
    assert(write(fd, "ok", 2) == 2);
    assert(fs_file_fsync(fd) == 0);
    assert(close(fd) == 0);
    assert(fs_rename_noreplace_at(&data, "marker.tmp", &data, "marker") == 0);
    assert(fs_rename_noreplace_at(&data, "marker", &data, "marker") == -EEXIST);
    assert(fs_dir_fsync(&data) == 0);
    assert(fs_unlink_at(&data, "marker") == 0);

    fs_dir_close(&data);
    fs_dir_close(&top);
    printf("smoke test: installed package round-trips correctly\n");
    return 0;
}
CSRC

gcc -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE \
    -I/usr/local/include \
    "${WORK_DIR}/smoke.c" \
    -L/usr/local/lib -Wl,-rpath,/usr/local/lib -lfsutil \
    -o "${WORK_DIR}/smoke"
"${WORK_DIR}/smoke"
