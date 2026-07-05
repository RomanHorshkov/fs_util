/**
 * @file fsutil.c
 * @brief Capability-oriented filesystem implementation.
 *
 * @author  Roman Horshkov <github.com/RomanHorshkov>
 * @date    2026
 * (c) 2026
 */

/*****************************************************************************************************************************************
 * PRIVATE DEFINES
 *****************************************************************************************************************************************
 */

#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif

/*****************************************************************************************************************************************
 * PRIVATE INCLUDES
 *****************************************************************************************************************************************
 */

#include "fsutil.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* NAME_MAX */
#include <stdio.h>  /* renameat */
#include <string.h> /* memcpy, strcmp, strchr */
#include <unistd.h> /* close, fsync, unlinkat, fchmod, fstat, geteuid, getegid */

/*****************************************************************************************************************************************
 * PRIVATE DEFINES
 *****************************************************************************************************************************************
 */
#define STRING_IS_NULL_OR_EMPTY(s) ((!s) || !*(s))

/*****************************************************************************************************************************************
 * PRIVATE ENUMERATED TYPEDEFS
 *****************************************************************************************************************************************
 */

enum _fs_verify_object_kind
{
    _FS_VERIFY_OBJECT_DIRECTORY = 0,
    _FS_VERIFY_OBJECT_REGULAR_FILE
};

enum _fs_component_state
{
    _FS_COMPONENT_STATE_END = 0,
    _FS_COMPONENT_STATE_VALUE
};

/*****************************************************************************************************************************************
 * PRIVATE STRUCTURED VARIABLES
 *****************************************************************************************************************************************
 */
/* None */

/*****************************************************************************************************************************************
 * PRIVATE VARIABLES
 *****************************************************************************************************************************************
 */
/* None */

/*****************************************************************************************************************************************
 * PRIVATE FUNCTIONS PROTOTYPES
 *****************************************************************************************************************************************
 */

/**
 * @brief Verify that fd has FD_CLOEXEC set.
 *
 * @param fd File descriptor to inspect.
 * @return 0 on success, negative errno on failure.
 *
 * Returns -EINVAL when the descriptor does not have FD_CLOEXEC set.
 */
static int _fs_fd_verify_cloexec(int fd);

/**
 * @brief Check whether mode uses only permission/special bits accepted here.
 *
 * @param mode Mode value to validate.
 * @return 1 if valid, 0 if invalid.
 *
 * Accepted bits are those in FS_MODE_MASK.
 * Object type bits such as S_IFDIR are rejected.
 */
static int _fs_mode_is_valid(mode_t mode);

/**
 * @brief Verify object type and optional metadata against an expectation.
 *
 * @param st Stat structure to inspect.
 * @param object_kind Expected object kind.
 * @param expect Optional exact metadata expectation.
 * @return 0 on success, negative errno on failure.
 *
 * When expect is null, only object-kind verification is performed.
 */
static int _fs_verify_stat(const struct stat* st, enum _fs_verify_object_kind object_kind, const fs_expect_t* expect);

/**
 * @brief Parse the next component from a relative walk path.
 *
 * @param cursor In/out scan position.
 * @param out_component Destination buffer for the parsed component.
 * @param out_component_size Size of out_component.
 * @param out_state Receives end-of-input or component-present state.
 * @return 0 on success, negative errno on failure.
 *
 * Repeated '/' characters are collapsed naturally.
 */
static int _fs_component_next(const char** cursor, char* out_component, size_t out_component_size, enum _fs_component_state* out_state);

/**
 * @brief Duplicate a directory fd with FD_CLOEXEC preserved.
 *
 * @param fd Directory fd to duplicate.
 * @param out_fd Receives the duplicated fd.
 * @return 0 on success, negative errno on failure.
 */
static int _fs_dup_dir_fd(int fd, int* out_fd);

/**
 * @brief Best-effort cleanup for a newly-created file after post-create failure.
 *
 * @param parent_fd Trusted parent directory fd.
 * @param name Single child name to unlink.
 *
 * This cleanup is name-based and intended for trusted/private parent
 * directories.
 */
static void _fs_cleanup_created_file_at(int parent_fd, const char* name);

