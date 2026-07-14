/**
 * @file fsutil.h
 * @brief Strict capability-oriented filesystem interface.
 *
 * This header exposes a dirfd-based API for code that wants explicit control
 * over filesystem traversal, verification, ownership, and durability.
 * 
 * @author  Roman Horshkov <github.com/RomanHorshkov>
 * @date    2026
 * (c) 2026
 */

#ifndef FSUTIL_H
#define FSUTIL_H

/*****************************************************************************************************************************************
 * PUBLIC INCLUDES
 *****************************************************************************************************************************************
 */

#include <stddef.h>    /* for size_t */
#include <sys/stat.h>  /* for stat, mode_t */
#include <sys/types.h> /* for uid_t, gid_t */


/*****************************************************************************************************************************************
 * PUBLIC FUNCTIONS PROTOTYPES
 *****************************************************************************************************************************************
 */

// ------------------------ Capability-oriented API --------------------------
/**
 * OVERVIEW
 *
 * This API is intentionally built around directory file descriptors ("dirfds")
 * treated as capabilities.
 * Instead of constructing full paths with string concatenation, callers open a
 * trusted starting directory once and then operate on one path component at a
 * time with *at()-style helpers.
 *
 * TARGET PLATFORM
 *
 * - this interface targets POSIX.1-2008 / Linux-like systems
 * - it is intentionally not a portable Windows filesystem abstraction
 *
 * SECURITY MODEL
 *
 * - bootstrap once from a trusted directory capability
 * - walk relative paths component-by-component
 * - reject symlinks during capability acquisition
 * - verify the opened object after acquisition
 * - keep durability policy explicit:
 *   create/rename/unlink helpers do not fsync parent directories implicitly
 *
 * PATH MODEL
 *
 * - single-component helpers accept exactly one component:
 *   no '/', no ".", no ".."
 * - path-walk helpers accept relative paths only
 * - absolute paths are rejected
 * - "." components are ignored during walks
 * - ".." components are rejected during walks
 * - repeated '/' characters in walk paths are collapsed naturally
 * - empty walk paths and "."-only walk paths resolve to the starting
 *   capability itself
 *
 * MODE MODEL
 *
 * - mode values in this API mean permission/special bits only
 * - accepted mode bits are those in FS_MODE_MASK
 * - object type bits such as S_IFDIR or S_IFREG are not accepted here
 * - when an expectation uses FS_EXPECT_ANY_MODE, mode checking is disabled
 * - exact mode 0000 is therefore expressible and verifiable
 * - when a create helper receives an invalid mode, it fails with -EINVAL
 *
 * FD / OWNERSHIP MODEL
 *
 * - every fs_dir_t must be initialized with fs_dir_init()
 * - every fs_dir_t output parameter must be in the closed state on entry
 *   (dir->fd == -1), otherwise the helper fails with -EBUSY
 * - successful directory-returning helpers transfer fd ownership to the caller
 * - callers release that ownership with fs_dir_close()
 * - verify helpers require FD_CLOEXEC to be set on the fd
 *
 * DURABILITY MODEL
 *
 * - create helpers make the object exist in the filesystem namespace, but do
 *   not fsync the parent directory automatically
 * - rename/unlink helpers also do not fsync automatically
 * - caller decides exactly where durability barriers belong
 * - use fs_file_fsync() and fs_dir_fsync() explicitly when crash consistency
 *   matters
 *
 * FAILURE CLEANUP MODEL
 *
 * - create helpers may perform best-effort cleanup if post-create steps fail
 *   (for example fchmod() or verification after creation)
 * - this cleanup is name-based under the supplied parent capability
 * - it is intended for trusted/private parent directories, which is the
 *   expected usage model of this API
 *
 * TYPICAL USAGE
 *
 * 1. Initialize handles with fs_dir_init().
 * 2. Open a trusted root capability with fs_dir_open_cwd() or keep an already
 *    trusted directory fd.
 * 3. Build explicit expectations with fs_expect_make() or
 *    fs_expect_private().
 * 4. Acquire child capabilities with fs_dir_open_at(), fs_dir_create_at(),
 *    fs_dir_walk_open(), or fs_dir_walk_create().
 * 5. Open or create files relative to those directory capabilities.
 * 6. Perform explicit fsync steps when the calling protocol requires
 *    persistence guarantees.
 *
 * PATH INPUT RULE OF THUMB
 *
 * - use fs_dir_walk_open() / fs_dir_walk_create() for relative paths such as
 *   "./demo_db", "./demo_db/meta", or "./demo_db/store"
 * - use fs_dir_open_at() / fs_dir_create_at() for single child names such as
 *   "meta", "store", "tmp", or ".db_mdb_install_marker"
 * - do not pass multi-component strings to single-component helpers
 *
 * INTEGRATION WITH STATIC PATH MACROS
 *
 * A configuration style like:
 * - `DB_DIR_ROOT  -> "./demo_db"`
 * - `DB_DIR_META  -> "./demo_db/meta"`
 * - `DB_DIR_STORE -> "./demo_db/store"`
 * - `DB_DIR_TMP   -> "./demo_db/tmp"`
 *
 * maps naturally to the walk helpers:
 * - pass `DB_DIR_ROOT`, `DB_DIR_META`, `DB_DIR_STORE`, or `DB_DIR_TMP`
 *   to fs_dir_walk_open() / fs_dir_walk_create()
 * - once a root capability is acquired, use single-component helpers for leaf
 *   names such as `"meta"`, `"store"`, `"tmp"`, or `".db_mdb_install_marker"`
 * - a full multi-component file path macro should be split into:
 *   "walk to parent directory" + "operate on final basename"
 *
 * CONCURRENCY NOTES
 *
 * - this layer does not provide locking
 * - kernel-level atomic operations such as mkdirat(O_EXCL-style semantics via
 *   mkdirat on a single name), openat(O_CREAT|O_EXCL), and renameat are
 *   preserved
 * - callers must still define the higher-level protocol when multiple writers
 *   operate in the same namespace
 * - best-effort cleanup after failed creates is name-based and is intended for
 *   trusted/private parent directories, not adversarial shared namespaces
 *
 * MINIMAL EXAMPLE
 *
 * @code{.c}
 * int rc;
 * fs_dir_t cwd;
 * fs_dir_t dbroot;
 * fs_dir_t data;
 * fs_expect_t dir_expect = fs_expect_private(0700);
 *
 * fs_dir_init(&cwd);
 * fs_dir_init(&dbroot);
 * fs_dir_init(&data);
 *
 * rc = fs_dir_open_cwd(&cwd);
 * if(rc != 0) goto cleanup;
 *
 * rc = fs_dir_walk_create(&cwd, "./database", 0700, &dir_expect, &dbroot);
 * if(rc != 0) goto cleanup;
 *
 * rc = fs_dir_create_at(&dbroot, "data", 0700, &dir_expect, &data, NULL);
 * if(rc != 0) goto cleanup;
 *
 * rc = fs_dir_fsync(&dbroot);
 * if(rc != 0) goto cleanup;
 *
 * cleanup:
 * fs_dir_close(&data);
 * fs_dir_close(&dbroot);
 * fs_dir_close(&cwd);
 * @endcode
 */

