/**
 * @file unit_tests.c
 * @brief fs_util unit tests: every public function, every documented refusal, every
 *        failure branch.
 *
 * Two layers:
 * - contract tests use only the public API against a private temporary tree and run in
 *   every build, including the release-profile run against the shipped archive;
 * - fault-injection tests exist only when the library is compiled with -DFS_UTIL_TESTING
 *   (see tests/UTs/fsutil_test_hooks.h): they make one specific kernel call fail so the
 *   cleanup and error paths a real filesystem never exercises on demand are proven, not
 *   assumed. The release build compiles them out entirely.
 *
 * @author  Roman Horshkov <github.com/RomanHorshkov>
 * @date    2026
 * (c) 2026
 */

/*****************************************************************************************************************************************
 * PRIVATE INCLUDES
 *****************************************************************************************************************************************
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fsutil.h"

#ifdef FS_UTIL_TESTING
#    include "fsutil_test_hooks.h"
#endif

/*****************************************************************************************************************************************
 * PRIVATE STRUCTURED TYPEDEFS
 *****************************************************************************************************************************************
 */

typedef struct test_env
{
    char     root_path[PATH_MAX];
    fs_dir_t root; /* O_CLOEXEC capability on root_path */
} test_env_t;

/*****************************************************************************************************************************************
 * PRIVATE VARIABLES
 *****************************************************************************************************************************************
 */

#ifdef FS_UTIL_TESTING
/* "Fail the N-th call of syscall X with errno E" knobs, one per hooked call. */
static int g_fail_on_call;     /* 1-based index of the call that fails; 0 = never */
static int g_fail_errno;
static int g_call_count;
static int g_fcntl_cmd_filter; /* only fcntl calls with this cmd count; -1 = all */
#endif

/*****************************************************************************************************************************************
 * PRIVATE FUNCTIONS PROTOTYPES
 *****************************************************************************************************************************************
 */

static int  env_setup(void** state);
static int  env_teardown(void** state);
static int  remove_tree_at(int dir_fd);
static void make_dir(const fs_dir_t* parent, const char* name, mode_t mode);
static void make_file(const fs_dir_t* parent, const char* name, mode_t mode);
static void make_symlink(const fs_dir_t* parent, const char* name, const char* target);
static int  entry_exists(const fs_dir_t* parent, const char* name);
static void assert_same_identity_fd(int fd_a, int fd_b);
static void assert_cloexec(int fd);
static void assert_blocking(int fd);
static void assert_dir_symlink_rejected(int rc);

/*****************************************************************************************************************************************
 * PRIVATE FUNCTIONS DEFINITIONS — helpers
 *****************************************************************************************************************************************
 */