/**
 * @brief Best-effort cleanup for a newly-created directory after post-create failure.
 *
 * @param parent_fd Trusted parent directory fd.
 * @param name Single child name to remove with AT_REMOVEDIR.
 *
 * This cleanup is name-based and intended for trusted/private parent
 * directories.
 */
static void _fs_cleanup_created_directory_at(int parent_fd, const char* name);

/*****************************************************************************************************************************************
 * PUBLIC FUNCTIONS DEFINITIONS
 *****************************************************************************************************************************************
 */

fs_expect_t fs_expect_make(mode_t mode, uid_t uid, gid_t gid)
{
    /* Build expectation object explicitly from caller-provided values. */
    fs_expect_t expect;
    expect.mode = mode;
    expect.uid  = uid;
    expect.gid  = gid;
    return expect;
}

fs_expect_t fs_expect_private(mode_t mode)
{
    /* Use the current effective uid/gid as the expected owner. */
    return fs_expect_make(mode, geteuid(), getegid());
}

void fs_dir_init(fs_dir_t* dir)
{
    /* Accept null for convenience in cleanup paths. */
    if(!dir) return;

    /* -1 is the canonical "closed / not-owned" state. */
    dir->fd = -1;
}

void fs_dir_close(fs_dir_t* dir)
{
    /* Accept null for convenience in cleanup paths. */
    if(!dir) return;

    /* Close the owned fd only when the handle currently owns one. */
    if(dir->fd >= 0) (void)close(dir->fd);

    /* Restore the exact canonical closed state regardless of prior value. */
    dir->fd = -1;
}

int fs_component_is_valid(const char* name)
{
    /* Component must exist and be non-empty. */
    if(STRING_IS_NULL_OR_EMPTY(name)) return 0;

    /* "." and ".." are not accepted as single-component names. */
    if(strcmp(name, ".") == 0) return 0;
    if(strcmp(name, "..") == 0) return 0;

    /* Single-component helpers reject '/' entirely. */
    return strchr(name, '/') == NULL ? 1 : 0;
}

int fs_dir_open_cwd(fs_dir_t* out_dir)
{
    /* Check input. */
    if(!out_dir) return -EINVAL;
    if(out_dir->fd != -1) return -EBUSY;

    /* Bootstrap a capability from the current working directory. */
    int fd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0) return -errno;

    /* Transfer ownership of the opened fd to the output handle. */
    out_dir->fd = fd;
    return 0;
}