/*****************************************************************************************************************************************
 * PUBLIC STRUCTURED TYPEDEFS
 *****************************************************************************************************************************************
 */

/**
 * @brief Directory capability.
 *
 * A directory capability is an owned directory file descriptor.
 *
 * Invariants expected by this API:
 * - the fd refers to a directory
 * - the fd has FD_CLOEXEC set
 * - the structure owns the fd when fd >= 0
 * - the structure is in the closed state when fd == -1
 *
 * Initialize with fs_dir_init() before first use.
 * Release ownership with fs_dir_close() when done.
 */
typedef struct fs_dir
{
    int fd;
} fs_dir_t;

/*****************************************************************************************************************************************
 * PUBLIC ENUMERATED TYPEDEFS
 *****************************************************************************************************************************************
 */

/**
 * @brief Result disposition for create-or-open style helpers.
 *
 * This is used instead of a boolean so call sites remain explicit.
 */
enum fs_create_disposition
{
    FS_CREATE_DISPOSITION_OPENED_EXISTING = 0,
    FS_CREATE_DISPOSITION_CREATED_NEW
};

/**
 * @brief Expected metadata for a filesystem object.
 *
 * Any field may be ignored:
 * - mode == FS_EXPECT_ANY_MODE => do not check permission bits
 * - uid == (uid_t)-1    => do not check owner uid
 * - gid == (gid_t)-1    => do not check owner gid
 *
 * When a field is not ignored, the check is exact.
 *
 * For `mode`, only bits in FS_MODE_MASK are meaningful.
 * Object type bits are not accepted.
 */
