/**
 * @file fsutil.c
 * @brief Capability-oriented filesystem implementation.
 *
 * @author  Roman Horshkov <github.com/RomanHorshkov>
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

#include <dirent.h> /* DIR, fdopendir, readdir, rewinddir, closedir */
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* NAME_MAX, PATH_MAX */
#include <stdio.h>  /* renameat, renameat2, RENAME_NOREPLACE */
#include <string.h> /* memcpy, strcmp, strchr, strlen */
#include <time.h>   /* nanosleep */
#include <unistd.h> /* close, fsync, fdatasync, read, write, pread, pwrite, unlinkat, fchmod, fchmodat, fstat, geteuid, getegid, linkat, symlinkat, readlinkat */

/*****************************************************************************************************************************************
 * PRIVATE DEFINES
 *****************************************************************************************************************************************
 */
#define STRING_IS_NULL_OR_EMPTY(s) ((!s) || !*(s))

/*
 * Every kernel call this library makes goes through one of the FS_SYS_* macros.
 *
 * Production builds map them straight onto libc. Test builds (-DFS_UTIL_TESTING)
 * route them through a table of function pointers the unit tests can replace,
 * which is the only way to drive the failure branches that a real filesystem
 * will not produce on demand (fchmod() failing on a file we just created,
 * fstat() failing on an open fd, fcntl() refusing F_GETFL, fsync() reporting
 * EIO, ...). The seam adds no symbol to the production library.
 */
#ifdef FS_UTIL_TESTING
#    include "fsutil_test_hooks.h"
#    define FS_SYS_OPEN(path, flags)                        (fs_test_hooks.open((path), (flags)))
#    define FS_SYS_OPENAT(dirfd, name, flags, mode)         (fs_test_hooks.openat((dirfd), (name), (flags), (mode)))
#    define FS_SYS_MKDIRAT(dirfd, name, mode)               (fs_test_hooks.mkdirat((dirfd), (name), (mode)))
#    define FS_SYS_FCHMOD(fd, mode)                         (fs_test_hooks.fchmod((fd), (mode)))
#    define FS_SYS_FCHMODAT(dirfd, name, mode)              (fs_test_hooks.fchmodat((dirfd), (name), (mode)))
#    define FS_SYS_FSTAT(fd, st)                            (fs_test_hooks.fstat((fd), (st)))
#    define FS_SYS_FCNTL(fd, cmd, arg)                      (fs_test_hooks.fcntl((fd), (cmd), (arg)))
#    define FS_SYS_FSYNC(fd)                                (fs_test_hooks.fsync((fd)))
#    define FS_SYS_FDATASYNC(fd)                            (fs_test_hooks.fdatasync((fd)))
#    define FS_SYS_READ(fd, buf, len)                       (fs_test_hooks.read((fd), (buf), (len)))
#    define FS_SYS_WRITE(fd, buf, len)                      (fs_test_hooks.write((fd), (buf), (len)))
#    define FS_SYS_PREAD(fd, buf, len, off)                 (fs_test_hooks.pread((fd), (buf), (len), (off)))
#    define FS_SYS_PWRITE(fd, buf, len, off)                (fs_test_hooks.pwrite((fd), (buf), (len), (off)))
#    define FS_SYS_RENAMEAT(ofd, oname, nfd, nname)         (fs_test_hooks.renameat((ofd), (oname), (nfd), (nname)))
#    define FS_SYS_RENAMEAT2(ofd, oname, nfd, nname, flags) (fs_test_hooks.renameat2((ofd), (oname), (nfd), (nname), (flags)))
#    define FS_SYS_UNLINKAT(dirfd, name, flags)             (fs_test_hooks.unlinkat((dirfd), (name), (flags)))
#    define FS_SYS_SYMLINKAT(target, dirfd, name)           (fs_test_hooks.symlinkat((target), (dirfd), (name)))
#    define FS_SYS_LINKAT(ofd, oname, nfd, nname, flags)    (fs_test_hooks.linkat((ofd), (oname), (nfd), (nname), (flags)))
#    define FS_SYS_FSTATAT(dirfd, name, st, flags)          (fs_test_hooks.fstatat((dirfd), (name), (st), (flags)))
#    define FS_SYS_READLINKAT(dirfd, name, buf, size)       (fs_test_hooks.readlinkat((dirfd), (name), (buf), (size)))
#    define FS_SYS_FDOPENDIR(fd)                            (fs_test_hooks.fdopendir((fd)))
#    define FS_SYS_READDIR(dir)                             (fs_test_hooks.readdir((dir)))
#else
#    define FS_SYS_OPEN(path, flags)                        open((path), (flags))
#    define FS_SYS_OPENAT(dirfd, name, flags, mode)         openat((dirfd), (name), (flags), (mode))
#    define FS_SYS_MKDIRAT(dirfd, name, mode)               mkdirat((dirfd), (name), (mode))
#    define FS_SYS_FCHMOD(fd, mode)                         fchmod((fd), (mode))
#    define FS_SYS_FCHMODAT(dirfd, name, mode)              fchmodat((dirfd), (name), (mode), 0)
#    define FS_SYS_FSTAT(fd, st)                            fstat((fd), (st))
#    define FS_SYS_FCNTL(fd, cmd, arg)                      fcntl((fd), (cmd), (arg))
#    define FS_SYS_FSYNC(fd)                                fsync((fd))
#    define FS_SYS_FDATASYNC(fd)                            fdatasync((fd))
#    define FS_SYS_READ(fd, buf, len)                       read((fd), (buf), (len))
#    define FS_SYS_WRITE(fd, buf, len)                      write((fd), (buf), (len))
#    define FS_SYS_PREAD(fd, buf, len, off)                 pread((fd), (buf), (len), (off))
#    define FS_SYS_PWRITE(fd, buf, len, off)                pwrite((fd), (buf), (len), (off))
#    define FS_SYS_RENAMEAT(ofd, oname, nfd, nname)         renameat((ofd), (oname), (nfd), (nname))
#    define FS_SYS_RENAMEAT2(ofd, oname, nfd, nname, flags) renameat2((ofd), (oname), (nfd), (nname), (flags))
#    define FS_SYS_UNLINKAT(dirfd, name, flags)             unlinkat((dirfd), (name), (flags))
#    define FS_SYS_SYMLINKAT(target, dirfd, name)           symlinkat((target), (dirfd), (name))
#    define FS_SYS_LINKAT(ofd, oname, nfd, nname, flags)    linkat((ofd), (oname), (nfd), (nname), (flags))
#    define FS_SYS_FSTATAT(dirfd, name, st, flags)          fstatat((dirfd), (name), (st), (flags))
#    define FS_SYS_READLINKAT(dirfd, name, buf, size)       readlinkat((dirfd), (name), (buf), (size))
#    define FS_SYS_FDOPENDIR(fd)                            fdopendir((fd))
#    define FS_SYS_READDIR(dir)                             readdir((dir))
#endif