int fs_dir_open_abs(const char* abs_path, const fs_expect_t* expect, fs_dir_t* out_dir)
{
    /* Check input. */
    if(!out_dir) return -EINVAL;
    if(STRING_IS_NULL_OR_EMPTY(abs_path)) return -EINVAL;
    if(abs_path[0] != '/') return -EINVAL;
    if(out_dir->fd != -1) return -EBUSY;

    /* Bootstrap a capability from an absolute directory root. O_NOFOLLOW
     * rejects a symlink at the final component; the trusted parent chain is
     * resolved normally. */
    int fd = open(abs_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(fd < 0) return -errno;

    /* Apply the same verification the *at helpers apply after open. */
    fs_dir_t opened = {.fd = fd};
    int      rc     = fs_dir_verify(&opened, expect);
    if(rc != 0)
    {
        close(fd);
        return rc;
    }

    /* Transfer ownership of the opened fd to the output handle. */
    out_dir->fd = fd;
    return 0;
}

int fs_dir_verify(const fs_dir_t* dir, const fs_expect_t* expect)
{
    /* Check input. */
    if(!dir || dir->fd < 0) return -EINVAL;

    /* Directory capabilities must be close-on-exec. */
    int rc = _fs_fd_verify_cloexec(dir->fd);
    if(rc != 0) return rc;

    /* Read metadata from the already-open directory fd. */
    struct stat st;
    if(fstat(dir->fd, &st) != 0) return -errno;

    /* Verify directory type first, then optional mode/uid/gid expectations. */
    return _fs_verify_stat(&st, _FS_VERIFY_OBJECT_DIRECTORY, expect);
}

int fs_file_verify(int fd, const fs_expect_t* expect)
{
    /* Check input. */
    if(fd < 0) return -EINVAL;

    /* File descriptors verified by this layer must be close-on-exec. */
    int rc = _fs_fd_verify_cloexec(fd);
    if(rc != 0) return rc;

    /* Read metadata from the already-open regular-file fd. */
    struct stat st;
    if(fstat(fd, &st) != 0) return -errno;

    /* Verify file type first, then optional mode/uid/gid expectations. */
    return _fs_verify_stat(&st, _FS_VERIFY_OBJECT_REGULAR_FILE, expect);
}

int fs_dir_open_at(const fs_dir_t* parent, const char* name, const fs_expect_t* expect, fs_dir_t* out_dir)
{
    /* Check input. */
    if(!parent || parent->fd < 0 || !out_dir) return -EINVAL;
    if(!fs_component_is_valid(name)) return -EINVAL;
    if(out_dir->fd != -1) return -EBUSY;

    /* Parent must already be a valid directory capability. */
    int rc = fs_dir_verify(parent, NULL);
    if(rc != 0) return rc;

    /* Open exactly one child directory, rejecting a final symlink. */
    int fd = openat(parent->fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(fd < 0) return -errno;

    /* Verify the opened child before returning its capability. */
    rc = fs_dir_verify(&(fs_dir_t){.fd = fd}, expect);
    if(rc != 0)
    {
        /* Verification failure means we must not leak the temporary fd. */
        close(fd);
        return rc;
    }

    /* Transfer ownership of the opened fd to the output handle. */
    out_dir->fd = fd;
    return 0;
}

int fs_dir_create_at(const fs_dir_t* parent, const char* name, mode_t create_mode, const fs_expect_t* expect, fs_dir_t* out_dir,
                     enum fs_create_disposition* out_disposition)
{
    /* Check input. */
    if(!parent || parent->fd < 0 || !out_dir) return -EINVAL;
    if(!fs_component_is_valid(name)) return -EINVAL;
    if(!_fs_mode_is_valid(create_mode)) return -EINVAL;
    if(out_dir->fd != -1) return -EBUSY;

    /* Parent must already be a valid directory capability. */
    int rc = fs_dir_verify(parent, NULL);
    if(rc != 0) return rc;

    /* Make outputs predictable even on failure. */
    if(out_disposition) *out_disposition = FS_CREATE_DISPOSITION_OPENED_EXISTING;

    /* Track whether mkdirat() created a new directory this time. */
    enum fs_create_disposition create_disposition = FS_CREATE_DISPOSITION_OPENED_EXISTING;

    /*
     * Try to create first.
     * This avoids a separate existence check and therefore avoids the classic
     * check-then-create race.
     */
    if(mkdirat(parent->fd, name, create_mode) == 0)
    {
        create_disposition = FS_CREATE_DISPOSITION_CREATED_NEW;
    }
    else if(errno != EEXIST)
    {
        return -errno;
    }

    /* Open the resulting directory capability, rejecting a final symlink. */
    int fd = openat(parent->fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(fd < 0)
    {
        int saved_errno = errno;
        if(create_disposition == FS_CREATE_DISPOSITION_CREATED_NEW) _fs_cleanup_created_directory_at(parent->fd, name);
        return -saved_errno;
    }

    /*
     * Force the final directory mode explicitly.
     * This makes the result independent from the caller's current umask.
     */
    if(create_disposition == FS_CREATE_DISPOSITION_CREATED_NEW && fchmod(fd, create_mode) != 0)
    {
        int saved_errno = errno;
        close(fd);
        _fs_cleanup_created_directory_at(parent->fd, name);
        return -saved_errno;
    }

    if(create_disposition == FS_CREATE_DISPOSITION_CREATED_NEW)
    {
        /* Verify the freshly-created directory has the explicit requested mode. */
        fs_expect_t created_expect = fs_expect_make(create_mode, FS_EXPECT_ANY_UID, FS_EXPECT_ANY_GID);
        rc                         = fs_dir_verify(&(fs_dir_t){.fd = fd}, &created_expect);
        if(rc != 0)
        {
            /* Verification failure means we must not leak the temporary fd. */
            close(fd);
            _fs_cleanup_created_directory_at(parent->fd, name);
            return rc;
        }
    }

    /* Apply the caller's full expectation policy to the resulting directory. */
    rc = fs_dir_verify(&(fs_dir_t){.fd = fd}, expect);
    if(rc != 0)
    {
        /* Verification failure means we must not leak the temporary fd. */
        close(fd);
        if(create_disposition == FS_CREATE_DISPOSITION_CREATED_NEW) _fs_cleanup_created_directory_at(parent->fd, name);
        return rc;
    }

    /* Report create/open disposition if the caller asked for it. */
    if(out_disposition) *out_disposition = create_disposition;

    /* Transfer ownership of the opened fd to the output handle. */
    out_dir->fd = fd;
    return 0;
}

int fs_dir_walk_open(const fs_dir_t* start, const char* relative_path, const fs_expect_t* expect, fs_dir_t* out_dir)
{
    /* Check input. */
    if(!start || start->fd < 0 || !relative_path || !out_dir) return -EINVAL;
    if(relative_path[0] == '/') return -EINVAL;
    if(out_dir->fd != -1) return -EBUSY;

    /* Starting handle must already be a valid directory capability. */
    int rc = fs_dir_verify(start, NULL);
    if(rc != 0) return rc;

    /* Walk relative_path one component at a time from the starting capability. */
    const char* cursor                 = relative_path;
    int         parent_fd              = start->fd;
    int         current_fd             = -1;
    size_t      walked_component_count = 0U;

    for(;;)
    {
        /* Parse the next relative path component. */
        char                     component[NAME_MAX + 1];
        enum _fs_component_state component_state = _FS_COMPONENT_STATE_END;
        rc                                       = _fs_component_next(&cursor, component, sizeof(component), &component_state);
        if(rc != 0)
        {
            /* Failure during walk must not leak any transient fd. */
            if(current_fd >= 0) close(current_fd);
            return rc;
        }

        /* No more components means the walk is complete. */
        if(component_state == _FS_COMPONENT_STATE_END) break;

        /* Ignore "." components so callers may pass "./database/". */
        if(strcmp(component, ".") == 0) continue;

        /* Reject ".." so the walk cannot escape the trusted starting point. */
        if(strcmp(component, "..") == 0)
        {
            if(current_fd >= 0) close(current_fd);
            return -EINVAL;
        }

        /* Open exactly one child directory, rejecting a final symlink. */
        int next_fd = openat(parent_fd, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if(next_fd < 0)
        {
            int saved_errno = errno;
            if(current_fd >= 0) close(current_fd);
            return -saved_errno;
        }

        /* Verify the opened child against the caller's policy. */
        rc = fs_dir_verify(&(fs_dir_t){.fd = next_fd}, expect);
        if(rc != 0)
        {
            close(next_fd);
            if(current_fd >= 0) close(current_fd);
            return rc;
        }

        /* Move the walk forward and release the previous transient directory fd. */
        if(current_fd >= 0) close(current_fd);
        current_fd = next_fd;
        parent_fd  = next_fd;
        ++walked_component_count;
    }

    if(walked_component_count == 0U)
    {
        /* Empty or "."-only paths resolve to the starting capability itself. */
        rc = fs_dir_verify(start, expect);
        if(rc != 0) return rc;
        return _fs_dup_dir_fd(start->fd, &out_dir->fd);
    }

    /* Transfer ownership of the final walked directory fd to the output handle. */
    out_dir->fd = current_fd;
    return 0;
}

int fs_dir_walk_create(const fs_dir_t* start, const char* relative_path, mode_t create_mode, const fs_expect_t* expect, fs_dir_t* out_dir)
{
    /* Check input. */
    if(!start || start->fd < 0 || !relative_path || !out_dir) return -EINVAL;
    if(relative_path[0] == '/') return -EINVAL;
    if(!_fs_mode_is_valid(create_mode)) return -EINVAL;
    if(out_dir->fd != -1) return -EBUSY;

    /* Starting handle must already be a valid directory capability. */
    int rc = fs_dir_verify(start, NULL);
    if(rc != 0) return rc;

    /* Walk relative_path one component at a time from the starting capability. */
    const char* cursor                 = relative_path;
    int         parent_fd              = start->fd;
    int         current_fd             = -1;
    size_t      walked_component_count = 0U;

    for(;;)
    {
        /* Parse the next relative path component. */
        char                     component[NAME_MAX + 1];
        enum _fs_component_state component_state = _FS_COMPONENT_STATE_END;
        rc                                       = _fs_component_next(&cursor, component, sizeof(component), &component_state);
        if(rc != 0)
        {
            /* Failure during walk must not leak any transient fd. */
            if(current_fd >= 0) close(current_fd);
            return rc;
        }

        /* No more components means the walk is complete. */
        if(component_state == _FS_COMPONENT_STATE_END) break;

        /* Ignore "." components so callers may pass "./database/". */
        if(strcmp(component, ".") == 0) continue;

        /* Reject ".." so the walk cannot escape the trusted starting point. */
        if(strcmp(component, "..") == 0)
        {
            if(current_fd >= 0) close(current_fd);
            return -EINVAL;
        }

        /* Create-or-open exactly one child directory component. */
        fs_dir_t next_dir;
        fs_dir_init(&next_dir);
        rc = fs_dir_create_at(&(fs_dir_t){.fd = parent_fd}, component, create_mode, expect, &next_dir, NULL);
        if(rc != 0)
        {
            /* Failure during walk must not leak any transient fd. */
            if(current_fd >= 0) close(current_fd);
            return rc;
        }

        /* Move the walk forward and release the previous transient directory fd. */
        if(current_fd >= 0) close(current_fd);
        current_fd = next_dir.fd;
        parent_fd  = next_dir.fd;
        ++walked_component_count;
    }

    if(walked_component_count == 0U)
    {
        /* Empty or "."-only paths resolve to the starting capability itself. */
        rc = fs_dir_verify(start, expect);
        if(rc != 0) return rc;
        return _fs_dup_dir_fd(start->fd, &out_dir->fd);
    }

    /* Transfer ownership of the final walked directory fd to the output handle. */
    out_dir->fd = current_fd;
    return 0;
}

int fs_file_open_read_at(const fs_dir_t* parent, const char* name, const fs_expect_t* expect, int* out_fd)
{
    /* Check output pointer first so raw-fd ownership is reset whenever possible. */
    if(!out_fd) return -EINVAL;

    /* Reset raw-fd output immediately so failure never leaves stale ownership behind. */
    *out_fd = -1;

    /* Check the remaining inputs. */
    if(!parent || parent->fd < 0) return -EINVAL;
    if(!fs_component_is_valid(name)) return -EINVAL;

    /* Parent must already be a valid directory capability. */
    int rc = fs_dir_verify(parent, NULL);
    if(rc != 0) return rc;

    /*
     * Open nonblocking first.
     * This avoids hanging on FIFOs or other special files before type
     * verification rejects them, and O_NOCTTY avoids controlling-terminal
     * side effects if the path names a terminal-like device.
     */
    int fd = openat(parent->fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY);
    if(fd < 0) return -errno;

    /* Verify the opened child before returning its fd to the caller. */
    rc = fs_file_verify(fd, expect);
    if(rc != 0)
    {
        /* Verification failure means we must not leak the temporary fd. */
        close(fd);
        return rc;
    }

    /*
     * Restore normal blocking status on the returned fd.
     * O_NONBLOCK was used only to make acquisition of non-regular files safe.
     */
    int status_flags = fcntl(fd, F_GETFL);
    if(status_flags < 0)
    {
        int saved_errno = errno;
        close(fd);
        return -saved_errno;
    }
    if((status_flags & O_NONBLOCK) != 0 && fcntl(fd, F_SETFL, status_flags & ~O_NONBLOCK) != 0)
    {
        int saved_errno = errno;
        close(fd);
        return -saved_errno;
    }

    /* Transfer ownership of the opened fd to the caller. */
    *out_fd = fd;
    return 0;
}

int fs_file_create_write_new_at(const fs_dir_t* parent, const char* name, mode_t create_mode, const fs_expect_t* expect, int* out_fd)
{
    /* Check output pointer first so raw-fd ownership is reset whenever possible. */
    if(!out_fd) return -EINVAL;

    /* Reset raw-fd output immediately so failure never leaves stale ownership behind. */
    *out_fd = -1;

    /* Check the remaining inputs. */
    if(!parent || parent->fd < 0) return -EINVAL;
    if(!fs_component_is_valid(name)) return -EINVAL;
    if(!_fs_mode_is_valid(create_mode)) return -EINVAL;

    /* Parent must already be a valid directory capability. */
    int rc = fs_dir_verify(parent, NULL);
    if(rc != 0) return rc;

    /* Create a brand-new child file, rejecting a final symlink and avoiding controlling-terminal side effects. */
    int fd = openat(parent->fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY, create_mode);
    if(fd < 0) return -errno;

    /*
     * Force the final file mode explicitly.
     * This makes the result independent from the caller's current umask.
     */
    if(fchmod(fd, create_mode) != 0)
    {
        int saved_errno = errno;
        close(fd);
        _fs_cleanup_created_file_at(parent->fd, name);
        return -saved_errno;
    }

    /* Verify the freshly-created file has the explicit requested mode. */
    fs_expect_t created_expect = fs_expect_make(create_mode, FS_EXPECT_ANY_UID, FS_EXPECT_ANY_GID);
    rc                         = fs_file_verify(fd, &created_expect);
    if(rc != 0)
    {
        /* Verification failure means we must not leak the temporary fd. */
        close(fd);
        _fs_cleanup_created_file_at(parent->fd, name);
        return rc;
    }

    /* Apply the caller's full expectation policy to the resulting file. */
    rc = fs_file_verify(fd, expect);
    if(rc != 0)
    {
        /* Verification failure means we must not leak the temporary fd. */
        close(fd);
        _fs_cleanup_created_file_at(parent->fd, name);
        return rc;
    }

    /* Transfer ownership of the opened fd to the caller. */
    *out_fd = fd;
    return 0;
}

int fs_rename_at(const fs_dir_t* old_parent, const char* old_name, const fs_dir_t* new_parent, const char* new_name)
{
    /* Check input. */
    if(!old_parent || old_parent->fd < 0 || !new_parent || new_parent->fd < 0) return -EINVAL;
    if(!fs_component_is_valid(old_name) || !fs_component_is_valid(new_name)) return -EINVAL;

    /* Both parents must already be valid directory capabilities. */
    int rc = fs_dir_verify(old_parent, NULL);
    if(rc != 0) return rc;
    rc = fs_dir_verify(new_parent, NULL);
    if(rc != 0) return rc;

    /* Rename exactly one path component from old_parent to new_parent. */
    if(renameat(old_parent->fd, old_name, new_parent->fd, new_name) != 0) return -errno;
    return 0;
}

int fs_unlink_at(const fs_dir_t* parent, const char* name)
{
    /* Check input. */
    if(!parent || parent->fd < 0) return -EINVAL;
    if(!fs_component_is_valid(name)) return -EINVAL;

    /* Parent must already be a valid directory capability. */
    int rc = fs_dir_verify(parent, NULL);
    if(rc != 0) return rc;

    /* Unlink exactly one child entry from the parent capability. */
    if(unlinkat(parent->fd, name, 0) != 0) return -errno;
    return 0;
}

int fs_dir_fsync(const fs_dir_t* dir)
{
    /* Check input. */
    int rc = fs_dir_verify(dir, NULL);
    if(rc != 0) return rc;

    /* Some filesystems do not support directory fsync; treat EINVAL as success. */
    if(fsync(dir->fd) == 0) return 0;
    if(errno == EINVAL) return 0;
    return -errno;
}

int fs_file_fsync(int fd)
{
    /* Check input. */
    int rc = fs_file_verify(fd, NULL);
    if(rc != 0) return rc;

    /* Push regular-file data and metadata to stable storage. */
    if(fsync(fd) != 0) return -errno;
    return 0;
}

/*****************************************************************************************************************************************
 * PRIVATE FUNCTIONS DEFINITIONS
 *****************************************************************************************************************************************
 */

static int _fs_verify_stat(const struct stat* st, enum _fs_verify_object_kind object_kind, const fs_expect_t* expect)
{
    /* Check input. */
    if(!st) return -EINVAL;

    /* Verify the object type first; mode/uid/gid checks happen only afterwards. */
    switch(object_kind)
    {
        case _FS_VERIFY_OBJECT_DIRECTORY:
            if(!S_ISDIR(st->st_mode)) return -ENOTDIR;
            break;

        case _FS_VERIFY_OBJECT_REGULAR_FILE:
            if(S_ISDIR(st->st_mode)) return -EISDIR;
            if(!S_ISREG(st->st_mode)) return -EINVAL;
            break;

        default:
            return -EINVAL;
    }

    /* Null expectation means "type check only". */
    if(expect == NULL) return 0;

    /* Reject invalid mode values in the expectation object. */
    if(expect->mode != FS_EXPECT_ANY_MODE && !_fs_mode_is_valid(expect->mode)) return -EINVAL;

    /* Check exact permission bits if the caller requested them. */
    if(expect->mode != FS_EXPECT_ANY_MODE && (st->st_mode & FS_MODE_MASK) != expect->mode) return -EACCES;

    /* Check exact uid if the caller requested it. */
    if(expect->uid != FS_EXPECT_ANY_UID && st->st_uid != expect->uid) return -EACCES;

    /* Check exact gid if the caller requested it. */
    if(expect->gid != FS_EXPECT_ANY_GID && st->st_gid != expect->gid) return -EACCES;

    return 0;
}

static int _fs_component_next(const char** cursor, char* out_component, size_t out_component_size, enum _fs_component_state* out_state)
{
    /* Check input. */
    if(!cursor || !*cursor || !out_component || out_component_size < 2 || !out_state) return -EINVAL;

    /* Start from the current scan position. */
    const char* p = *cursor;

    /* Skip path separators so repeated '/' characters collapse naturally. */
    while(*p == '/')
        ++p;

    /* No non-separator character means there are no more components. */
    if(*p == '\0')
    {
        *cursor    = p;
        *out_state = _FS_COMPONENT_STATE_END;
        return 0;
    }

    /* Mark the beginning of the next path component. */
    const char* start = p;

    /* Advance until the next separator or string terminator. */
    while(*p != '\0' && *p != '/')
        ++p;

    /* Compute component length and ensure it fits into the caller buffer. */
    size_t len = (size_t)(p - start);
    if(len == 0 || len >= out_component_size) return -ENAMETOOLONG;

    /* Copy the parsed component and NUL-terminate it. */
    memcpy(out_component, start, len);
    out_component[len] = '\0';

    /* Publish the new scan position and the "component present" state. */
    *cursor    = p;
    *out_state = _FS_COMPONENT_STATE_VALUE;
    return 0;
}

static int _fs_dup_dir_fd(int fd, int* out_fd)
{
    /* Check input. */
    if(fd < 0 || !out_fd) return -EINVAL;

    /* Duplicate with close-on-exec preserved as an explicit capability copy. */
    int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if(dup_fd < 0) return -errno;

    /* Transfer the duplicated fd to the caller. */
    *out_fd = dup_fd;
    return 0;
}

static int _fs_fd_verify_cloexec(int fd)
{
    /* Read descriptor flags from the kernel. */
    int flags = fcntl(fd, F_GETFD);
    if(flags < 0) return -errno;

    /* Directory/file capabilities in this layer must not survive exec(). */
    if((flags & FD_CLOEXEC) == 0) return -EINVAL;

    return 0;
}

static int _fs_mode_is_valid(mode_t mode)
{
    /* Only permission bits plus sticky/setgid/setuid are accepted here. */
    return (mode & ~FS_MODE_MASK) == 0 ? 1 : 0;
}

static void _fs_cleanup_created_file_at(int parent_fd, const char* name)
{
    /* Best-effort cleanup for a newly-created file after post-create failure. */
    if(parent_fd < 0 || !name) return;

    (void)unlinkat(parent_fd, name, 0);
}

static void _fs_cleanup_created_directory_at(int parent_fd, const char* name)
{
    /* Best-effort cleanup for a newly-created directory after post-create failure. */
    if(parent_fd < 0 || !name) return;

    (void)unlinkat(parent_fd, name, AT_REMOVEDIR);
}