typedef struct fs_expect
{
    mode_t mode;
    uid_t  uid;
    gid_t  gid;
} fs_expect_t;

/*****************************************************************************************************************************************
 * PUBLIC DEFINES
 *****************************************************************************************************************************************
 */

#define FS_EXPECT_ANY_MODE ((mode_t) - 1)
#define FS_EXPECT_ANY_UID  ((uid_t) - 1)
#define FS_EXPECT_ANY_GID  ((gid_t) - 1)
#define FS_MODE_MASK       ((mode_t)07777)

/**
 * @brief Build an expectation object explicitly.
 *
 * This is the most explicit constructor:
 * caller chooses exact mode, uid, and gid checks directly.
 *
 * The returned structure is not validated here.
 * Validation happens when the expectation is consumed by a verify or create
 * helper.
 */
fs_expect_t fs_expect_make(mode_t mode, uid_t uid, gid_t gid);

/**
 * @brief Build an expectation object for the current euid/egid.
 *
 * Typical use for private DB directories and marker files:
 * `fs_expect_private(0700)` or `fs_expect_private(0600)`.
 *
 * The returned structure is not validated here.
 * Validation happens when the expectation is consumed by a verify or create
 * helper.
 */
fs_expect_t fs_expect_private(mode_t mode);

/**
 * @brief Initialize a directory capability to the closed state.
 *
 * After this call, dir->fd is guaranteed to be exactly -1.
 * This function is idempotent for null pointers.
 */
void fs_dir_init(fs_dir_t* dir);

/**
 * @brief Close a directory capability and reset it.
 *
 * Null pointers and already-closed handles are accepted.
 * On return, dir->fd is guaranteed to be exactly -1.
 */
void fs_dir_close(fs_dir_t* dir);

/**
 * @brief Check whether a string is a valid single path component.
 *
 * Valid means:
 * - non-null
 * - non-empty
 * - no '/' characters
 * - not "." and not ".."
 *
 * @return 1 if valid, 0 if invalid.
 *
 * This helper is intended for names passed to single-component operations such
 * as fs_dir_open_at(), fs_dir_create_at(), fs_file_open_read_at(), and
 * fs_file_create_write_new_at().
 */
int fs_component_is_valid(const char* name);

/**
 * @brief Open an absolute directory path as a capability root.
 *
 * The absolute-path sibling of fs_dir_open_cwd(): the intended bootstrap for
 * security-sensitive code whose root lives at a fixed absolute location (for
 * example a service data directory such as /srv/app/data). After this call the
 * rest of the work is done with dirfds and the single-component *at helpers.
 *
 * @param abs_path Absolute directory path. Must begin with '/'.
 * @param expect Expected metadata. If null, only "is a directory" is checked.
 * @param out_dir Receives the opened directory capability.
 * @return 0 on success, negative errno on failure.
 *
 * Path rules:
 * - `abs_path` must be absolute (leading '/').
 * - The final component is opened with O_NOFOLLOW: a symlink at the final
 *   component is rejected. Intermediate components are resolved normally, so
 *   callers are expected to own or trust the parent chain (typically it is
 *   provisioned once with known ownership/mode).
 *
 * Contract:
 * - `out_dir` must already be initialized and closed exactly
 *   (`out_dir->fd == -1`).
 *
 * Ownership:
 * - on success, ownership of the opened fd is transferred to `out_dir`.
 * - caller must later call fs_dir_close().
 *
 * Failure cases:
 * - -EINVAL if `out_dir` is null, `abs_path` is null/empty, or not absolute.
 * - -EBUSY if `out_dir` already owns an fd.
 * - -ELOOP if the final component is a symlink and the kernel reports it.
 * - -ENOTDIR / -EACCES if verification fails.
 * - other negative errno from open(), fcntl(), or fstat().
 */
