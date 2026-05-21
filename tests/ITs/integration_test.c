#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fsutil.h"

typedef struct test_env
{
    char root_path[PATH_MAX];
    int  old_cwd_fd;
} test_env_t;

static int  test_env_setup(void** state);
static int  test_env_teardown(void** state);
static int  remove_tree_at(int dir_fd);
static void open_root_cap(const test_env_t* env, fs_dir_t* out_dir);
static void create_raw_directory(const fs_dir_t* parent, const char* name, mode_t mode);
static void create_raw_file(const fs_dir_t* parent, const char* name, mode_t mode, const char* text);
static void assert_same_file_identity(int fd_a, int fd_b);

static void test_component_validation(void** state);
static void test_open_cwd_and_verify(void** state);
static void test_dir_create_open_walk_and_disposition(void** state);
static void test_walk_rejects_invalid_paths(void** state);
static void test_walk_duplicates_start_for_empty_path(void** state);
static void test_file_create_open_read_rename_and_unlink(void** state);
static void test_open_helpers_type_rejection_and_long_components(void** state);
static void test_symlink_rejection(void** state);
static void test_expectations_and_cloexec_enforcement(void** state);
static void test_create_cleanup_on_expectation_mismatch(void** state);
static void test_umask_independence_for_create_helpers(void** state);
static void test_output_contracts(void** state);

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_component_validation, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_open_cwd_and_verify, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_dir_create_open_walk_and_disposition, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_walk_rejects_invalid_paths, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_walk_duplicates_start_for_empty_path, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_file_create_open_read_rename_and_unlink, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_open_helpers_type_rejection_and_long_components, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_symlink_rejection, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_expectations_and_cloexec_enforcement, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_create_cleanup_on_expectation_mismatch, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_umask_independence_for_create_helpers, test_env_setup, test_env_teardown),
        cmocka_unit_test_setup_teardown(test_output_contracts, test_env_setup, test_env_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

static int test_env_setup(void** state)
{
    test_env_t* env = calloc(1, sizeof(*env));
    if(env == NULL) return -1;

    memcpy(env->root_path, "/tmp/fsutil-it-XXXXXX", sizeof("/tmp/fsutil-it-XXXXXX"));
    if(mkdtemp(env->root_path) == NULL)
    {
        free(env);
        return -1;
    }

    env->old_cwd_fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(env->old_cwd_fd < 0)
    {
        (void)rmdir(env->root_path);
        free(env);
        return -1;
    }

    *state = env;
    return 0;
}

static int test_env_teardown(void** state)
{
    test_env_t* env = *state;
    if(env == NULL) return 0;

    if(env->old_cwd_fd >= 0)
    {
        (void)fchdir(env->old_cwd_fd);
        (void)close(env->old_cwd_fd);
    }

    int root_fd = open(env->root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(root_fd >= 0)
    {
        (void)remove_tree_at(root_fd);
        (void)close(root_fd);
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

    for(;;)
    {
        struct dirent* entry = readdir(dir);
        if(entry == NULL) break;

        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        struct stat st;
        if(fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
        {
            rc = -1;
            break;
        }

        if(S_ISDIR(st.st_mode))
        {
            int child_fd = openat(dir_fd,
                                  entry->d_name,
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if(child_fd < 0)
            {
                rc = -1;
                break;
            }

            if(remove_tree_at(child_fd) != 0)
            {
                (void)close(child_fd);
                rc = -1;
                break;
            }

            (void)close(child_fd);
            if(unlinkat(dir_fd, entry->d_name, AT_REMOVEDIR) != 0)
            {
                rc = -1;
                break;
            }
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

static void open_root_cap(const test_env_t* env, fs_dir_t* out_dir)
{
    int fd;

    fs_dir_init(out_dir);
    fd = open(env->root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert_true(fd >= 0);
    out_dir->fd = fd;
}

static void create_raw_directory(const fs_dir_t* parent, const char* name, mode_t mode)
{
    assert_non_null(parent);
    assert_non_null(name);
    assert_true(mkdirat(parent->fd, name, mode) == 0);
}

static void create_raw_file(const fs_dir_t* parent, const char* name, mode_t mode, const char* text)
{
    int          fd;
    const size_t text_len = text == NULL ? 0U : strlen(text);

    assert_non_null(parent);
    assert_non_null(name);

    fd = openat(parent->fd, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    assert_true(fd >= 0);

    if(text_len > 0U)
    {
        ssize_t written = write(fd, text, text_len);
        assert_int_equal(written, (ssize_t)text_len);
    }

    assert_int_equal(close(fd), 0);
}

static void assert_same_file_identity(int fd_a, int fd_b)
{
    struct stat st_a;
    struct stat st_b;

    assert_true(fstat(fd_a, &st_a) == 0);
    assert_true(fstat(fd_b, &st_b) == 0);
    assert_int_equal(st_a.st_dev, st_b.st_dev);
    assert_int_equal(st_a.st_ino, st_b.st_ino);
}

static void test_component_validation(void** state)
{
    (void)state;

    assert_int_equal(fs_component_is_valid("alpha"), 1);
    assert_int_equal(fs_component_is_valid("alpha.beta"), 1);
    assert_int_equal(fs_component_is_valid(""), 0);
    assert_int_equal(fs_component_is_valid(NULL), 0);
    assert_int_equal(fs_component_is_valid("."), 0);
    assert_int_equal(fs_component_is_valid(".."), 0);
    assert_int_equal(fs_component_is_valid("alpha/beta"), 0);
}

static void test_open_cwd_and_verify(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    cwd;

    fs_dir_init(&cwd);

    assert_int_equal(chdir(env->root_path), 0);
    assert_int_equal(fs_dir_open_cwd(&cwd), 0);
    assert_int_equal(fs_dir_verify(&cwd, NULL), 0);

    fs_dir_close(&cwd);
}

static void test_dir_create_open_walk_and_disposition(void** state)
{
    test_env_t*                env = *state;
    fs_dir_t                   root;
    fs_dir_t                   alpha;
    fs_dir_t                   reopened;
    fs_dir_t                   nested;
    enum fs_create_disposition disposition;
    fs_expect_t                expect = fs_expect_private(0700);
    struct stat                st;

    open_root_cap(env, &root);
    fs_dir_init(&alpha);
    fs_dir_init(&reopened);
    fs_dir_init(&nested);

    assert_int_equal(fs_dir_create_at(&root, "alpha", 0700, &expect, &alpha, &disposition), 0);
    assert_int_equal(disposition, FS_CREATE_DISPOSITION_CREATED_NEW);

    assert_int_equal(fs_dir_create_at(&root, "alpha", 0700, &expect, &reopened, &disposition), 0);
    assert_int_equal(disposition, FS_CREATE_DISPOSITION_OPENED_EXISTING);

    assert_int_equal(fs_dir_walk_create(&root, "./alpha//beta/./gamma/", 0700, &expect, &nested), 0);
    assert_int_equal(fs_dir_fsync(&nested), 0);
    assert_true(fstat(nested.fd, &st) == 0);
    assert_true(S_ISDIR(st.st_mode));

    fs_dir_close(&nested);
    fs_dir_close(&reopened);
    fs_dir_close(&alpha);
    fs_dir_close(&root);
}

static void test_walk_rejects_invalid_paths(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    root;
    fs_dir_t    out_dir;

    open_root_cap(env, &root);
    fs_dir_init(&out_dir);
    create_raw_directory(&root, "alpha", 0700);

    assert_int_equal(fs_dir_walk_open(&root, "/absolute", NULL, &out_dir), -EINVAL);
    assert_int_equal(fs_dir_walk_open(&root, "alpha/../beta", NULL, &out_dir), -EINVAL);
    assert_int_equal(fs_dir_walk_create(&root, "../escape", 0700, NULL, &out_dir), -EINVAL);

    fs_dir_close(&out_dir);
    fs_dir_close(&root);
}

static void test_walk_duplicates_start_for_empty_path(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    root;
    fs_dir_t    duplicate;

    open_root_cap(env, &root);
    fs_dir_init(&duplicate);

    assert_int_equal(fs_dir_walk_open(&root, "", NULL, &duplicate), 0);
    assert_true(duplicate.fd >= 0);
    assert_false(duplicate.fd == root.fd);
    assert_same_file_identity(root.fd, duplicate.fd);
    fs_dir_close(&duplicate);

    assert_int_equal(fs_dir_walk_create(&root, "././", 0700, NULL, &duplicate), 0);
    assert_true(duplicate.fd >= 0);
    assert_false(duplicate.fd == root.fd);
    assert_same_file_identity(root.fd, duplicate.fd);

    fs_dir_close(&duplicate);
    fs_dir_close(&root);
}

static void test_file_create_open_read_rename_and_unlink(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    root;
    int         write_fd = -1;
    int         read_fd  = -1;
    char        buffer[64];
    ssize_t     nread;
    fs_expect_t expect = fs_expect_private(0640);

    memset(buffer, 0, sizeof(buffer));
    open_root_cap(env, &root);

    assert_int_equal(fs_file_create_write_new_at(&root, "payload.txt", 0640, &expect, &write_fd), 0);
    assert_true(write_fd >= 0);
    assert_int_equal(write(write_fd, "fsutil-data", strlen("fsutil-data")), (ssize_t)strlen("fsutil-data"));
    assert_int_equal(fs_file_fsync(write_fd), 0);
    assert_int_equal(close(write_fd), 0);
    write_fd = -1;

    assert_int_equal(fs_file_open_read_at(&root, "payload.txt", &expect, &read_fd), 0);
    nread = read(read_fd, buffer, sizeof(buffer));
    assert_true(nread >= 0);
    assert_memory_equal(buffer, "fsutil-data", strlen("fsutil-data"));
    assert_int_equal(close(read_fd), 0);
    read_fd = -1;

    assert_int_equal(fs_rename_at(&root, "payload.txt", &root, "payload-renamed.txt"), 0);
    assert_int_equal(fs_unlink_at(&root, "payload-renamed.txt"), 0);
    assert_int_equal(fs_file_open_read_at(&root, "payload-renamed.txt", NULL, &read_fd), -ENOENT);
    assert_int_equal(read_fd, -1);

    fs_dir_close(&root);
}

static void test_open_helpers_type_rejection_and_long_components(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    root;
    fs_dir_t    alpha;
    fs_dir_t    nested;
    int         read_fd = -1;
    int         rc;
    char        too_long_component[NAME_MAX + 2];
    char        long_walk_path[NAME_MAX + 16];
    fs_expect_t bad_expect = fs_expect_make((mode_t)S_IFDIR, FS_EXPECT_ANY_UID, FS_EXPECT_ANY_GID);

    memset(too_long_component, 'x', sizeof(too_long_component) - 1U);
    too_long_component[sizeof(too_long_component) - 1U] = '\0';

    open_root_cap(env, &root);
    fs_dir_init(&alpha);
    fs_dir_init(&nested);

    create_raw_directory(&root, "alpha", 0700);
    create_raw_directory(&root, "nested", 0700);
    assert_int_equal(fs_dir_open_at(&root, "nested", NULL, &nested), 0);
    create_raw_directory(&nested, "leaf", 0700);
    fs_dir_close(&nested);

    assert_int_equal(fs_dir_open_at(&root, "alpha", NULL, &alpha), 0);
    assert_int_equal(fs_dir_verify(&alpha, &bad_expect), -EINVAL);

    assert_int_equal(fs_dir_walk_open(&root, "nested/leaf", NULL, &nested), 0);
    assert_int_equal(fs_file_open_read_at(&root, "alpha", NULL, &read_fd), -EISDIR);
    assert_int_equal(read_fd, -1);
    assert_int_equal(fs_file_fsync(alpha.fd), -EISDIR);
    fs_dir_close(&nested);

    assert_true(snprintf(long_walk_path, sizeof(long_walk_path), "nested/%s", too_long_component) > 0);
    rc = fs_dir_walk_open(&root, long_walk_path, NULL, &nested);
    assert_int_equal(rc, -ENAMETOOLONG);

    rc = fs_dir_walk_create(&root, long_walk_path, 0700, NULL, &nested);
    assert_int_equal(rc, -ENAMETOOLONG);

    fs_dir_close(&nested);
    fs_dir_close(&alpha);
    fs_dir_close(&root);
}

static void test_symlink_rejection(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    root;
    fs_dir_t    dir_out;
    int         file_fd = -1;
    int         rc;

    open_root_cap(env, &root);
    fs_dir_init(&dir_out);

    create_raw_directory(&root, "real-dir", 0700);
    create_raw_file(&root, "real-file", 0600, "payload");

    assert_int_equal(symlinkat("real-dir", root.fd, "dir-link"), 0);
    assert_int_equal(symlinkat("real-file", root.fd, "file-link"), 0);

    rc = fs_dir_open_at(&root, "dir-link", NULL, &dir_out);
    assert_true(rc == -ELOOP || rc == -ENOTDIR);

    rc = fs_dir_create_at(&root, "dir-link", 0700, NULL, &dir_out, NULL);
    assert_true(rc == -ELOOP || rc == -ENOTDIR);

    rc = fs_file_open_read_at(&root, "file-link", NULL, &file_fd);
    assert_int_equal(rc, -ELOOP);
    assert_int_equal(file_fd, -1);

    fs_dir_close(&dir_out);
    fs_dir_close(&root);
}

static void test_expectations_and_cloexec_enforcement(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    root;
    fs_dir_t    non_cloexec_dir = { .fd = -1 };
    int         non_cloexec_file = -1;
    int         cloexec_file     = -1;
    fs_expect_t mismatch_expect  = fs_expect_make(0644, FS_EXPECT_ANY_UID, FS_EXPECT_ANY_GID);

    open_root_cap(env, &root);
    create_raw_directory(&root, "plain-dir", 0700);
    create_raw_file(&root, "plain-file", 0600, "plain");

    non_cloexec_dir.fd = openat(root.fd, "plain-dir", O_RDONLY | O_DIRECTORY);
    assert_true(non_cloexec_dir.fd >= 0);
    assert_int_equal(fs_dir_verify(&non_cloexec_dir, NULL), -EINVAL);

    non_cloexec_file = openat(root.fd, "plain-file", O_RDONLY);
    assert_true(non_cloexec_file >= 0);
    assert_int_equal(fs_file_verify(non_cloexec_file, NULL), -EINVAL);

    cloexec_file = openat(root.fd, "plain-file", O_RDONLY | O_CLOEXEC);
    assert_true(cloexec_file >= 0);
    assert_int_equal(fs_file_verify(cloexec_file, &mismatch_expect), -EACCES);

    assert_int_equal(close(cloexec_file), 0);
    assert_int_equal(close(non_cloexec_file), 0);
    fs_dir_close(&non_cloexec_dir);
    fs_dir_close(&root);
}

static void test_create_cleanup_on_expectation_mismatch(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    root;
    fs_dir_t    dir_out;
    int         file_fd = 123;
    int         rc;
    fs_expect_t dir_expect = fs_expect_make(0755, FS_EXPECT_ANY_UID, FS_EXPECT_ANY_GID);
    fs_expect_t file_expect = fs_expect_make(0644, FS_EXPECT_ANY_UID, FS_EXPECT_ANY_GID);

    open_root_cap(env, &root);
    fs_dir_init(&dir_out);

    rc = fs_dir_create_at(&root, "cleanup-dir", 0700, &dir_expect, &dir_out, NULL);
    assert_int_equal(rc, -EACCES);
    assert_int_equal(dir_out.fd, -1);
    assert_int_equal(faccessat(root.fd, "cleanup-dir", F_OK, AT_EACCESS), -1);
    assert_int_equal(errno, ENOENT);

    rc = fs_file_create_write_new_at(&root, "cleanup-file", 0600, &file_expect, &file_fd);
    assert_int_equal(rc, -EACCES);
    assert_int_equal(file_fd, -1);
    assert_int_equal(faccessat(root.fd, "cleanup-file", F_OK, AT_EACCESS), -1);
    assert_int_equal(errno, ENOENT);

    fs_dir_close(&dir_out);
    fs_dir_close(&root);
}

static void test_umask_independence_for_create_helpers(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    root;
    fs_dir_t    dir_out;
    int         file_fd = -1;
    mode_t      old_umask;
    struct stat st;
    fs_expect_t dir_expect  = fs_expect_private(0750);
    fs_expect_t file_expect = fs_expect_private(0640);

    open_root_cap(env, &root);
    fs_dir_init(&dir_out);

    old_umask = umask(0077);
    assert_int_equal(fs_dir_create_at(&root, "mode-dir", 0750, &dir_expect, &dir_out, NULL), 0);
    assert_int_equal(fs_file_create_write_new_at(&root, "mode-file", 0640, &file_expect, &file_fd), 0);
    (void)umask(old_umask);

    assert_true(fstat(dir_out.fd, &st) == 0);
    assert_int_equal(st.st_mode & FS_MODE_MASK, 0750);

    assert_true(fstat(file_fd, &st) == 0);
    assert_int_equal(st.st_mode & FS_MODE_MASK, 0640);

    assert_int_equal(close(file_fd), 0);
    fs_dir_close(&dir_out);
    fs_dir_close(&root);
}

static void test_output_contracts(void** state)
{
    test_env_t* env = *state;
    fs_dir_t    root;
    fs_dir_t    busy_dir;
    int         out_fd = 77;

    open_root_cap(env, &root);
    open_root_cap(env, &busy_dir);

    assert_int_equal(fs_file_open_read_at(&root, "missing.txt", NULL, &out_fd), -ENOENT);
    assert_int_equal(out_fd, -1);

    assert_int_equal(fs_dir_walk_open(&root, "", NULL, &busy_dir), -EBUSY);
    assert_int_equal(fs_dir_open_at(&root, "anything", NULL, &busy_dir), -EBUSY);

    fs_dir_close(&busy_dir);
    fs_dir_close(&root);
}