/* Flags shared by every directory acquisition in this file: read-only, must be a
 * directory, never survives exec(), never follows a symlink at the final component. */
#define FS_DIR_OPEN_FLAGS   (O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)

/* Suffix of the temporary entry the composed helpers create beside their target. A fixed
 * name (not a random one) is what lets a restart clean up after a crash. */
#define FS_TMP_SUFFIX       ".tmp"

/* fs_dir_create_at() meeting a directory another creator made a moment ago: until that creator's fchmodat() lands, the
 * directory carries the mode mkdirat() gave it (the umask's cut, the setgid bit a setgid parent hands down), so its
 * expectation is checked again FS_DIR_SETTLE_TRIES times, FS_DIR_SETTLE_NS apart (50 ms at most), before it is refused. */
#define FS_DIR_SETTLE_TRIES 50
#define FS_DIR_SETTLE_NS    1000000L

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

enum _fs_walk_mode
{
    _FS_WALK_OPEN_EXISTING = 0,
    _FS_WALK_CREATE_MISSING
};

/*****************************************************************************************************************************************
 * PRIVATE STRUCTURED TYPEDEFS
 *****************************************************************************************************************************************
 */
/* None */

/*****************************************************************************************************************************************
 * PRIVATE STRUCTURED VARIABLES
 *****************************************************************************************************************************************
 */
/* None */

/*****************************************************************************************************************************************
 * PRIVATE VARIABLES
 *****************************************************************************************************************************************
 */

#ifdef FS_UTIL_TESTING
static int _fs_default_open(const char* path, int flags)
{
    return open(path, flags);
}
static int _fs_default_openat(int dirfd, const char* name, int flags, mode_t mode)
{
    return openat(dirfd, name, flags, mode);
}
static int _fs_default_fcntl(int fd, int cmd, int arg)
{
    return fcntl(fd, cmd, arg);
}
static int _fs_default_fchmodat(int dirfd, const char* name, mode_t mode)
{
    return fchmodat(dirfd, name, mode, 0);
}
static int _fs_default_renameat2(int ofd, const char* oname, int nfd, const char* nname, unsigned int flags)
{
    return renameat2(ofd, oname, nfd, nname, flags);
}

fs_test_hooks_t fs_test_hooks = {
    .open       = _fs_default_open,
    .openat     = _fs_default_openat,
    .mkdirat    = mkdirat,
    .fchmod     = fchmod,
    .fchmodat   = _fs_default_fchmodat,
    .fstat      = fstat,
    .fcntl      = _fs_default_fcntl,
    .fsync      = fsync,
    .fdatasync  = fdatasync,
    .read       = read,
    .write      = write,
    .pread      = pread,
    .pwrite     = pwrite,
    .renameat   = renameat,
    .renameat2  = _fs_default_renameat2,
    .unlinkat   = unlinkat,
    .symlinkat  = symlinkat,
    .linkat     = linkat,
    .fstatat    = fstatat,
    .readlinkat = readlinkat,
    .fdopendir  = fdopendir,
    .readdir    = readdir,
};

void fs_test_hooks_reset(void)
{
    fs_test_hooks.open       = _fs_default_open;
    fs_test_hooks.openat     = _fs_default_openat;
    fs_test_hooks.mkdirat    = mkdirat;
    fs_test_hooks.fchmod     = fchmod;
    fs_test_hooks.fchmodat   = _fs_default_fchmodat;
    fs_test_hooks.fstat      = fstat;
    fs_test_hooks.fcntl      = _fs_default_fcntl;
    fs_test_hooks.fsync      = fsync;
    fs_test_hooks.fdatasync  = fdatasync;
    fs_test_hooks.read       = read;
    fs_test_hooks.write      = write;
    fs_test_hooks.pread      = pread;
    fs_test_hooks.pwrite     = pwrite;
    fs_test_hooks.renameat   = renameat;
    fs_test_hooks.renameat2  = _fs_default_renameat2;
    fs_test_hooks.unlinkat   = unlinkat;
    fs_test_hooks.symlinkat  = symlinkat;
    fs_test_hooks.linkat     = linkat;
    fs_test_hooks.fstatat    = fstatat;
    fs_test_hooks.readlinkat = readlinkat;
    fs_test_hooks.fdopendir  = fdopendir;
    fs_test_hooks.readdir    = readdir;
}
#endif