int fs_dir_open_abs(const char* abs_path, const fs_expect_t* expect, fs_dir_t* out_dir);

/**
 * @brief Open the current working directory as a capability root.
 *
 * This is the intended bootstrap for security-sensitive code that wants to do
 * the rest of the filesystem work with dirfds and *at() calls.
 *
 * @param out_dir Receives the opened directory capability.
 * @return 0 on success, negative errno on failure.
 *
 * Contract:
 * - `out_dir` must not be null
 * - `out_dir` must already be initialized and closed exactly
 *   (`out_dir->fd == -1`)
 *
 * Ownership:
 * - on success, ownership of the opened fd is transferred to `out_dir`
 * - caller must later call fs_dir_close()
 *
 * Failure cases:
 * - -EINVAL if `out_dir` is null
 * - -EBUSY if `out_dir` already owns an fd
 * - other negative errno from open()
 */
int fs_dir_open_cwd(fs_dir_t* out_dir);

/**
 * @brief Verify that a directory capability matches the expected metadata.
 *
 * @param dir Directory capability to verify.
 * @param expect Expected metadata. If null, only "is a directory" is checked.
 * @return 0 on success, negative errno on failure.
 *
 * Verification performed:
 * - fd must be valid
 * - fd must have FD_CLOEXEC set
 * - fd must refer to a directory
 * - if `expect != NULL`, mode/uid/gid checks are applied exactly
 *
 * Mode semantics:
 * - `expect->mode == FS_EXPECT_ANY_MODE` disables mode checking
 * - otherwise only FS_MODE_MASK bits are accepted
 *
 * Typical failure cases:
 * - -EINVAL for invalid inputs, invalid mode expectations, or missing
 *   FD_CLOEXEC
 * - -ENOTDIR if the fd does not refer to a directory
 * - -EACCES if metadata does not match the expectation
 * - other negative errno from fcntl() or fstat()
 */
int fs_dir_verify(const fs_dir_t* dir, const fs_expect_t* expect);

/**
 * @brief Verify that an open fd is a regular file with the expected metadata.
 *
 * @param fd Open file descriptor.
 * @param expect Expected metadata. If null, only "is a regular file" is checked.
 * @return 0 on success, negative errno on failure.
 *
 * Verification performed:
 * - fd must be valid
 * - fd must have FD_CLOEXEC set
 * - fd must refer to a regular file
 * - if `expect != NULL`, mode/uid/gid checks are applied exactly
 *
 * Mode semantics:
 * - `expect->mode == FS_EXPECT_ANY_MODE` disables mode checking
 * - otherwise only FS_MODE_MASK bits are accepted
 *
 * Typical failure cases:
 * - -EINVAL for invalid inputs, invalid mode expectations, non-regular
 *   non-directory objects, or missing FD_CLOEXEC
 * - -EISDIR if the fd refers to a directory
 * - -EACCES if metadata does not match the expectation
 * - other negative errno from fcntl() or fstat()
 */
int fs_file_verify(int fd, const fs_expect_t* expect);