static int env_setup(void** state)
{
    test_env_t* env = calloc(1, sizeof(*env));
    if(env == NULL) return -1;

    memcpy(env->root_path, "/tmp/fsutil-ut-XXXXXX", sizeof("/tmp/fsutil-ut-XXXXXX"));
    if(mkdtemp(env->root_path) == NULL)
    {
        free(env);
        return -1;
    }

    fs_dir_init(&env->root);
    env->root.fd = open(env->root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(env->root.fd < 0)
    {
        (void)rmdir(env->root_path);
        free(env);
        return -1;
    }

#ifdef FS_UTIL_TESTING
    fs_test_hooks_reset();
    g_fail_on_call     = 0;
    g_fail_errno       = 0;
    g_call_count       = 0;
    g_fcntl_cmd_filter = -1;
#endif

    *state = env;
    return 0;
}

static int env_teardown(void** state)
{
    test_env_t* env = *state;
    if(env == NULL) return 0;

#ifdef FS_UTIL_TESTING
    fs_test_hooks_reset();
#endif
    (void)umask(022);

    if(env->root.fd >= 0)
    {
        (void)remove_tree_at(env->root.fd);
        fs_dir_close(&env->root);
    }
    (void)rmdir(env->root_path);
    free(env);
    return 0;
}

static int remove_tree_at(int dir_fd)
{
    int  rc      = 0;
    int  scan_fd = dup(dir_fd);
    DIR* dir     = NULL;

    if(scan_fd < 0) return -1;
    dir = fdopendir(scan_fd);
    if(dir == NULL)
    {
        (void)close(scan_fd);
        return -1;
    }
    rewinddir(dir);

    for(;;)
    {
        struct dirent* entry = readdir(dir);
        if(entry == NULL) break;
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        struct stat st;
        if(fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        {
            rc = -1;
            break;
        }
        if(S_ISDIR(st.st_mode))
        {
            int chmod_rc = fchmodat(dir_fd, entry->d_name, 0700, 0); /* best effort: locked dirs from tests */
            (void)chmod_rc;
            int child_fd = openat(dir_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if(child_fd < 0 || remove_tree_at(child_fd) != 0 || unlinkat(dir_fd, entry->d_name, AT_REMOVEDIR) != 0)
            {
                if(child_fd >= 0) (void)close(child_fd);
                rc = -1;
                break;
            }
            (void)close(child_fd);
        }
        else if(unlinkat(dir_fd, entry->d_name, 0) != 0)
        {
            rc = -1;
            break;
        }
    }
    (void)closedir(dir);
    return rc;
}

static void make_dir(const fs_dir_t* parent, const char* name, mode_t mode)
{
    assert_int_equal(mkdirat(parent->fd, name, mode), 0);
    assert_int_equal(fchmodat(parent->fd, name, mode, 0), 0);
}

static void make_file(const fs_dir_t* parent, const char* name, mode_t mode)
{
    int fd = openat(parent->fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    assert_true(fd >= 0);
    assert_int_equal(fchmod(fd, mode), 0);
    assert_int_equal(write(fd, "x", 1), 1);
    assert_int_equal(close(fd), 0);
}

static void make_symlink(const fs_dir_t* parent, const char* name, const char* target)
{
    assert_int_equal(symlinkat(target, parent->fd, name), 0);
}

static int entry_exists(const fs_dir_t* parent, const char* name)
{
    struct stat st;
    return fstatat(parent->fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
}

static void assert_same_identity_fd(int fd_a, int fd_b)
{
    struct stat a;
    struct stat b;
    assert_int_equal(fstat(fd_a, &a), 0);
    assert_int_equal(fstat(fd_b, &b), 0);
    assert_true(a.st_dev == b.st_dev);
    assert_true(a.st_ino == b.st_ino);
}

static void assert_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    assert_true(flags >= 0);
    assert_true((flags & FD_CLOEXEC) != 0);
}

static void assert_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    assert_true(flags >= 0);
    assert_true((flags & O_NONBLOCK) == 0);
}

/* A symlink where a DIRECTORY is required is refused by openat(O_DIRECTORY|O_NOFOLLOW):
 * current kernels say -ENOTDIR ("the link is not a directory"), older ones -ELOOP. Both
 * mean the link was never followed, which is the property under test. */
static void assert_dir_symlink_rejected(int rc)
{
    assert_true(rc == -ENOTDIR || rc == -ELOOP);
}

/*****************************************************************************************************************************************
 * PRIVATE FUNCTIONS DEFINITIONS — fault-injection hooks (test builds only)
 *****************************************************************************************************************************************
 */

#ifdef FS_UTIL_TESTING
static int counted_failure(void)
{
    if(g_fail_on_call > 0 && ++g_call_count == g_fail_on_call)
    {
        errno = g_fail_errno;
        return 1;
    }
    return 0;
}

static int hook_open_fail(const char* path, int flags)
{
    if(counted_failure()) return -1;
    return open(path, flags);
}
static int hook_openat_fail(int dirfd, const char* name, int flags, mode_t mode)
{
    if(counted_failure()) return -1;
    return openat(dirfd, name, flags, mode);
}
static int hook_mkdirat_fail(int dirfd, const char* name, mode_t mode)
{
    if(counted_failure()) return -1;
    return mkdirat(dirfd, name, mode);
}
static int hook_fchmod_fail(int fd, mode_t mode)
{
    if(counted_failure()) return -1;
    return fchmod(fd, mode);
}
static int hook_fstat_fail(int fd, struct stat* st)
{
    if(counted_failure()) return -1;
    return fstat(fd, st);
}
static int hook_fcntl_fail(int fd, int cmd, int arg)
{
    if((g_fcntl_cmd_filter < 0 || cmd == g_fcntl_cmd_filter) && counted_failure()) return -1;
    return fcntl(fd, cmd, arg);
}
static int hook_fcntl_getfl_without_nonblock(int fd, int cmd, int arg)
{
    if(cmd == F_GETFL) return O_RDONLY; /* pretend the kernel never set O_NONBLOCK */
    return fcntl(fd, cmd, arg);
}
static int hook_fsync_fail(int fd)
{
    (void)fd;
    errno = g_fail_errno;
    return -1;
}
static int hook_renameat_fail(int ofd, const char* oname, int nfd, const char* nname)
{
    (void)ofd;
    (void)oname;
    (void)nfd;
    (void)nname;
    errno = g_fail_errno;
    return -1;
}
static int hook_renameat2_fail(int ofd, const char* oname, int nfd, const char* nname, unsigned int flags)
{
    (void)ofd;
    (void)oname;
    (void)nfd;
    (void)nname;
    (void)flags;
    errno = g_fail_errno;
    return -1;
}
static int hook_unlinkat_clobbers_errno(int dirfd, const char* name, int flags)
{
    int rc = unlinkat(dirfd, name, flags);
    errno  = ENOENT; /* a cleanup that fails must not leak its errno into the caller's result */
    return rc;
}

static void arm_failure(int nth_call, int err)
{
    g_fail_on_call     = nth_call;
    g_fail_errno       = err;
    g_call_count       = 0;
    g_fcntl_cmd_filter = -1;
}
#endif

/*****************************************************************************************************************************************
 * TESTS — construction, handles, components
 *****************************************************************************************************************************************
 */

static void test_expect_constructors(void** state)
{
    (void)state;
    fs_expect_t e = fs_expect_make(0640, 7, 9);
    assert_int_equal(e.mode, 0640);
    assert_int_equal(e.uid, 7);
    assert_int_equal(e.gid, 9);

    fs_expect_t p = fs_expect_private(0700);
    assert_int_equal(p.mode, 0700);
    assert_int_equal(p.uid, geteuid());
    assert_int_equal(p.gid, getegid());
}

static void test_dir_init_and_close(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    d;

    fs_dir_init(NULL);
    fs_dir_close(NULL);

    d.fd = 12345;
    fs_dir_init(&d);
    assert_int_equal(d.fd, -1);

    fs_dir_close(&d); /* closed handle: no-op */
    assert_int_equal(d.fd, -1);

    d.fd = dup(env->root.fd);
    assert_true(d.fd >= 0);
    fs_dir_close(&d);
    assert_int_equal(d.fd, -1);
}

static void test_component_is_valid(void** state)
{
    (void)state;
    assert_int_equal(fs_component_is_valid(NULL), 0);
    assert_int_equal(fs_component_is_valid(""), 0);
    assert_int_equal(fs_component_is_valid("."), 0);
    assert_int_equal(fs_component_is_valid(".."), 0);
    assert_int_equal(fs_component_is_valid("a/b"), 0);
    assert_int_equal(fs_component_is_valid("/"), 0);
    assert_int_equal(fs_component_is_valid("..."), 1);
    assert_int_equal(fs_component_is_valid(".hidden"), 1);
    assert_int_equal(fs_component_is_valid("name"), 1);
}

/*****************************************************************************************************************************************
 * TESTS — root capabilities
 *****************************************************************************************************************************************
 */

static void test_open_cwd(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    d;
    fs_dir_init(&d);

    assert_int_equal(fs_dir_open_cwd(NULL), -EINVAL);
    d.fd = 0;
    assert_int_equal(fs_dir_open_cwd(&d), -EBUSY);
    d.fd = -1;

    assert_int_equal(fchdir(env->root.fd), 0);
    assert_int_equal(fs_dir_open_cwd(&d), 0);
    assert_cloexec(d.fd);
    assert_same_identity_fd(d.fd, env->root.fd);
    fs_dir_close(&d);
    assert_int_equal(chdir("/"), 0);
}

static void test_open_abs_nofollow_contract(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    d;
    char        path[2 * PATH_MAX];
    fs_dir_init(&d);

    assert_int_equal(fs_dir_open_abs_nofollow("/", NULL, NULL), -EINVAL);
    assert_int_equal(fs_dir_open_abs_nofollow(NULL, NULL, &d), -EINVAL);
    assert_int_equal(fs_dir_open_abs_nofollow("", NULL, &d), -EINVAL);
    assert_int_equal(fs_dir_open_abs_nofollow("relative", NULL, &d), -EINVAL);
    d.fd = 0;
    assert_int_equal(fs_dir_open_abs_nofollow("/", NULL, &d), -EBUSY);
    d.fd = -1;

    /* "/" and "/./" resolve to the root directory itself. */
    int slash_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert_true(slash_fd >= 0);
    assert_int_equal(fs_dir_open_abs_nofollow("/", NULL, &d), 0);
    assert_same_identity_fd(d.fd, slash_fd);
    assert_cloexec(d.fd);
    fs_dir_close(&d);
    assert_int_equal(fs_dir_open_abs_nofollow("/./", NULL, &d), 0);
    assert_same_identity_fd(d.fd, slash_fd);
    fs_dir_close(&d);
    (void)close(slash_fd);

    /* A real multi-component path, with '/' runs and "." noise. */
    make_dir(&env->root, "a", 0700);
    snprintf(path, sizeof(path), "%s//a/./", env->root_path);
    assert_int_equal(fs_dir_open_abs_nofollow(path, NULL, &d), 0);
    int a_fd = openat(env->root.fd, "a", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert_same_identity_fd(d.fd, a_fd);
    (void)close(a_fd);
    fs_dir_close(&d);

    /* The expectation applies to the final directory only: the parent chain is 0755/1777. */
    fs_expect_t want = fs_expect_private(0700);
    snprintf(path, sizeof(path), "%s/a", env->root_path);
    assert_int_equal(fs_dir_open_abs_nofollow(path, &want, &d), 0);
    fs_dir_close(&d);
    fs_expect_t wrong = fs_expect_private(0750);
    assert_int_equal(fs_dir_open_abs_nofollow(path, &wrong, &d), -EACCES);
    assert_int_equal(d.fd, -1);

    /* ".." anywhere is refused, even where it would resolve inside the tree. */
    snprintf(path, sizeof(path), "%s/a/../a", env->root_path);
    assert_int_equal(fs_dir_open_abs_nofollow(path, NULL, &d), -EINVAL);

    /* Missing leaf. */
    snprintf(path, sizeof(path), "%s/missing", env->root_path);
    assert_int_equal(fs_dir_open_abs_nofollow(path, NULL, &d), -ENOENT);

    /* Regular file where a directory is expected. */
    make_file(&env->root, "f", 0600);
    snprintf(path, sizeof(path), "%s/f", env->root_path);
    assert_int_equal(fs_dir_open_abs_nofollow(path, NULL, &d), -ENOTDIR);

    /* Component longer than NAME_MAX. */
    char longname[NAME_MAX + 8];
    memset(longname, 'z', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    snprintf(path, sizeof(path), "%s/%s", env->root_path, longname);
    assert_int_equal(fs_dir_open_abs_nofollow(path, NULL, &d), -ENAMETOOLONG);
    assert_int_equal(d.fd, -1);
}

static void test_open_abs_nofollow_rejects_symlink_anywhere(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    d;
    char        path[2 * PATH_MAX];
    fs_dir_init(&d);

    make_dir(&env->root, "real", 0700);
    make_dir(&env->root, "real/leaf", 0700);
    make_symlink(&env->root, "link", "real");

    /* The permissive opener follows the symlink in the middle of the chain ... */
    snprintf(path, sizeof(path), "%s/link/leaf", env->root_path);
    assert_int_equal(fs_dir_open_abs(path, NULL, &d), 0);
    fs_dir_close(&d);

    /* ... the strict one refuses it, and refuses a symlink at the end too. */
    assert_dir_symlink_rejected(fs_dir_open_abs_nofollow(path, NULL, &d));
    assert_int_equal(d.fd, -1);
    snprintf(path, sizeof(path), "%s/link", env->root_path);
    assert_dir_symlink_rejected(fs_dir_open_abs_nofollow(path, NULL, &d));
    assert_dir_symlink_rejected(fs_dir_open_abs(path, NULL, &d));
}

/*****************************************************************************************************************************************
 * TESTS — verification
 *****************************************************************************************************************************************
 */

static void test_dir_verify(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    d;

    assert_int_equal(fs_dir_verify(NULL, NULL), -EINVAL);
    fs_dir_init(&d);
    assert_int_equal(fs_dir_verify(&d, NULL), -EINVAL);

    /* A directory fd without FD_CLOEXEC is not a capability. */
    d.fd = open(env->root_path, O_RDONLY | O_DIRECTORY);
    assert_true(d.fd >= 0);
    assert_int_equal(fs_dir_verify(&d, NULL), -EINVAL);
    fs_dir_close(&d);

    /* A regular file is not a directory. */
    make_file(&env->root, "f", 0600);
    d.fd = openat(env->root.fd, "f", O_RDONLY | O_CLOEXEC);
    assert_true(d.fd >= 0);
    assert_int_equal(fs_dir_verify(&d, NULL), -ENOTDIR);
    fs_dir_close(&d);

    /* Metadata expectations: exact, individually ignorable, validated. */
    make_dir(&env->root, "d", 0750);
    d.fd = openat(env->root.fd, "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert_true(d.fd >= 0);
    fs_expect_t ok = fs_expect_private(0750);
    assert_int_equal(fs_dir_verify(&d, &ok), 0);
    fs_expect_t any = fs_expect_make(FS_EXPECT_ANY_MODE, FS_EXPECT_ANY_UID, FS_EXPECT_ANY_GID);
    assert_int_equal(fs_dir_verify(&d, &any), 0);
    fs_expect_t bad_mode = fs_expect_make(S_IFDIR | 0750, FS_EXPECT_ANY_UID, FS_EXPECT_ANY_GID);
    assert_int_equal(fs_dir_verify(&d, &bad_mode), -EINVAL);
    fs_expect_t wrong_mode = fs_expect_make(0700, FS_EXPECT_ANY_UID, FS_EXPECT_ANY_GID);
    assert_int_equal(fs_dir_verify(&d, &wrong_mode), -EACCES);
    fs_expect_t wrong_uid = fs_expect_make(FS_EXPECT_ANY_MODE, geteuid() + 1, FS_EXPECT_ANY_GID);
    assert_int_equal(fs_dir_verify(&d, &wrong_uid), -EACCES);
    fs_expect_t wrong_gid = fs_expect_make(FS_EXPECT_ANY_MODE, FS_EXPECT_ANY_UID, getegid() + 1);
    assert_int_equal(fs_dir_verify(&d, &wrong_gid), -EACCES);
    fs_dir_close(&d);
}

static void test_file_verify(void** state)
{
    test_env_t* env = *state;

    assert_int_equal(fs_file_verify(-1, NULL), -EINVAL);

    /* A directory is reported distinctly from other non-regular objects. */
    assert_int_equal(fs_file_verify(env->root.fd, NULL), -EISDIR);

    /* A FIFO is "not a regular file". */
    assert_int_equal(mkfifoat(env->root.fd, "fifo", 0600), 0);
    int fifo_fd = openat(env->root.fd, "fifo", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    assert_true(fifo_fd >= 0);
    assert_int_equal(fs_file_verify(fifo_fd, NULL), -EINVAL);
    (void)close(fifo_fd);

    /* Missing FD_CLOEXEC is refused before anything else is looked at. */
    make_file(&env->root, "f", 0640);
    int plain_fd = openat(env->root.fd, "f", O_RDONLY);
    assert_true(plain_fd >= 0);
    assert_int_equal(fs_file_verify(plain_fd, NULL), -EINVAL);
    (void)close(plain_fd);

    int fd = openat(env->root.fd, "f", O_RDONLY | O_CLOEXEC);
    assert_true(fd >= 0);
    assert_int_equal(fs_file_verify(fd, NULL), 0);
    fs_expect_t ok = fs_expect_private(0640);
    assert_int_equal(fs_file_verify(fd, &ok), 0);
    fs_expect_t wrong = fs_expect_private(0600);
    assert_int_equal(fs_file_verify(fd, &wrong), -EACCES);
    (void)close(fd);
}

/*****************************************************************************************************************************************
 * TESTS — single-component directory helpers
 *****************************************************************************************************************************************
 */

static void test_dir_open_at(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    d;
    fs_dir_t    closed;
    fs_dir_init(&d);
    fs_dir_init(&closed);

    make_dir(&env->root, "d", 0700);

    assert_int_equal(fs_dir_open_at(NULL, "d", NULL, &d), -EINVAL);
    assert_int_equal(fs_dir_open_at(&closed, "d", NULL, &d), -EINVAL);
    assert_int_equal(fs_dir_open_at(&env->root, "d", NULL, NULL), -EINVAL);
    assert_int_equal(fs_dir_open_at(&env->root, "a/b", NULL, &d), -EINVAL);
    d.fd = 0;
    assert_int_equal(fs_dir_open_at(&env->root, "d", NULL, &d), -EBUSY);
    d.fd = -1;

    /* The parent itself is verified: a file fd wearing a capability struct is refused. */
    make_file(&env->root, "f", 0600);
    fs_dir_t not_a_dir = {.fd = openat(env->root.fd, "f", O_RDONLY | O_CLOEXEC)};
    assert_true(not_a_dir.fd >= 0);
    assert_int_equal(fs_dir_open_at(&not_a_dir, "d", NULL, &d), -ENOTDIR);
    fs_dir_close(&not_a_dir);

    assert_int_equal(fs_dir_open_at(&env->root, "missing", NULL, &d), -ENOENT);
    assert_int_equal(fs_dir_open_at(&env->root, "f", NULL, &d), -ENOTDIR);
    make_symlink(&env->root, "l", "d");
    assert_dir_symlink_rejected(fs_dir_open_at(&env->root, "l", NULL, &d));
    fs_expect_t wrong = fs_expect_private(0755);
    assert_int_equal(fs_dir_open_at(&env->root, "d", &wrong, &d), -EACCES);
    assert_int_equal(d.fd, -1);

    fs_expect_t ok = fs_expect_private(0700);
    assert_int_equal(fs_dir_open_at(&env->root, "d", &ok, &d), 0);
    assert_cloexec(d.fd);
    fs_dir_close(&d);
}

static void test_dir_create_at_contract(void** state)
{
    test_env_t*                env = *state;
    fs_dir_t                   d;
    fs_dir_t                   closed;
    enum fs_create_disposition disp = FS_CREATE_DISPOSITION_CREATED_NEW;
    fs_expect_t                want = fs_expect_private(0700);
    fs_dir_init(&d);
    fs_dir_init(&closed);

    assert_int_equal(fs_dir_create_at(NULL, "d", 0700, &want, &d, &disp), -EINVAL);
    assert_int_equal(fs_dir_create_at(&closed, "d", 0700, &want, &d, &disp), -EINVAL);
    assert_int_equal(fs_dir_create_at(&env->root, "d", 0700, &want, NULL, &disp), -EINVAL);
    assert_int_equal(fs_dir_create_at(&env->root, "..", 0700, &want, &d, &disp), -EINVAL);
    assert_int_equal(fs_dir_create_at(&env->root, "d", S_IFDIR | 0700, &want, &d, &disp), -EINVAL);
    d.fd = 0;
    assert_int_equal(fs_dir_create_at(&env->root, "d", 0700, &want, &d, &disp), -EBUSY);
    d.fd = -1;

    /* Parent verify failure. */
    make_file(&env->root, "f", 0600);
    fs_dir_t not_a_dir = {.fd = openat(env->root.fd, "f", O_RDONLY | O_CLOEXEC)};
    assert_int_equal(fs_dir_create_at(&not_a_dir, "d", 0700, &want, &d, &disp), -ENOTDIR);
    fs_dir_close(&not_a_dir);

    /* Create new: exact mode regardless of umask, disposition reported. */
    (void)umask(077);
    assert_int_equal(fs_dir_create_at(&env->root, "d", 0750, NULL, &d, &disp), 0);
    assert_int_equal(disp, FS_CREATE_DISPOSITION_CREATED_NEW);
    fs_expect_t exact = fs_expect_private(0750);
    assert_int_equal(fs_dir_verify(&d, &exact), 0);
    fs_dir_close(&d);
    (void)umask(022);

    /* Open existing: disposition flips, expectation still enforced, NULL disposition accepted. */
    assert_int_equal(fs_dir_create_at(&env->root, "d", 0750, &exact, &d, &disp), 0);
    assert_int_equal(disp, FS_CREATE_DISPOSITION_OPENED_EXISTING);
    fs_dir_close(&d);
    assert_int_equal(fs_dir_create_at(&env->root, "d", 0750, &exact, &d, NULL), 0);
    fs_dir_close(&d);
    fs_expect_t wrong = fs_expect_private(0700);
    assert_int_equal(fs_dir_create_at(&env->root, "d", 0750, &wrong, &d, &disp), -EACCES);
    assert_true(entry_exists(&env->root, "d")); /* an existing directory is never cleaned up */

    /* Existing regular file / symlink in the way. */
    assert_int_equal(fs_dir_create_at(&env->root, "f", 0700, NULL, &d, &disp), -ENOTDIR);
    make_symlink(&env->root, "l", "d");
    assert_dir_symlink_rejected(fs_dir_create_at(&env->root, "l", 0700, NULL, &d, &disp));

    /* A brand-new directory that fails the caller's expectation is removed again. */
    fs_expect_t not_me = fs_expect_make(0700, geteuid() + 1, FS_EXPECT_ANY_GID);
    assert_int_equal(fs_dir_create_at(&env->root, "fresh", 0700, &not_me, &d, &disp), -EACCES);
    assert_false(entry_exists(&env->root, "fresh"));
    assert_int_equal(d.fd, -1);
    assert_int_equal(disp, FS_CREATE_DISPOSITION_OPENED_EXISTING); /* output reset on failure */

    /* mkdirat failing for a reason other than EEXIST propagates (read-only parent). */
    make_dir(&env->root, "ro", 0500);
    fs_dir_t ro;
    fs_dir_init(&ro);
    assert_int_equal(fs_dir_open_at(&env->root, "ro", NULL, &ro), 0);
    if(geteuid() != 0)
    {
        assert_int_equal(fs_dir_create_at(&ro, "child", 0700, NULL, &d, &disp), -EACCES);
    }
    fs_dir_close(&ro);
}

/*****************************************************************************************************************************************
 * TESTS — walks
 *****************************************************************************************************************************************
 */

static void test_walk_open(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    d;
    fs_dir_t    closed;
    fs_dir_init(&d);
    fs_dir_init(&closed);

    make_dir(&env->root, "a", 0700);
    make_dir(&env->root, "a/b", 0700);
    make_dir(&env->root, "a/b/c", 0700);

    assert_int_equal(fs_dir_walk_open(NULL, "a", NULL, &d), -EINVAL);
    assert_int_equal(fs_dir_walk_open(&closed, "a", NULL, &d), -EINVAL);
    assert_int_equal(fs_dir_walk_open(&env->root, NULL, NULL, &d), -EINVAL);
    assert_int_equal(fs_dir_walk_open(&env->root, "a", NULL, NULL), -EINVAL);
    assert_int_equal(fs_dir_walk_open(&env->root, "/a", NULL, &d), -EINVAL);
    d.fd = 0;
    assert_int_equal(fs_dir_walk_open(&env->root, "a", NULL, &d), -EBUSY);
    d.fd = -1;

    make_file(&env->root, "f", 0600);
    fs_dir_t not_a_dir = {.fd = openat(env->root.fd, "f", O_RDONLY | O_CLOEXEC)};
    assert_int_equal(fs_dir_walk_open(&not_a_dir, "a", NULL, &d), -ENOTDIR);
    fs_dir_close(&not_a_dir);

    /* Multi-component, with '/' runs and "." noise, lands on the right inode. */
    int c_fd = openat(env->root.fd, "a/b/c", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert_int_equal(fs_dir_walk_open(&env->root, ".//a/./b//c/", NULL, &d), 0);
    assert_same_identity_fd(d.fd, c_fd);
    assert_cloexec(d.fd);
    fs_dir_close(&d);
    (void)close(c_fd);

    /* Empty and "."-only paths hand back a DUPLICATE of the start capability. */
    const char* self_paths[] = {"", ".", "./", "././"};
    for(size_t i = 0; i < sizeof(self_paths) / sizeof(self_paths[0]); ++i)
    {
        assert_int_equal(fs_dir_walk_open(&env->root, self_paths[i], NULL, &d), 0);
        assert_true(d.fd != env->root.fd);
        assert_same_identity_fd(d.fd, env->root.fd);
        assert_cloexec(d.fd);
        fs_dir_close(&d);
    }
    /* ... and the expectation is applied to that start capability. */
    fs_expect_t wrong = fs_expect_private(0000);
    assert_int_equal(fs_dir_walk_open(&env->root, ".", &wrong, &d), -EACCES);

    /* ".." is refused at the first position and after a component (transient fd released). */
    assert_int_equal(fs_dir_walk_open(&env->root, "..", NULL, &d), -EINVAL);
    assert_int_equal(fs_dir_walk_open(&env->root, "a/../a", NULL, &d), -EINVAL);
    assert_int_equal(d.fd, -1);

    /* Missing, not-a-directory, symlink in the middle. */
    assert_int_equal(fs_dir_walk_open(&env->root, "a/missing", NULL, &d), -ENOENT);
    assert_int_equal(fs_dir_walk_open(&env->root, "f", NULL, &d), -ENOTDIR);
    make_symlink(&env->root, "a/l", "b");
    assert_dir_symlink_rejected(fs_dir_walk_open(&env->root, "a/l/c", NULL, &d));

    /* Expectation applied to EVERY walked component. */
    fs_expect_t ok = fs_expect_private(0700);
    assert_int_equal(fs_dir_walk_open(&env->root, "a/b/c", &ok, &d), 0);
    fs_dir_close(&d);
    assert_int_equal(fchmodat(env->root.fd, "a/b", 0750, 0), 0);
    assert_int_equal(fs_dir_walk_open(&env->root, "a/b/c", &ok, &d), -EACCES);
    assert_int_equal(d.fd, -1);

    /* Over-long component, at the first position and after a walked one. */
    char longpath[NAME_MAX + 16];
    memset(longpath, 'z', NAME_MAX + 1);
    longpath[NAME_MAX + 1] = '\0';
    assert_int_equal(fs_dir_walk_open(&env->root, longpath, NULL, &d), -ENAMETOOLONG);
    char nested[NAME_MAX + 32];
    snprintf(nested, sizeof(nested), "a/%s", longpath);
    assert_int_equal(fs_dir_walk_open(&env->root, nested, NULL, &d), -ENAMETOOLONG);
    assert_int_equal(d.fd, -1);
}

static void test_walk_create(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    d;
    fs_dir_t    closed;
    fs_expect_t ok = fs_expect_private(0700);
    fs_dir_init(&d);
    fs_dir_init(&closed);

    assert_int_equal(fs_dir_walk_create(NULL, "x", 0700, &ok, &d), -EINVAL);
    assert_int_equal(fs_dir_walk_create(&closed, "x", 0700, &ok, &d), -EINVAL);
    assert_int_equal(fs_dir_walk_create(&env->root, NULL, 0700, &ok, &d), -EINVAL);
    assert_int_equal(fs_dir_walk_create(&env->root, "x", 0700, &ok, NULL), -EINVAL);
    assert_int_equal(fs_dir_walk_create(&env->root, "/x", 0700, &ok, &d), -EINVAL);
    assert_int_equal(fs_dir_walk_create(&env->root, "x", S_IFREG | 0700, &ok, &d), -EINVAL);
    d.fd = 0;
    assert_int_equal(fs_dir_walk_create(&env->root, "x", 0700, &ok, &d), -EBUSY);
    d.fd = -1;

    make_file(&env->root, "f", 0600);
    fs_dir_t not_a_dir = {.fd = openat(env->root.fd, "f", O_RDONLY | O_CLOEXEC)};
    assert_int_equal(fs_dir_walk_create(&not_a_dir, "x", 0700, &ok, &d), -ENOTDIR);
    fs_dir_close(&not_a_dir);

    /* Creates the whole chain, each component with the exact mode; re-walk opens it. */
    (void)umask(077);
    assert_int_equal(fs_dir_walk_create(&env->root, "./x/y//z/", 0750, NULL, &d), 0);
    (void)umask(022);
    fs_dir_close(&d);
    fs_expect_t exact = fs_expect_private(0750);
    assert_int_equal(fs_dir_walk_open(&env->root, "x/y/z", &exact, &d), 0);
    fs_dir_close(&d);
    assert_int_equal(fs_dir_walk_create(&env->root, "x/y/z", 0750, &exact, &d), 0);
    fs_dir_close(&d);

    /* Self paths duplicate the start; expectation applies to it. */
    assert_int_equal(fs_dir_walk_create(&env->root, ".", 0700, NULL, &d), 0);
    assert_same_identity_fd(d.fd, env->root.fd);
    fs_dir_close(&d);
    assert_int_equal(fs_dir_walk_create(&env->root, "", 0700, &exact, &d), -EACCES);

    /* ".." after a component; a regular file in the way mid-walk. */
    assert_int_equal(fs_dir_walk_create(&env->root, "x/../x", 0700, NULL, &d), -EINVAL);
    assert_int_equal(fs_dir_walk_create(&env->root, "f/child", 0700, NULL, &d), -ENOTDIR);
    assert_int_equal(d.fd, -1);
    assert_false(entry_exists(&env->root, "child"));
}

/*****************************************************************************************************************************************
 * TESTS — files
 *****************************************************************************************************************************************
 */

static void test_file_open_read_and_rw(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    closed;
    int         fd = 7;
    fs_dir_init(&closed);

    make_file(&env->root, "f", 0640);
    make_dir(&env->root, "d", 0700);
    make_symlink(&env->root, "l", "f");
    assert_int_equal(mkfifoat(env->root.fd, "fifo", 0600), 0);

    typedef int (*opener_fn)(const fs_dir_t*, const char*, const fs_expect_t*, int*);
    const opener_fn openers[] = {fs_file_open_read_at, fs_file_open_rw_at};
    for(size_t i = 0; i < 2; ++i)
    {
        opener_fn open_fn = openers[i];

        assert_int_equal(open_fn(&env->root, "f", NULL, NULL), -EINVAL);
        fd = 7;
        assert_int_equal(open_fn(NULL, "f", NULL, &fd), -EINVAL);
        assert_int_equal(fd, -1); /* output reset before any validation */
        fd = 7;
        assert_int_equal(open_fn(&closed, "f", NULL, &fd), -EINVAL);
        assert_int_equal(fd, -1);
        assert_int_equal(open_fn(&env->root, "a/b", NULL, &fd), -EINVAL);

        fs_dir_t not_a_dir = {.fd = openat(env->root.fd, "f", O_RDONLY | O_CLOEXEC)};
        assert_int_equal(open_fn(&not_a_dir, "f", NULL, &fd), -ENOTDIR);
        fs_dir_close(&not_a_dir);

        assert_int_equal(open_fn(&env->root, "missing", NULL, &fd), -ENOENT);
        assert_int_equal(open_fn(&env->root, "d", NULL, &fd), -EISDIR);
        assert_int_equal(open_fn(&env->root, "fifo", NULL, &fd), -EINVAL); /* did not block */
        assert_int_equal(open_fn(&env->root, "l", NULL, &fd), -ELOOP);
        fs_expect_t wrong = fs_expect_private(0600);
        assert_int_equal(open_fn(&env->root, "f", &wrong, &fd), -EACCES);
        assert_int_equal(fd, -1);

        fs_expect_t ok = fs_expect_private(0640);
        assert_int_equal(open_fn(&env->root, "f", &ok, &fd), 0);
        assert_true(fd >= 0);
        assert_cloexec(fd);
        assert_blocking(fd);
        char byte = 0;
        assert_int_equal(read(fd, &byte, 1), 1);
        assert_int_equal(byte, 'x');
        if(open_fn == fs_file_open_rw_at)
        {
            assert_int_equal(write(fd, "y", 1), 1);
        }
        else
        {
            assert_int_equal(write(fd, "y", 1), -1);
            assert_int_equal(errno, EBADF);
        }
        (void)close(fd);
    }
}

static void test_file_create_write_new_at(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    closed;
    int         fd = 7;
    fs_dir_init(&closed);

    assert_int_equal(fs_file_create_write_new_at(&env->root, "n", 0600, NULL, NULL), -EINVAL);
    assert_int_equal(fs_file_create_write_new_at(NULL, "n", 0600, NULL, &fd), -EINVAL);
    assert_int_equal(fd, -1);
    assert_int_equal(fs_file_create_write_new_at(&closed, "n", 0600, NULL, &fd), -EINVAL);
    assert_int_equal(fs_file_create_write_new_at(&env->root, ".", 0600, NULL, &fd), -EINVAL);
    assert_int_equal(fs_file_create_write_new_at(&env->root, "n", S_IFREG | 0600, NULL, &fd), -EINVAL);

    make_file(&env->root, "f", 0600);
    fs_dir_t not_a_dir = {.fd = openat(env->root.fd, "f", O_RDONLY | O_CLOEXEC)};
    assert_int_equal(fs_file_create_write_new_at(&not_a_dir, "n", 0600, NULL, &fd), -ENOTDIR);
    fs_dir_close(&not_a_dir);

    /* Brand new only: an existing file, and a dangling symlink, both refuse. */
    assert_int_equal(fs_file_create_write_new_at(&env->root, "f", 0600, NULL, &fd), -EEXIST);
    make_symlink(&env->root, "dangling", "nowhere");
    assert_int_equal(fs_file_create_write_new_at(&env->root, "dangling", 0600, NULL, &fd), -EEXIST);
    assert_false(entry_exists(&env->root, "nowhere"));

    /* Exact mode independent of umask; expectation applied; fd usable. */
    (void)umask(077);
    fs_expect_t exact = fs_expect_private(0644);
    assert_int_equal(fs_file_create_write_new_at(&env->root, "n", 0644, &exact, &fd), 0);
    (void)umask(022);
    assert_cloexec(fd);
    assert_int_equal(write(fd, "hi", 2), 2);
    assert_int_equal(fs_file_verify(fd, &exact), 0);
    (void)close(fd);

    /* A fresh file that fails the caller's expectation is unlinked again. */
    fs_expect_t not_me = fs_expect_make(0600, geteuid() + 1, FS_EXPECT_ANY_GID);
    assert_int_equal(fs_file_create_write_new_at(&env->root, "gone", 0600, &not_me, &fd), -EACCES);
    assert_false(entry_exists(&env->root, "gone"));
    assert_int_equal(fd, -1);
}

/*****************************************************************************************************************************************
 * TESTS — rename, unlink, fsync
 *****************************************************************************************************************************************
 */

static void test_rename_and_unlink(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    closed;
    fs_dir_t    sub;
    fs_dir_init(&closed);
    fs_dir_init(&sub);

    make_file(&env->root, "a", 0600);
    make_file(&env->root, "b", 0600);
    make_dir(&env->root, "sub", 0700);
    assert_int_equal(fs_dir_open_at(&env->root, "sub", NULL, &sub), 0);
    make_file(&env->root, "f", 0600);
    fs_dir_t not_a_dir = {.fd = openat(env->root.fd, "f", O_RDONLY | O_CLOEXEC)};

    /* fs_rename_at: argument and capability checks, replace semantics. */
    assert_int_equal(fs_rename_at(NULL, "a", &env->root, "b"), -EINVAL);
    assert_int_equal(fs_rename_at(&env->root, "a", &closed, "b"), -EINVAL);
    assert_int_equal(fs_rename_at(&closed, "a", &env->root, "b"), -EINVAL);
    assert_int_equal(fs_rename_at(&env->root, "a", NULL, "b"), -EINVAL);
    assert_int_equal(fs_rename_at(&env->root, "a/x", &env->root, "b"), -EINVAL);
    assert_int_equal(fs_rename_at(&env->root, "a", &env->root, ".."), -EINVAL);
    assert_int_equal(fs_rename_at(&not_a_dir, "a", &env->root, "b"), -ENOTDIR);
    assert_int_equal(fs_rename_at(&env->root, "a", &not_a_dir, "b"), -ENOTDIR);
    assert_int_equal(fs_rename_at(&env->root, "missing", &env->root, "b"), -ENOENT);
    assert_int_equal(fs_rename_at(&env->root, "a", &env->root, "b"), 0); /* replaces b */
    assert_false(entry_exists(&env->root, "a"));
    assert_true(entry_exists(&env->root, "b"));

    /* fs_rename_noreplace_at: same checks, but an existing destination is refused atomically. */
    make_file(&env->root, "c", 0600);
    assert_int_equal(fs_rename_noreplace_at(NULL, "c", &env->root, "b"), -EINVAL);
    assert_int_equal(fs_rename_noreplace_at(&env->root, "c", &closed, "b"), -EINVAL);
    assert_int_equal(fs_rename_noreplace_at(&closed, "c", &env->root, "b"), -EINVAL);
    assert_int_equal(fs_rename_noreplace_at(&env->root, "c", NULL, "b"), -EINVAL);
    assert_int_equal(fs_rename_noreplace_at(&env->root, "", &env->root, "b"), -EINVAL);
    assert_int_equal(fs_rename_noreplace_at(&env->root, "c", &env->root, "x/y"), -EINVAL);
    assert_int_equal(fs_rename_noreplace_at(&not_a_dir, "c", &env->root, "b"), -ENOTDIR);
    assert_int_equal(fs_rename_noreplace_at(&env->root, "c", &not_a_dir, "b"), -ENOTDIR);
    assert_int_equal(fs_rename_noreplace_at(&env->root, "c", &env->root, "b"), -EEXIST);
    assert_true(entry_exists(&env->root, "c")); /* nothing moved */
    assert_int_equal(fs_rename_noreplace_at(&env->root, "c", &sub, "moved"), 0);
    assert_false(entry_exists(&env->root, "c"));
    assert_true(entry_exists(&sub, "moved"));

    /* fs_unlink_at. */
    assert_int_equal(fs_unlink_at(NULL, "b"), -EINVAL);
    assert_int_equal(fs_unlink_at(&closed, "b"), -EINVAL);
    assert_int_equal(fs_unlink_at(&env->root, "."), -EINVAL);
    assert_int_equal(fs_unlink_at(&not_a_dir, "b"), -ENOTDIR);
    assert_int_equal(fs_unlink_at(&env->root, "missing"), -ENOENT);
    assert_int_equal(fs_unlink_at(&env->root, "sub"), -EISDIR);
    assert_int_equal(fs_unlink_at(&env->root, "b"), 0);
    assert_false(entry_exists(&env->root, "b"));

    fs_dir_close(&not_a_dir);
    fs_dir_close(&sub);
}

static void test_fsync_helpers(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    closed;
    fs_dir_init(&closed);

    assert_int_equal(fs_dir_fsync(NULL), -EINVAL);
    assert_int_equal(fs_dir_fsync(&closed), -EINVAL);
    assert_int_equal(fs_dir_fsync(&env->root), 0);

    make_file(&env->root, "f", 0600);
    int fd = openat(env->root.fd, "f", O_RDWR | O_CLOEXEC);
    assert_true(fd >= 0);
    assert_int_equal(fs_file_fsync(-1), -EINVAL);
    assert_int_equal(fs_file_fsync(env->root.fd), -EISDIR);
    assert_int_equal(fs_file_fsync(fd), 0);
    (void)close(fd);
}

/*****************************************************************************************************************************************
 * TESTS — fault injection (test builds only)
 *****************************************************************************************************************************************
 */

#ifdef FS_UTIL_TESTING
static void test_fault_root_opens(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    d;
    fs_dir_init(&d);

    fs_test_hooks.open = hook_open_fail;
    arm_failure(1, EACCES);
    assert_int_equal(fs_dir_open_cwd(&d), -EACCES);
    arm_failure(1, EIO);
    assert_int_equal(fs_dir_open_abs(env->root_path, NULL, &d), -EIO);
    arm_failure(1, EMFILE);
    assert_int_equal(fs_dir_open_abs_nofollow(env->root_path, NULL, &d), -EMFILE);
    assert_int_equal(d.fd, -1);
    fs_test_hooks_reset();

    /* Verification failing on an opened root closes the fd and reports the error. */
    fs_test_hooks.fstat = hook_fstat_fail;
    arm_failure(1, EIO);
    assert_int_equal(fs_dir_open_abs(env->root_path, NULL, &d), -EIO);
    /* nofollow: parent chain verifications pass, the final one fails. */
    arm_failure(1, EIO);
    assert_int_equal(fs_dir_open_abs_nofollow("/", NULL, &d), -EIO);
    assert_int_equal(d.fd, -1);
}

static void test_fault_verify(void** state)
{
    test_env_t* env = *state;

    fs_test_hooks.fcntl = hook_fcntl_fail;
    arm_failure(1, EBADF);
    assert_int_equal(fs_dir_verify(&env->root, NULL), -EBADF);

    make_file(&env->root, "f", 0600);
    int fd = openat(env->root.fd, "f", O_RDONLY | O_CLOEXEC);
    arm_failure(1, EBADF);
    assert_int_equal(fs_file_verify(fd, NULL), -EBADF);
    fs_test_hooks_reset();

    fs_test_hooks.fstat = hook_fstat_fail;
    arm_failure(1, EIO);
    assert_int_equal(fs_dir_verify(&env->root, NULL), -EIO);
    arm_failure(1, EIO);
    assert_int_equal(fs_file_verify(fd, NULL), -EIO);
    (void)close(fd);
}

static void test_fault_dir_create_at(void** state)
{
    test_env_t*                env = *state;
    fs_dir_t                   d;
    enum fs_create_disposition disp;
    fs_dir_init(&d);

    /* mkdirat fails for a non-EEXIST reason. */
    fs_test_hooks.mkdirat = hook_mkdirat_fail;
    arm_failure(1, ENOSPC);
    assert_int_equal(fs_dir_create_at(&env->root, "n", 0700, NULL, &d, &disp), -ENOSPC);
    assert_false(entry_exists(&env->root, "n"));
    fs_test_hooks_reset();

    /* The just-created directory cannot be opened: it is removed again. */
    fs_test_hooks.openat = hook_openat_fail;
    arm_failure(1, EIO);
    assert_int_equal(fs_dir_create_at(&env->root, "n", 0700, NULL, &d, &disp), -EIO);
    assert_false(entry_exists(&env->root, "n"));
    fs_test_hooks_reset();

    /* fchmod on the fresh directory fails: removed again, errno preserved past the cleanup. */
    fs_test_hooks.fchmod   = hook_fchmod_fail;
    fs_test_hooks.unlinkat = hook_unlinkat_clobbers_errno;
    arm_failure(1, EPERM);
    assert_int_equal(fs_dir_create_at(&env->root, "n", 0700, NULL, &d, &disp), -EPERM);
    assert_false(entry_exists(&env->root, "n"));
    fs_test_hooks_reset();

    /* Post-create verification fails (fstat #1 verifies the parent, #2 the new directory). */
    fs_test_hooks.fstat = hook_fstat_fail;
    arm_failure(2, EIO);
    assert_int_equal(fs_dir_create_at(&env->root, "n", 0700, NULL, &d, &disp), -EIO);
    assert_false(entry_exists(&env->root, "n"));
    fs_test_hooks_reset();

    /* Same failure while OPENING an existing directory leaves it in place. */
    make_dir(&env->root, "e", 0700);
    fs_test_hooks.fstat = hook_fstat_fail;
    arm_failure(2, EIO);
    assert_int_equal(fs_dir_create_at(&env->root, "e", 0700, NULL, &d, &disp), -EIO);
    assert_true(entry_exists(&env->root, "e"));
    fs_test_hooks_reset();
    assert_int_equal(d.fd, -1);
}

static void test_fault_walks(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    d;
    fs_dir_init(&d);

    make_dir(&env->root, "a", 0700);
    make_dir(&env->root, "a/b", 0700);

    /* Duplicating the start capability fails (self path). */
    fs_test_hooks.fcntl = hook_fcntl_fail;
    arm_failure(1, EMFILE);
    g_fcntl_cmd_filter = F_DUPFD_CLOEXEC;
    assert_int_equal(fs_dir_walk_open(&env->root, ".", NULL, &d), -EMFILE);
    arm_failure(1, EMFILE);
    g_fcntl_cmd_filter = F_DUPFD_CLOEXEC;
    assert_int_equal(fs_dir_walk_create(&env->root, "", 0700, NULL, &d), -EMFILE);
    fs_test_hooks_reset();

    /* Verification of the SECOND component fails: the first one's fd is released. */
    fs_test_hooks.fstat = hook_fstat_fail;
    arm_failure(3, EIO); /* #1 start, #2 "a", #3 "b" */
    assert_int_equal(fs_dir_walk_open(&env->root, "a/b", NULL, &d), -EIO);
    fs_test_hooks_reset();

    /* Creating the second component fails mid-walk. */
    fs_test_hooks.mkdirat = hook_mkdirat_fail;
    arm_failure(2, ENOSPC); /* #1 "a" (EEXIST path? no: hook counts calls, "a" exists -> real EEXIST) */
    assert_int_equal(fs_dir_walk_create(&env->root, "a/new", 0700, NULL, &d), -ENOSPC);
    assert_false(entry_exists(&env->root, "a/new"));
    fs_test_hooks_reset();
    assert_int_equal(d.fd, -1);
}

static void test_fault_file_opens(void** state)
{
    test_env_t* env = *state;
    int         fd  = 7;

    make_file(&env->root, "f", 0600);

    /* F_GETFL fails after a successful open+verify: fd is closed, error reported. */
    fs_test_hooks.fcntl = hook_fcntl_fail;
    arm_failure(1, EBADF);
    g_fcntl_cmd_filter = F_GETFL;
    assert_int_equal(fs_file_open_read_at(&env->root, "f", NULL, &fd), -EBADF);
    assert_int_equal(fd, -1);

    /* F_SETFL fails. */
    arm_failure(1, EBADF);
    g_fcntl_cmd_filter = F_SETFL;
    assert_int_equal(fs_file_open_rw_at(&env->root, "f", NULL, &fd), -EBADF);
    assert_int_equal(fd, -1);
    fs_test_hooks_reset();

    /* F_GETFL reports no O_NONBLOCK: the clear step is skipped and the open succeeds. */
    fs_test_hooks.fcntl = hook_fcntl_getfl_without_nonblock;
    assert_int_equal(fs_file_open_read_at(&env->root, "f", NULL, &fd), 0);
    assert_true(fd >= 0);
    (void)close(fd);
    fs_test_hooks_reset();

    /* Create: fchmod fails -> file removed, errno preserved past a cleanup that clobbers it. */
    fs_test_hooks.fchmod   = hook_fchmod_fail;
    fs_test_hooks.unlinkat = hook_unlinkat_clobbers_errno;
    arm_failure(1, EPERM);
    assert_int_equal(fs_file_create_write_new_at(&env->root, "n", 0600, NULL, &fd), -EPERM);
    assert_false(entry_exists(&env->root, "n"));
    fs_test_hooks_reset();

    /* Create: post-create verification fails (fstat #1 parent, #2 the new file). */
    fs_test_hooks.fstat = hook_fstat_fail;
    arm_failure(2, EIO);
    assert_int_equal(fs_file_create_write_new_at(&env->root, "n", 0600, NULL, &fd), -EIO);
    assert_false(entry_exists(&env->root, "n"));
    fs_test_hooks_reset();
    assert_int_equal(fd, -1);
}

static void test_fault_rename_unlink_fsync(void** state)
{
    test_env_t* env = *state;

    make_file(&env->root, "a", 0600);

    fs_test_hooks.renameat = hook_renameat_fail;
    g_fail_errno           = EXDEV;
    assert_int_equal(fs_rename_at(&env->root, "a", &env->root, "b"), -EXDEV);
    fs_test_hooks.renameat2 = hook_renameat2_fail;
    g_fail_errno            = EINVAL; /* filesystem without RENAME_NOREPLACE support */
    assert_int_equal(fs_rename_noreplace_at(&env->root, "a", &env->root, "b"), -EINVAL);
    fs_test_hooks_reset();
    assert_true(entry_exists(&env->root, "a"));

    /* Directory fsync: EINVAL is "unsupported here" and counts as success, anything else fails. */
    fs_test_hooks.fsync = hook_fsync_fail;
    g_fail_errno        = EINVAL;
    assert_int_equal(fs_dir_fsync(&env->root), 0);
    g_fail_errno = EIO;
    assert_int_equal(fs_dir_fsync(&env->root), -EIO);

    /* File fsync never masks. */
    int fd = openat(env->root.fd, "a", O_RDWR | O_CLOEXEC);
    assert_true(fd >= 0);
    g_fail_errno = EIO;
    assert_int_equal(fs_file_fsync(fd), -EIO);
    fs_test_hooks_reset();
    (void)close(fd);
}
#endif /* FS_UTIL_TESTING */

/*****************************************************************************************************************************************
 * PUBLIC FUNCTIONS DEFINITIONS
 *****************************************************************************************************************************************
 */

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_expect_constructors, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_dir_init_and_close, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_component_is_valid, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_open_cwd, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_open_abs_nofollow_contract, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_open_abs_nofollow_rejects_symlink_anywhere, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_dir_verify, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_file_verify, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_dir_open_at, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_dir_create_at_contract, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_walk_open, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_walk_create, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_file_open_read_and_rw, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_file_create_write_new_at, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_rename_and_unlink, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_fsync_helpers, env_setup, env_teardown),
#ifdef FS_UTIL_TESTING
        cmocka_unit_test_setup_teardown(test_fault_root_opens, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_fault_verify, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_fault_dir_create_at, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_fault_walks, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_fault_file_opens, env_setup, env_teardown),
        cmocka_unit_test_setup_teardown(test_fault_rename_unlink_fsync, env_setup, env_teardown),
#endif
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