/*****************************************************************************************************************************************
 * PRIVATE FUNCTIONS PROTOTYPES
 *****************************************************************************************************************************************
 */

/**
 * @brief open() that retries when a signal interrupts it.
 *
 * A blocking open on a FUSE or network filesystem may return EINTR; local
 * filesystems never do. Retrying is always safe: no fd was created.
 */
static int _fs_open_eintr(const char* path, int flags);

/**
 * @brief openat() that retries when a signal interrupts it. See _fs_open_eintr().
 */
static int _fs_openat_eintr(int dirfd, const char* name, int flags, mode_t mode);

/**
 * @brief fsync() that retries when a signal interrupts it.
 *
 * Only EINTR is retried. Any other failure is reported once by the kernel and
 * is final; see fs_file_fsync().
 */
static int _fs_fsync_eintr(int fd);

/**
 * @brief fdatasync() that retries when a signal interrupts it. See _fs_fsync_eintr().
 */
static int _fs_fdatasync_eintr(int fd);

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
 * @brief Walk a relative path from an already-verified directory fd, one component at a time.
 *
 * @param start_fd Verified directory fd to start from (not consumed, not closed).
 * @param relative_path Relative path; '/' runs collapse, "." is skipped, ".." is refused.
 * @param mode _FS_WALK_OPEN_EXISTING opens each component with O_NOFOLLOW; _FS_WALK_CREATE_MISSING
 *             creates missing components through fs_dir_create_at().
 * @param create_mode Mode for components created in _FS_WALK_CREATE_MISSING mode.
 * @param each_expect Expectation applied to every walked component (null = directory type only).
 * @param out_fd Receives the final component's fd, or -1 when no component was walked.
 * @param out_walked Receives the number of components actually walked.
 * @return 0 on success, negative errno on failure. Never leaks a transient fd on failure.
 *
 * This is the single traversal engine behind fs_dir_walk_open(), fs_dir_walk_create() and
 * fs_dir_open_abs_nofollow(): every symlink at every component is rejected by the kernel
 * (O_NOFOLLOW on each openat()), and every ".." is rejected here, so the walk can never
 * leave the subtree rooted at start_fd.
 */
static int _fs_walk(int start_fd, const char* relative_path, enum _fs_walk_mode mode, mode_t create_mode, const fs_expect_t* each_expect,
                    int* out_fd, size_t* out_walked);

/**
 * @brief Open an EXISTING regular file under a parent capability with the given access mode.
 *
 * Shared body of fs_file_open_read_at() and fs_file_open_rw_at(): O_NOFOLLOW, the
 * O_NONBLOCK|O_NOCTTY acquisition, fs_file_verify(), then the blocking flags restored.
 *
 * @param access_flags O_RDONLY or O_RDWR.
 */
static int _fs_file_open_existing_at(const fs_dir_t* parent, const char* name, const fs_expect_t* expect, int* out_fd, int access_flags);

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
 * directories. The caller's errno is preserved across it.
 */
static void _fs_cleanup_created_file_at(int parent_fd, const char* name);

/**
 * @brief Best-effort cleanup for a newly-created directory after post-create failure.
 *
 * @param parent_fd Trusted parent directory fd.
 * @param name Single child name to remove with AT_REMOVEDIR.
 *
 * This cleanup is name-based and intended for trusted/private parent
 * directories. The caller's errno is preserved across it.
 */
static void _fs_cleanup_created_directory_at(int parent_fd, const char* name);

/**
 * @brief Build "<name>.tmp" for the composed helpers.
 *
 * @return 0 on success, -ENAMETOOLONG when the suffixed name does not fit `out_size`.
 */
static int _fs_tmp_name(const char* name, char* out, size_t out_size);

/**
 * @brief Mirror one directory: create `dst_name` under `dst_parent_fd`, then every entry of `src_fd`.
 *
 * @param depth Nesting level of `src_fd`; refused with -ELOOP beyond FS_LINK_TREE_MAX_DEPTH.
 */
static int _fs_link_tree(int src_fd, int dst_parent_fd, const char* dst_name, unsigned depth);

/**
 * @brief Mirror every entry of the open directory `src_fd` into the open directory `dst_fd`.
 */
static int _fs_link_entries(int src_fd, int dst_fd, unsigned depth);

/**
 * @brief Mirror one entry: recurse into a directory, hard-link a regular file, recreate a symlink.
 */
static int _fs_link_entry(int src_fd, int dst_fd, const char* name, unsigned depth);

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

    /* Single-component helpers reject '/' entirely, and control characters:
     * legal on POSIX, never needed by a database, and poison for logs and tooling. */
    for(const unsigned char* p = (const unsigned char*)name; *p != '\0'; ++p)
    {
        if(*p == '/' || *p < 0x20U || *p == 0x7FU) return 0;
    }
    return 1;
}