/**
 * @brief Open an existing directory under a parent capability.
 *
 * @param parent Parent directory capability.
 * @param name Single path component, not a multi-component path.
 * @param expect Expected metadata. If null, only directory type is checked.
 * @param out_dir Receives the opened directory capability.
 * @return 0 on success, negative errno on failure.
 *
 * Path rules:
 * - `name` must be a single path component
 * - symlinks are rejected on the final component
 *
 * Contract:
 * - `parent` must be a valid directory capability
 * - `out_dir` must already be initialized and closed exactly
 *   (`out_dir->fd == -1`)
 *
 * Ownership:
 * - on success, ownership of the opened child fd is transferred to `out_dir`
 * - caller must later call fs_dir_close()
 *
 * Typical failure cases:
 * - -EINVAL for invalid inputs or invalid component names
 * - -EBUSY if `out_dir` already owns an fd
 * - -ELOOP if the final component is a symlink and the kernel reports it
 * - -ENOTDIR / -EACCES if verification fails
 * - other negative errno from openat(), fcntl(), or fstat()
 */
int fs_dir_open_at(const fs_dir_t* parent, const char* name, const fs_expect_t* expect, fs_dir_t* out_dir);

/**
 * @brief Create or open a single directory component under a parent capability.
 *
 * @param parent Parent directory capability.
 * @param name Single path component, not a multi-component path.
 * @param create_mode Final mode requested for a newly-created directory.
 * @param expect Expected metadata for the resulting directory.
 * If null, only directory type is checked after open.
 * @param out_dir Receives the opened directory capability.
 * @param out_disposition Optional result disposition.
 * If not null, receives whether the helper created a new directory or opened
 * an existing one.
 * @return 0 on success, negative errno on failure.
 *
 * Behavior:
 * - attempts mkdirat() first
 * - if the directory already exists, it is opened and verified
 * - if a directory is newly created, the helper may call fchmod() so the final
 *   mode is explicit and not silently left to the process umask
 *
 * Path rules:
 * - `name` must be a single path component
 * - symlinks are rejected on the final component
 *
 * Mode rules:
 * - `create_mode` must use only bits in FS_MODE_MASK
 *
 * Output contract:
 * - `out_dir` must already be initialized and closed exactly
 *   (`out_dir->fd == -1`)
 * - on success, ownership of the opened child fd is transferred to `out_dir`
 * - caller must later call fs_dir_close()
 *
 * Durability:
 * - this function does not fsync the parent directory automatically
 *
 * Failure cleanup:
 * - if post-create steps fail after a brand-new directory was created, the
 *   helper may attempt best-effort cleanup with unlinkat(..., AT_REMOVEDIR)
 *
 * Typical failure cases:
 * - -EINVAL for invalid inputs, invalid component names, or invalid mode bits
 * - -EBUSY if `out_dir` already owns an fd
 * - -ENOTDIR / -EACCES if verification fails
 * - other negative errno from mkdirat(), openat(), fchmod(), fcntl(), or
 *   fstat()
 */
int fs_dir_create_at(const fs_dir_t* parent, const char* name, mode_t create_mode, const fs_expect_t* expect, fs_dir_t* out_dir,
                     enum fs_create_disposition* out_disposition);

/**
 * @brief Walk an existing relative directory path from a starting capability.
 *
 * @param start Starting directory capability.
 * @param relative_path Relative path such as "./database/".
 * @param expect Expected metadata applied to every walked directory component.
 * If null, only directory type is checked.
 * @param out_dir Receives the opened directory capability for the final component.
 * @return 0 on success, negative errno on failure.
 *
 * Rules:
 * - absolute paths are rejected
 * - "." components are ignored
 * - ".." components are rejected
 * - symlinks are rejected at each component
 * - empty or "."-only paths resolve to a verified duplicate of `start`
 *
 * Contract:
 * - `start` must be a valid directory capability
 * - `relative_path` must not be null
 * - `out_dir` must already be initialized and closed exactly
 *   (`out_dir->fd == -1`)
 *
 * Ownership:
 * - on success, ownership of the final directory fd is transferred to
 *   `out_dir`
 * - when the path resolves to `start`, the returned fd is a duplicated fd, not
 *   the original one
 *
 * Typical failure cases:
 * - -EINVAL for null inputs, absolute paths, ".." components, or missing
 *   FD_CLOEXEC during verification
 * - -EBUSY if `out_dir` already owns an fd
 * - -ENOTDIR / -EACCES if a walked component fails verification
 * - other negative errno from openat(), fcntl(), or fstat()
 */
