/**
 * @file fsutil_test_hooks.h
 * @brief Private syscall seam for the fs_util unit tests.
 *
 * Only exists in a build compiled with -DFS_UTIL_TESTING (see the FS_SYS_* macros in
 * fsutil.c). A release/production build never includes this header and exports none of
 * these symbols. The unit tests replace individual entries to make a kernel call fail
 * exactly where a real filesystem would not fail on demand, then call
 * fs_test_hooks_reset() to put the real libc functions back.
 *
 * @author  Roman Horshkov <github.com/RomanHorshkov>
 * @date    2026
 * (c) 2026
 */

#ifndef FSUTIL_TEST_HOOKS_H
#define FSUTIL_TEST_HOOKS_H

#include <sys/stat.h>
#include <sys/types.h>

typedef struct fs_test_hooks
{
    int (*open)(const char* path, int flags);
    int (*openat)(int dirfd, const char* name, int flags, mode_t mode);
    int (*mkdirat)(int dirfd, const char* name, mode_t mode);
    int (*fchmod)(int fd, mode_t mode);
    int (*fchmodat)(int dirfd, const char* name, mode_t mode);
    int (*fstat)(int fd, struct stat* st);
    int (*fcntl)(int fd, int cmd, int arg);
    int (*fsync)(int fd);
    int (*renameat)(int old_dirfd, const char* old_name, int new_dirfd, const char* new_name);
    int (*renameat2)(int old_dirfd, const char* old_name, int new_dirfd, const char* new_name, unsigned int flags);
    int (*unlinkat)(int dirfd, const char* name, int flags);
} fs_test_hooks_t;

/** The live table consulted by every FS_SYS_* call in a test build. */
extern fs_test_hooks_t fs_test_hooks;

/** Restore every entry to the real libc function. Call from every test teardown. */
void fs_test_hooks_reset(void);

#endif /* FSUTIL_TEST_HOOKS_H */
