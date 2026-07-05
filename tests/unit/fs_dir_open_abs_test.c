#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fsutil.h"

typedef struct unit_env
{
    char root_path[PATH_MAX];
    char child_path[PATH_MAX];
    char file_path[PATH_MAX];
    char link_path[PATH_MAX];
} unit_env_t;

static int  unit_env_setup(void** state);
static int  unit_env_teardown(void** state);
static void require_join(char* out, size_t out_size, const char* root, const char* leaf);
static void assert_same_directory_identity(const char* path, const fs_dir_t* dir);

static void test_open_abs_rejects_invalid_arguments(void** state);
static void test_open_abs_rejects_busy_output(void** state);
static void test_open_abs_opens_absolute_directory(void** state);
static void test_open_abs_applies_expectation(void** state);
static void test_open_abs_rejects_non_directory(void** state);
static void test_open_abs_rejects_final_symlink(void** state);

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_open_abs_rejects_invalid_arguments, unit_env_setup, unit_env_teardown),
        cmocka_unit_test_setup_teardown(test_open_abs_rejects_busy_output, unit_env_setup, unit_env_teardown),
        cmocka_unit_test_setup_teardown(test_open_abs_opens_absolute_directory, unit_env_setup, unit_env_teardown),
        cmocka_unit_test_setup_teardown(test_open_abs_applies_expectation, unit_env_setup, unit_env_teardown),
        cmocka_unit_test_setup_teardown(test_open_abs_rejects_non_directory, unit_env_setup, unit_env_teardown),
        cmocka_unit_test_setup_teardown(test_open_abs_rejects_final_symlink, unit_env_setup, unit_env_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}

static int unit_env_setup(void** state)
{
    unit_env_t* env = calloc(1, sizeof(*env));
    int         fd;

    if(env == NULL) return -1;

    memcpy(env->root_path, "/tmp/fsutil-unit-XXXXXX", sizeof("/tmp/fsutil-unit-XXXXXX"));
    if(mkdtemp(env->root_path) == NULL)
    {
        free(env);
        return -1;
    }

    require_join(env->child_path, sizeof(env->child_path), env->root_path, "child");
    require_join(env->file_path, sizeof(env->file_path), env->root_path, "regular-file");
    require_join(env->link_path, sizeof(env->link_path), env->root_path, "child-link");

    if(mkdir(env->child_path, 0700) != 0)
    {
        (void)rmdir(env->root_path);
        free(env);
        return -1;
    }

    fd = open(env->file_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if(fd < 0)
    {
        (void)rmdir(env->child_path);
        (void)rmdir(env->root_path);
        free(env);
        return -1;
    }
    if(close(fd) != 0)
    {
        (void)unlink(env->file_path);
        (void)rmdir(env->child_path);
        (void)rmdir(env->root_path);
        free(env);
        return -1;
    }

    if(symlink(env->child_path, env->link_path) != 0)
    {
        (void)unlink(env->file_path);
        (void)rmdir(env->child_path);
        (void)rmdir(env->root_path);
        free(env);
        return -1;
    }

    *state = env;
    return 0;
}

static int unit_env_teardown(void** state)
{
    unit_env_t* env = *state;

    if(env == NULL) return 0;

    (void)unlink(env->link_path);
    (void)unlink(env->file_path);
    (void)rmdir(env->child_path);
    (void)rmdir(env->root_path);
    free(env);
    return 0;
}

static void require_join(char* out, size_t out_size, const char* root, const char* leaf)
{
    size_t root_len;
    size_t leaf_len;

    assert_non_null(out);
    assert_non_null(root);
    assert_non_null(leaf);

    root_len = strlen(root);
    leaf_len = strlen(leaf);
    assert_true(root_len + 1u + leaf_len + 1u <= out_size);

    memcpy(out, root, root_len);
    out[root_len] = '/';
    memcpy(out + root_len + 1u, leaf, leaf_len + 1u);
}

static void assert_same_directory_identity(const char* path, const fs_dir_t* dir)
{
    struct stat by_path;
    struct stat by_fd;

    assert_non_null(path);
    assert_non_null(dir);
    assert_true(dir->fd >= 0);
    assert_int_equal(stat(path, &by_path), 0);
    assert_int_equal(fstat(dir->fd, &by_fd), 0);
    assert_int_equal(by_path.st_dev, by_fd.st_dev);
    assert_int_equal(by_path.st_ino, by_fd.st_ino);
}

static void test_open_abs_rejects_invalid_arguments(void** state)
{
    unit_env_t* env = *state;
    fs_dir_t    out;

    fs_dir_init(&out);

    assert_int_equal(fs_dir_open_abs(env->root_path, NULL, NULL), -EINVAL);
    assert_int_equal(fs_dir_open_abs(NULL, NULL, &out), -EINVAL);
    assert_int_equal(fs_dir_open_abs("", NULL, &out), -EINVAL);
    assert_int_equal(fs_dir_open_abs("relative/path", NULL, &out), -EINVAL);
    assert_int_equal(out.fd, -1);
}

static void test_open_abs_rejects_busy_output(void** state)
{
    unit_env_t* env = *state;
    fs_dir_t    out;
    int         original_fd;

    fs_dir_init(&out);
    assert_int_equal(fs_dir_open_abs(env->root_path, NULL, &out), 0);
    original_fd = out.fd;
    assert_true(original_fd >= 0);

    assert_int_equal(fs_dir_open_abs(env->child_path, NULL, &out), -EBUSY);
    assert_int_equal(out.fd, original_fd);

    fs_dir_close(&out);
}

static void test_open_abs_opens_absolute_directory(void** state)
{
    unit_env_t* env = *state;
    fs_dir_t    out;

    fs_dir_init(&out);
    assert_int_equal(fs_dir_open_abs(env->child_path, NULL, &out), 0);
    assert_same_directory_identity(env->child_path, &out);
    assert_int_equal(fs_dir_verify(&out, NULL), 0);
    fs_dir_close(&out);
}

static void test_open_abs_applies_expectation(void** state)
{
    unit_env_t* env = *state;
    fs_dir_t    out;
    fs_expect_t expect_ok;
    fs_expect_t expect_wrong_mode;
    fs_expect_t expect_invalid_mode;

    fs_dir_init(&out);
    expect_ok           = fs_expect_private(0700);
    expect_wrong_mode   = fs_expect_private(0755);
    expect_invalid_mode = fs_expect_private(S_IFDIR | 0700);

    assert_int_equal(fs_dir_open_abs(env->child_path, &expect_ok, &out), 0);
    fs_dir_close(&out);

    assert_int_equal(fs_dir_open_abs(env->child_path, &expect_wrong_mode, &out), -EACCES);
    assert_int_equal(out.fd, -1);

    assert_int_equal(fs_dir_open_abs(env->child_path, &expect_invalid_mode, &out), -EINVAL);
    assert_int_equal(out.fd, -1);
}

static void test_open_abs_rejects_non_directory(void** state)
{
    unit_env_t* env = *state;
    fs_dir_t    out;

    fs_dir_init(&out);
    assert_int_equal(fs_dir_open_abs(env->file_path, NULL, &out), -ENOTDIR);
    assert_int_equal(out.fd, -1);
}

static void test_open_abs_rejects_final_symlink(void** state)
{
    unit_env_t* env = *state;
    fs_dir_t    out;
    int         rc;

    fs_dir_init(&out);
    rc = fs_dir_open_abs(env->link_path, NULL, &out);
    assert_true(rc == -ELOOP || rc == -ENOTDIR);
    assert_int_equal(out.fd, -1);
}