int fs_dir_walk_open(const fs_dir_t* start, const char* relative_path, const fs_expect_t* expect, fs_dir_t* out_dir);

/**
 * @brief Walk a relative directory path, creating missing components as needed.
 *
 * @param start Starting directory capability.
 * @param relative_path Relative path such as "./database/".
 * @param create_mode Final mode requested for each newly-created component.
 * @param expect Expected metadata applied to every walked directory component.
 * If null, only directory type is checked after open.
 * @param out_dir Receives the opened directory capability for the final component.
 * @return 0 on success, negative errno on failure.
 *
 * Behavior:
 * - walks one component at a time from `start`
 * - ignores "." components
 * - rejects ".." components
 * - rejects absolute paths
 * - creates missing components with fs_dir_create_at()
 * - empty or "."-only paths resolve to a verified duplicate of `start`
 *
 * Mode rules:
 * - `create_mode` must use only bits in FS_MODE_MASK
 *
 * Output contract:
 * - `out_dir` must already be initialized and closed exactly
 *   (`out_dir->fd == -1`)
 * - on success, ownership of the final directory fd is transferred to
 *   `out_dir`
 *
 * Durability:
 * - this function does not fsync parent directories automatically
 *
 * Typical failure cases:
 * - -EINVAL for null inputs, absolute paths, ".." components, or invalid
 *   mode bits
 * - -EBUSY if `out_dir` already owns an fd
 * - other negative errno propagated from fs_dir_create_at()
 */
int fs_dir_walk_create(const fs_dir_t* start, const char* relative_path, mode_t create_mode, const fs_expect_t* expect, fs_dir_t* out_dir);

/**
 * @brief Open an existing regular file for read-only access under a parent dir.
 *
 * @param parent Parent directory capability.
 * @param name Single path component, not a multi-component path.
 * @param expect Expected metadata. If null, only regular-file type is checked.
 * @param out_fd Receives the opened fd.
 * @return 0 on success, negative errno on failure.
 *
 * Path rules:
 * - `name` must be a single path component
 * - symlinks are rejected on the final component
 *
 * Contract:
 * - `parent` must be a valid directory capability
 *
 * Ownership:
 * - on success, ownership of the opened fd is transferred to the caller
 * - caller must close *out_fd
 * - if `out_fd` itself is non-null, *out_fd is reset to -1 before deeper
 *   validation begins
 *
 * Verification:
 * - the returned fd must have FD_CLOEXEC set
 * - if `expect != NULL`, fs_file_verify() rules apply
 *
 * Acquisition detail:
 * - this helper acquires the fd with O_NONBLOCK first so that special files
 *   such as FIFOs do not block the caller before regular-file verification
 * - O_NOCTTY is also used during acquisition so terminal-like special files do
 *   not accidentally become controlling terminals before verification rejects
 *   them
 * - before success returns, the helper restores blocking file-status flags on
 *   the returned fd
 */
int fs_file_open_read_at(const fs_dir_t* parent, const char* name, const fs_expect_t* expect, int* out_fd);

/**
 * Open an EXISTING regular file for READ-WRITE under a directory capability.
 *
 * Identical safety and semantics to fs_file_open_read_at() (O_NOFOLLOW, the
 * NONBLOCK-then-restore acquisition, fs_file_verify()), but the returned fd is
 * O_RDWR so the caller may lseek() and write() — the resumable upload spool
 * appends at the persisted offset across separate requests. NEVER creates: the
 * named file must already exist (returns -ENOENT via -errno otherwise).
 */
int fs_file_open_rw_at(const fs_dir_t* parent, const char* name, const fs_expect_t* expect, int* out_fd);