int fs_dir_open_cwd(fs_dir_t* out_dir)
{
    /* Check input. */
    if(!out_dir) return -EINVAL;
    if(out_dir->fd != -1) return -EBUSY;

    /* Bootstrap a capability from the current working directory. */
    int fd = _fs_open_eintr(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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

    /* O_NOFOLLOW protects the LAST component only. With a trailing '/', "/." or "/.."
     * the last component is empty or a dot entry, and the kernel would follow a symlink
     * sitting just before it. Require a real final component; "/" alone is the root. */
    if(abs_path[1] != '\0' && !fs_component_is_valid(strrchr(abs_path, '/') + 1)) return -EINVAL;

    /* Bootstrap a capability from an absolute directory root. O_NOFOLLOW
     * rejects a symlink at the final component; the trusted parent chain is
     * resolved normally. */
    int fd = _fs_open_eintr(abs_path, FS_DIR_OPEN_FLAGS);
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

int fs_dir_open_abs_nofollow(const char* abs_path, const fs_expect_t* expect, fs_dir_t* out_dir)
{
    /* Check input. */
    if(!out_dir) return -EINVAL;
    if(STRING_IS_NULL_OR_EMPTY(abs_path)) return -EINVAL;
    if(abs_path[0] != '/') return -EINVAL;
    if(out_dir->fd != -1) return -EBUSY;

    /* Start from the filesystem root; from here on every component is opened with
     * O_NOFOLLOW by the shared walker, so a symlink ANYWHERE in the chain is refused,
     * not only at the final component as in fs_dir_open_abs(). */
    int root_fd = _fs_open_eintr("/", FS_DIR_OPEN_FLAGS);
    if(root_fd < 0) return -errno;

    int    final_fd = -1;
    size_t walked   = 0U;
    int    rc       = _fs_walk(root_fd, abs_path + 1, _FS_WALK_OPEN_EXISTING, 0, NULL, &final_fd, &walked);
    if(rc != 0)
    {
        close(root_fd);
        return rc;
    }

    /* "/" itself (or "/./") resolves to the root fd; otherwise the walked leaf. */
    if(walked == 0U)
    {
        final_fd = root_fd;
    }
    else
    {
        close(root_fd);
    }

    /* The caller's expectation applies to the final directory only. */
    rc = fs_dir_verify(&(fs_dir_t){.fd = final_fd}, expect);
    if(rc != 0)
    {
        close(final_fd);
        return rc;
    }

    /* Transfer ownership of the final fd to the output handle. */
    out_dir->fd = final_fd;
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
    if(FS_SYS_FSTAT(dir->fd, &st) != 0) return -errno;

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
    if(FS_SYS_FSTAT(fd, &st) != 0) return -errno;

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
    int fd = _fs_openat_eintr(parent->fd, name, FS_DIR_OPEN_FLAGS, 0);
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
    if(FS_SYS_MKDIRAT(parent->fd, name, create_mode) == 0)
    {
        create_disposition = FS_CREATE_DISPOSITION_CREATED_NEW;

        /*
         * Force the final directory mode explicitly, before opening it.
         * mkdirat() applies the process umask; a restrictive umask can strip the
         * owner's read/search bits and then the O_RDONLY open below would fail.
         * The name-based chmod is within the trusted-parent model already used
         * by the cleanup path; the fstat()-based verification below is what
         * guarantees the exact mode on the fd actually returned.
         */
        if(FS_SYS_FCHMODAT(parent->fd, name, create_mode) != 0)
        {
            int saved_errno = errno;
            _fs_cleanup_created_directory_at(parent->fd, name);
            return -saved_errno;
        }
    }
    else if(errno != EEXIST)
    {
        return -errno;
    }

    /* Open the resulting directory capability, rejecting a final symlink. */
    int fd = _fs_openat_eintr(parent->fd, name, FS_DIR_OPEN_FLAGS, 0);
    if(fd < 0)
    {
        int saved_errno = errno;
        if(create_disposition == FS_CREATE_DISPOSITION_CREATED_NEW) _fs_cleanup_created_directory_at(parent->fd, name);
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

    /* A directory this call did not create may be another creator's, still between its mkdirat() and its fchmodat(). */
    for(int tries = 0; rc == -EACCES && create_disposition == FS_CREATE_DISPOSITION_OPENED_EXISTING && tries < FS_DIR_SETTLE_TRIES; ++tries)
    {
        (void)nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = FS_DIR_SETTLE_NS}, NULL);
        rc = fs_dir_verify(&(fs_dir_t){.fd = fd}, expect);
    }
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
    int    final_fd = -1;
    size_t walked   = 0U;
    rc              = _fs_walk(start->fd, relative_path, _FS_WALK_OPEN_EXISTING, 0, expect, &final_fd, &walked);
    if(rc != 0) return rc;

    if(walked == 0U)
    {
        /* Empty or "."-only paths resolve to the starting capability itself. */
        rc = fs_dir_verify(start, expect);
        if(rc != 0) return rc;
        return _fs_dup_dir_fd(start->fd, &out_dir->fd);
    }

    /* Transfer ownership of the final walked directory fd to the output handle. */
    out_dir->fd = final_fd;
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

    /* Walk relative_path one component at a time, creating what is missing. */
    int    final_fd = -1;
    size_t walked   = 0U;
    rc              = _fs_walk(start->fd, relative_path, _FS_WALK_CREATE_MISSING, create_mode, expect, &final_fd, &walked);
    if(rc != 0) return rc;

    if(walked == 0U)
    {
        /* Empty or "."-only paths resolve to the starting capability itself. */
        rc = fs_dir_verify(start, expect);
        if(rc != 0) return rc;
        return _fs_dup_dir_fd(start->fd, &out_dir->fd);
    }

    /* Transfer ownership of the final walked directory fd to the output handle. */
    out_dir->fd = final_fd;
    return 0;
}

int fs_file_open_read_at(const fs_dir_t* parent, const char* name, const fs_expect_t* expect, int* out_fd)
{
    return _fs_file_open_existing_at(parent, name, expect, out_fd, O_RDONLY);
}

int fs_file_open_rw_at(const fs_dir_t* parent, const char* name, const fs_expect_t* expect, int* out_fd)
{
    return _fs_file_open_existing_at(parent, name, expect, out_fd, O_RDWR);
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
    int fd = _fs_openat_eintr(parent->fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY, create_mode);
    if(fd < 0) return -errno;

    /*
     * Force the final file mode explicitly.
     * This makes the result independent from the caller's current umask.
     */
    if(FS_SYS_FCHMOD(fd, create_mode) != 0)
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
    if(FS_SYS_RENAMEAT(old_parent->fd, old_name, new_parent->fd, new_name) != 0) return -errno;
    return 0;
}

int fs_rename_noreplace_at(const fs_dir_t* old_parent, const char* old_name, const fs_dir_t* new_parent, const char* new_name)
{
    /* Check input. */
    if(!old_parent || old_parent->fd < 0 || !new_parent || new_parent->fd < 0) return -EINVAL;
    if(!fs_component_is_valid(old_name) || !fs_component_is_valid(new_name)) return -EINVAL;

    /* Both parents must already be valid directory capabilities. */
    int rc = fs_dir_verify(old_parent, NULL);
    if(rc != 0) return rc;
    rc = fs_dir_verify(new_parent, NULL);
    if(rc != 0) return rc;

    /* One kernel call: the existence check and the rename are the same operation, so
     * there is no window in which another writer can slip a destination in between. */
    if(FS_SYS_RENAMEAT2(old_parent->fd, old_name, new_parent->fd, new_name, RENAME_NOREPLACE) != 0) return -errno;
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
    if(FS_SYS_UNLINKAT(parent->fd, name, 0) != 0) return -errno;
    return 0;
}

int fs_rmdir_at(const fs_dir_t* parent, const char* name)
{
    /* Check input. */
    if(!parent || parent->fd < 0) return -EINVAL;
    if(!fs_component_is_valid(name)) return -EINVAL;

    /* Parent must already be a valid directory capability. */
    int rc = fs_dir_verify(parent, NULL);
    if(rc != 0) return rc;

    /* Remove exactly one empty child directory; a symlink to a directory is -ENOTDIR. */
    if(FS_SYS_UNLINKAT(parent->fd, name, AT_REMOVEDIR) != 0) return -errno;
    return 0;
}

int fs_dir_fsync(const fs_dir_t* dir)
{
    /* Check input. */
    int rc = fs_dir_verify(dir, NULL);
    if(rc != 0) return rc;

    /* Some filesystems do not support directory fsync; treat EINVAL as success. */
    if(_fs_fsync_eintr(dir->fd) == 0) return 0;
    if(errno == EINVAL) return 0;
    return -errno;
}

int fs_file_fsync(int fd)
{
    /* Check input. */
    int rc = fs_file_verify(fd, NULL);
    if(rc != 0) return rc;

    /* Push regular-file data and metadata to stable storage. */
    if(_fs_fsync_eintr(fd) != 0) return -errno;
    return 0;
}

int fs_file_fdatasync(int fd)
{
    /* Check input. */
    int rc = fs_file_verify(fd, NULL);
    if(rc != 0) return rc;

    /* Push regular-file data, and only the metadata needed to read it back, to stable storage. */
    if(_fs_fdatasync_eintr(fd) != 0) return -errno;
    return 0;
}

int fs_file_write_all(int fd, const void* buf, size_t len)
{
    /* Check input. A null buffer is only acceptable for a zero-length write. */
    if(fd < 0 || (!buf && len > 0) || len > (size_t)SSIZE_MAX) return -EINVAL;

    /* Loop until every byte is accepted: write() may transfer less than asked. */
    const unsigned char* p = buf;
    while(len > 0)
    {
        ssize_t n = FS_SYS_WRITE(fd, p, len);
        if(n < 0)
        {
            if(errno == EINTR) continue;
            return -errno;
        }
        /* write() returns 0 only for a zero-length request; anything else is a fd that makes no progress. */
        if(n == 0) return -EIO;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

int fs_file_pwrite_all(int fd, const void* buf, size_t len, off_t offset)
{
    /* Check input. A null buffer is only acceptable for a zero-length write. */
    if(fd < 0 || (!buf && len > 0) || len > (size_t)SSIZE_MAX || offset < 0) return -EINVAL;

    /* Loop until every byte is accepted: pwrite() may transfer less than asked. */
    const unsigned char* p = buf;
    while(len > 0)
    {
        ssize_t n = FS_SYS_PWRITE(fd, p, len, offset);
        if(n < 0)
        {
            if(errno == EINTR) continue;
            return -errno;
        }
        if(n == 0) return -EIO;
        p      += n;
        len    -= (size_t)n;
        offset += n;
    }
    return 0;
}

int fs_file_read_all(int fd, void* buf, size_t len)
{
    /* Check input. A null buffer is only acceptable for a zero-length read. */
    if(fd < 0 || (!buf && len > 0) || len > (size_t)SSIZE_MAX) return -EINVAL;

    /* Loop until every byte has arrived: read() may transfer less than asked. */
    unsigned char* p = buf;
    while(len > 0)
    {
        ssize_t n = FS_SYS_READ(fd, p, len);
        if(n < 0)
        {
            if(errno == EINTR) continue;
            return -errno;
        }
        /* End of file before the request was satisfied. */
        if(n == 0) return -ENODATA;
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

int fs_file_pread_all(int fd, void* buf, size_t len, off_t offset)
{
    /* Check input. A null buffer is only acceptable for a zero-length read. */
    if(fd < 0 || (!buf && len > 0) || len > (size_t)SSIZE_MAX || offset < 0) return -EINVAL;

    /* Loop until every byte has arrived: pread() may transfer less than asked. */
    unsigned char* p = buf;
    while(len > 0)
    {
        ssize_t n = FS_SYS_PREAD(fd, p, len, offset);
        if(n < 0)
        {
            if(errno == EINTR) continue;
            return -errno;
        }
        if(n == 0) return -ENODATA;
        p      += n;
        len    -= (size_t)n;
        offset += n;
    }
    return 0;
}

int fs_symlink_at(const fs_dir_t* parent, const char* target, const char* name)
{
    /* Check input. The target is stored verbatim, so only emptiness is refused. */
    if(!parent || parent->fd < 0) return -EINVAL;
    if(STRING_IS_NULL_OR_EMPTY(target)) return -EINVAL;
    if(!fs_component_is_valid(name)) return -EINVAL;

    /* Parent must already be a valid directory capability. */
    int rc = fs_dir_verify(parent, NULL);
    if(rc != 0) return rc;

    /* Create exactly one link entry; an existing entry of any kind is -EEXIST. */
    if(FS_SYS_SYMLINKAT(target, parent->fd, name) != 0) return -errno;
    return 0;
}

int fs_symlink_replace_at(const fs_dir_t* parent, const char* target, const char* name)
{
    char tmp_name[NAME_MAX + 1];

    /* Check input. */
    if(!parent || parent->fd < 0) return -EINVAL;
    if(STRING_IS_NULL_OR_EMPTY(target)) return -EINVAL;
    if(!fs_component_is_valid(name)) return -EINVAL;

    /* Parent must already be a valid directory capability. */
    int rc = fs_dir_verify(parent, NULL);
    if(rc != 0) return rc;
    rc = _fs_tmp_name(name, tmp_name, sizeof(tmp_name));
    if(rc != 0) return rc;

    /* A temporary link left by an earlier crash would make symlinkat() fail with EEXIST. */
    if(FS_SYS_UNLINKAT(parent->fd, tmp_name, 0) != 0 && errno != ENOENT) return -errno;

    /* Build the new link beside the old one, then swap it in with one rename. */
    if(FS_SYS_SYMLINKAT(target, parent->fd, tmp_name) != 0) return -errno;
    if(FS_SYS_RENAMEAT(parent->fd, tmp_name, parent->fd, name) != 0)
    {
        int saved_errno = errno;
        _fs_cleanup_created_file_at(parent->fd, tmp_name);
        return -saved_errno;
    }

    /* The switch is not a switch until the directory entry is on stable storage. */
    return fs_dir_fsync(parent);
}

int fs_write_file_atomic_at(const fs_dir_t* parent, const char* name, const void* buf, size_t len, mode_t mode)
{
    char tmp_name[NAME_MAX + 1];
    int  fd = -1;

    /* Check input. A null buffer is only acceptable for an empty file. */
    if(!parent || parent->fd < 0) return -EINVAL;
    if(!fs_component_is_valid(name)) return -EINVAL;
    if((!buf && len > 0) || len > (size_t)SSIZE_MAX) return -EINVAL;
    if(!_fs_mode_is_valid(mode)) return -EINVAL;

    /* Parent must already be a valid directory capability. */
    int rc = fs_dir_verify(parent, NULL);
    if(rc != 0) return rc;
    rc = _fs_tmp_name(name, tmp_name, sizeof(tmp_name));
    if(rc != 0) return rc;

    /* A temporary file left by an earlier crash would block the O_EXCL create. */
    if(FS_SYS_UNLINKAT(parent->fd, tmp_name, 0) != 0 && errno != ENOENT) return -errno;

    /* Create, fill and sync the temporary file; only then does it get the real name. */
    rc = fs_file_create_write_new_at(parent, tmp_name, mode, NULL, &fd);
    if(rc != 0) return rc;
    rc = fs_file_write_all(fd, buf, len);
    if(rc != 0) goto fail;
    rc = fs_file_fsync(fd);
    if(rc != 0) goto fail;
    (void)close(fd);
    fd = -1;

    if(FS_SYS_RENAMEAT(parent->fd, tmp_name, parent->fd, name) != 0)
    {
        rc = -errno;
        goto fail;
    }

    /* The rename is not durable until the directory entry is on stable storage. */
    return fs_dir_fsync(parent);

fail:
    if(fd >= 0) (void)close(fd);
    _fs_cleanup_created_file_at(parent->fd, tmp_name);
    return rc;
}

int fs_link_tree_at(const fs_dir_t* src_parent, const char* src_name, const fs_dir_t* dst_parent, const char* dst_name)
{
    /* Check input. */
    if(!src_parent || src_parent->fd < 0 || !dst_parent || dst_parent->fd < 0) return -EINVAL;
    if(!fs_component_is_valid(src_name) || !fs_component_is_valid(dst_name)) return -EINVAL;

    /* Both parents must already be valid directory capabilities. */
    int rc = fs_dir_verify(src_parent, NULL);
    if(rc != 0) return rc;
    rc = fs_dir_verify(dst_parent, NULL);
    if(rc != 0) return rc;

    /* The source must be a real directory; a symlink to one is never followed. */
    int src_fd = _fs_openat_eintr(src_parent->fd, src_name, FS_DIR_OPEN_FLAGS, 0);
    if(src_fd < 0) return -errno;

    rc = _fs_link_tree(src_fd, dst_parent->fd, dst_name, 0U);
    (void)close(src_fd);
    return rc;
}

/*****************************************************************************************************************************************
 * PRIVATE FUNCTIONS DEFINITIONS
 *****************************************************************************************************************************************
 */

static int _fs_tmp_name(const char* name, char* out, size_t out_size)
{
    /* sizeof(FS_TMP_SUFFIX) counts the terminator, so the sum is the bytes actually stored. */
    size_t len = strlen(name);
    if(len + sizeof(FS_TMP_SUFFIX) > out_size) return -ENAMETOOLONG;
    memcpy(out, name, len);
    memcpy(out + len, FS_TMP_SUFFIX, sizeof(FS_TMP_SUFFIX));
    return 0;
}

static int _fs_link_tree(int src_fd, int dst_parent_fd, const char* dst_name, unsigned depth)
{
    struct stat st;

    /* Bound the recursion: each level holds two directory fds. */
    if(depth > FS_LINK_TREE_MAX_DEPTH) return -ELOOP;
    if(FS_SYS_FSTAT(src_fd, &st) != 0) return -errno;
    mode_t mode = st.st_mode & FS_MODE_MASK;

    /* The mirror must not pre-exist (EEXIST propagates). It is created owner-writable so entries
     * can be added, and receives the source's exact bits once it is complete. */
    if(FS_SYS_MKDIRAT(dst_parent_fd, dst_name, mode | S_IRWXU) != 0) return -errno;
    int dst_fd = _fs_openat_eintr(dst_parent_fd, dst_name, FS_DIR_OPEN_FLAGS, 0);
    if(dst_fd < 0) return -errno;

    int rc = _fs_link_entries(src_fd, dst_fd, depth);
    if(rc == 0 && FS_SYS_FCHMOD(dst_fd, mode) != 0) rc = -errno;
    (void)close(dst_fd);
    return rc;
}

static int _fs_link_entries(int src_fd, int dst_fd, unsigned depth)
{
    /* fdopendir() takes ownership of its fd; scan through a duplicate so src_fd stays usable. */
    int scan_fd = -1;
    int rc      = _fs_dup_dir_fd(src_fd, &scan_fd);
    if(rc != 0) return rc;
    DIR* dir = FS_SYS_FDOPENDIR(scan_fd);
    if(dir == NULL)
    {
        int saved_errno = errno;
        (void)close(scan_fd);
        return -saved_errno;
    }
    rewinddir(dir);

    for(;;)
    {
        /* readdir() reports both the end and an error with NULL; only errno tells them apart. */
        errno                = 0;
        struct dirent* entry = FS_SYS_READDIR(dir);
        if(entry == NULL)
        {
            if(errno != 0) rc = -errno;
            break;
        }
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        rc = _fs_link_entry(src_fd, dst_fd, entry->d_name, depth);
        if(rc != 0) break;
    }
    (void)closedir(dir);
    return rc;
}

static int _fs_link_entry(int src_fd, int dst_fd, const char* name, unsigned depth)
{
    struct stat st;
    if(FS_SYS_FSTATAT(src_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return -errno;

    if(S_ISDIR(st.st_mode))
    {
        int child_fd = _fs_openat_eintr(src_fd, name, FS_DIR_OPEN_FLAGS, 0);
        if(child_fd < 0) return -errno;
        int rc = _fs_link_tree(child_fd, dst_fd, name, depth + 1U);
        (void)close(child_fd);
        return rc;
    }
    if(S_ISREG(st.st_mode))
    {
        /* A hard link shares the inode: no bytes move, and EXDEV names the wrong filesystem. */
        if(FS_SYS_LINKAT(src_fd, name, dst_fd, name, 0) != 0) return -errno;
        return 0;
    }
    if(S_ISLNK(st.st_mode))
    {
        char    target[PATH_MAX];
        ssize_t n = FS_SYS_READLINKAT(src_fd, name, target, sizeof(target));
        if(n < 0) return -errno;
        if((size_t)n >= sizeof(target)) return -ENAMETOOLONG;
        target[n] = '\0';
        if(FS_SYS_SYMLINKAT(target, dst_fd, name) != 0) return -errno;
        return 0;
    }

    /* FIFOs, sockets and devices have no place in a mirrored data tree. */
    return -EINVAL;
}

static int _fs_verify_stat(const struct stat* st, enum _fs_verify_object_kind object_kind, const fs_expect_t* expect)
{
    /* Verify the object type first; mode/uid/gid checks happen only afterwards. */
    if(object_kind == _FS_VERIFY_OBJECT_DIRECTORY)
    {
        if(!S_ISDIR(st->st_mode)) return -ENOTDIR;
    }
    else
    {
        if(S_ISDIR(st->st_mode)) return -EISDIR;
        if(!S_ISREG(st->st_mode)) return -EINVAL;
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
    if(len >= out_component_size) return -ENAMETOOLONG;

    /* Copy the parsed component and NUL-terminate it. */
    memcpy(out_component, start, len);
    out_component[len] = '\0';

    /* Publish the new scan position and the "component present" state. */
    *cursor    = p;
    *out_state = _FS_COMPONENT_STATE_VALUE;
    return 0;
}

static int _fs_walk(int start_fd, const char* relative_path, enum _fs_walk_mode mode, mode_t create_mode, const fs_expect_t* each_expect,
                    int* out_fd, size_t* out_walked)
{
    const char* cursor     = relative_path;
    int         parent_fd  = start_fd;
    int         current_fd = -1;
    size_t      walked     = 0U;
    int         rc;

    *out_fd     = -1;
    *out_walked = 0U;

    for(;;)
    {
        /* Parse the next relative path component. */
        char                     component[NAME_MAX + 1];
        enum _fs_component_state component_state = _FS_COMPONENT_STATE_END;
        rc                                       = _fs_component_next(&cursor, component, sizeof(component), &component_state);
        if(rc != 0) break;

        /* No more components means the walk is complete. */
        if(component_state == _FS_COMPONENT_STATE_END) break;

        /* Ignore "." components so callers may pass "./database/". */
        if(strcmp(component, ".") == 0) continue;

        /* Reject ".." so the walk cannot escape the trusted starting point. */
        if(strcmp(component, "..") == 0)
        {
            rc = -EINVAL;
            break;
        }

        int next_fd = -1;
        if(mode == _FS_WALK_OPEN_EXISTING)
        {
            /* Open exactly one child directory, rejecting a final symlink. */
            next_fd = _fs_openat_eintr(parent_fd, component, FS_DIR_OPEN_FLAGS, 0);
            if(next_fd < 0)
            {
                rc = -errno;
                break;
            }

            /* Verify the opened child against the caller's policy. */
            rc = fs_dir_verify(&(fs_dir_t){.fd = next_fd}, each_expect);
            if(rc != 0)
            {
                close(next_fd);
                break;
            }
        }
        else
        {
            /* Create-or-open exactly one child directory component. */
            fs_dir_t next_dir;
            fs_dir_init(&next_dir);
            rc = fs_dir_create_at(&(fs_dir_t){.fd = parent_fd}, component, create_mode, each_expect, &next_dir, NULL);
            if(rc != 0) break;
            next_fd = next_dir.fd;
        }

        /* Move the walk forward and release the previous transient directory fd. */
        if(current_fd >= 0) close(current_fd);
        current_fd = next_fd;
        parent_fd  = next_fd;
        ++walked;
    }

    if(rc != 0)
    {
        /* Failure during walk must not leak any transient fd. */
        if(current_fd >= 0) close(current_fd);
        return rc;
    }

    *out_fd     = current_fd;
    *out_walked = walked;
    return 0;
}

static int _fs_file_open_existing_at(const fs_dir_t* parent, const char* name, const fs_expect_t* expect, int* out_fd, int access_flags)
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
    int fd = _fs_openat_eintr(parent->fd, name, access_flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY, 0);
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
    int status_flags = FS_SYS_FCNTL(fd, F_GETFL, 0);
    if(status_flags < 0)
    {
        int saved_errno = errno;
        close(fd);
        return -saved_errno;
    }
    if((status_flags & O_NONBLOCK) != 0 && FS_SYS_FCNTL(fd, F_SETFL, status_flags & ~O_NONBLOCK) != 0)
    {
        int saved_errno = errno;
        close(fd);
        return -saved_errno;
    }

    /* Transfer ownership of the opened fd to the caller. */
    *out_fd = fd;
    return 0;
}

static int _fs_dup_dir_fd(int fd, int* out_fd)
{
    /* Duplicate with close-on-exec preserved as an explicit capability copy. */
    int dup_fd = FS_SYS_FCNTL(fd, F_DUPFD_CLOEXEC, 0);
    if(dup_fd < 0) return -errno;

    /* Transfer the duplicated fd to the caller. */
    *out_fd = dup_fd;
    return 0;
}

static int _fs_open_eintr(const char* path, int flags)
{
    int fd;
    do
    {
        fd = FS_SYS_OPEN(path, flags);
    } while(fd < 0 && errno == EINTR);
    return fd;
}

static int _fs_openat_eintr(int dirfd, const char* name, int flags, mode_t mode)
{
    int fd;
    do
    {
        fd = FS_SYS_OPENAT(dirfd, name, flags, mode);
    } while(fd < 0 && errno == EINTR);
    return fd;
}

static int _fs_fsync_eintr(int fd)
{
    int rc;
    do
    {
        rc = FS_SYS_FSYNC(fd);
    } while(rc != 0 && errno == EINTR);
    return rc;
}

static int _fs_fdatasync_eintr(int fd)
{
    int rc;
    do
    {
        rc = FS_SYS_FDATASYNC(fd);
    } while(rc != 0 && errno == EINTR);
    return rc;
}

static int _fs_fd_verify_cloexec(int fd)
{
    /* Read descriptor flags from the kernel. */
    int flags = FS_SYS_FCNTL(fd, F_GETFD, 0);
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
    /* Best-effort cleanup for a newly-created file after post-create failure. The
     * caller's errno is preserved: this runs between the failure and the return. */
    int saved_errno = errno;
    (void)FS_SYS_UNLINKAT(parent_fd, name, 0);
    errno = saved_errno;
}

static void _fs_cleanup_created_directory_at(int parent_fd, const char* name)
{
    /* Best-effort cleanup for a newly-created directory after post-create failure. */
    int saved_errno = errno;
    (void)FS_SYS_UNLINKAT(parent_fd, name, AT_REMOVEDIR);
    errno = saved_errno;
}