/**
 * @brief Create a brand new regular file for writing under a parent dir.
 *
 * @param parent Parent directory capability.
 * @param name Single path component, not a multi-component path.
 * @param create_mode Final mode requested for the newly-created file.
 * @param expect Expected metadata for the resulting file.
 * If null, only regular-file type is checked after open.
 * @param out_fd Receives the opened fd.
 * @return 0 on success, negative errno on failure.
 *
 * Behavior:
 * - opens with O_CREAT | O_EXCL, so an existing destination returns -EEXIST
 * - symlinks are rejected on the final component
 * - may call fchmod() after creation so the final mode is explicit and not
 *   silently left to the process umask
 *
 * Contract:
 * - `parent` must be a valid directory capability
 *
 * Mode rules:
 * - `create_mode` must use only bits in FS_MODE_MASK
 *
 * Ownership:
 * - on success, ownership of the opened fd is transferred to the caller
 * - caller must close *out_fd
 * - if `out_fd` itself is non-null, *out_fd is reset to -1 before deeper
 *   validation begins
 *
 * Durability:
 * - this function does not fsync the parent directory automatically
 * - if the caller writes file content, changes file metadata via this helper's
 *   internal fchmod(), and then needs crash-consistent persistence, the usual
 *   pattern is:
 *   fs_file_fsync(fd) followed by fs_dir_fsync(parent)
 *
 * Failure cleanup:
 * - if post-create steps fail after a brand-new file was created, the helper
 *   may attempt best-effort cleanup with unlinkat(..., 0)
 * - that cleanup is name-based under the trusted/private parent capability
 */
int fs_file_create_write_new_at(const fs_dir_t* parent, const char* name, mode_t create_mode, const fs_expect_t* expect, int* out_fd);

/**
 * @brief Rename a single path component from one directory capability to another.
 *
 * Path rules:
 * - `old_name` and `new_name` must each be a single path component
 * - `old_parent` and `new_parent` must be valid directory capabilities
 *
 * Semantics:
 * - this is a thin wrapper around renameat()
 * - if the destination already exists, it may be atomically replaced
 * - no symlink traversal checks are performed here beyond the fact that the
 *   parent directories are already capabilities
 *
 * Durability:
 * - for rename within one directory, fsync that directory when crash-consistent
 *   namespace persistence matters
 * - for rename across two different directories, fsync both the old parent and
 *   the new parent when crash-consistent namespace persistence matters
 *
 * @return 0 on success, negative errno on failure.
 */
int fs_rename_at(const fs_dir_t* old_parent, const char* old_name, const fs_dir_t* new_parent, const char* new_name);

/**
 * @brief Unlink a single non-directory entry under a directory capability.
 *
 * Path rules:
 * - `name` must be a single path component
 * - `parent` must be a valid directory capability
 *
 * Semantics:
 * - this is a thin wrapper around unlinkat(..., 0)
 * - it may unlink regular files, symlinks themselves, FIFOs, sockets, and
 *   other non-directory entries permitted by the kernel and filesystem
 * - it does not remove directories
 *
 * Durability:
 * - caller should fsync the parent directory when durable deletion semantics
 *   matter
 *
 * @return 0 on success, negative errno on failure.
 */
int fs_unlink_at(const fs_dir_t* parent, const char* name);

/**
 * @brief Fsync a directory capability.
 *
 * Some filesystems report EINVAL for directory fsync. That case is treated as
 * success, matching fs_fsync_dir().
 *
 * This function is intentionally small and explicit:
 * caller decides when a directory durability barrier belongs in the higher
 * level protocol.
 *
 * Verification:
 * - the supplied handle must verify as a directory capability before fsync()
 *   is attempted
 */
int fs_dir_fsync(const fs_dir_t* dir);

/**
 * @brief Fsync a regular file descriptor.
 *
 * This function is intentionally small and explicit:
 * caller decides when a file durability barrier belongs in the higher level
 * protocol.
 *
 * Verification:
 * - the supplied fd must verify as a regular file descriptor before fsync()
 *   is attempted
 */
int fs_file_fsync(int fd);



#endif /* FSUTIL_H */
