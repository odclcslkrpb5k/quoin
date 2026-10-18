/* File operations: copy, move, trash, delete, rename. Blocking: run on a worker thread
 * (see job.h), never on the main thread.
 *
 * Safety rules:
 * - Never overwrite without asking. New names are created with O_EXCL / RENAME_NOREPLACE,
 *   so something appearing mid-operation becomes a conflict, not a silent overwrite.
 * - Replacing a file is atomic: data goes to a hidden temp file in the destination
 *   directory, is fsync'd, then renamed over the old one. A failed or cancelled copy never
 *   leaves a half-written file where a good one was.
 * - Symlinks are copied/moved/deleted as links, never followed. Deletion walks the tree
 *   with dirfds (openat/unlinkat, O_NOFOLLOW): a symlink can't redirect it elsewhere.
 * - Copies are like `cp -a`: permissions, ownership, timestamps (files, folders and
 *   links), extended attributes (so ACLs too), hard links within what's copied, and
 *   special files (FIFOs, sockets; device nodes need root). As with cp -a, ownership and
 *   attributes that can't be kept (not root; filesystem without xattrs) are silently
 *   dropped. Sparse files are copied in full.
 * - A folder can't be copied or moved into itself.
 * - Directory depth is capped (GBK_OPS_MAX_DEPTH): deep trees fail cleanly instead of
 *   overflowing the stack. */
#pragma once

#define GBK_OPS_MAX_DEPTH 512

typedef enum
{
  GBK_OP_COPY,
  GBK_OP_MOVE,
  GBK_OP_TRASH,
  GBK_OP_DELETE,       /* permanent */
} GbkOpKind;

typedef enum
{
  GBK_CHOICE_REPLACE,   /* files: overwrite; folders: merge */
  GBK_CHOICE_SKIP,
  GBK_CHOICE_KEEP_BOTH, /* the new one gets a free "name (1).ext" */
  GBK_CHOICE_CANCEL,
} GbkChoice;

typedef struct
{
  /* `dst` exists. Return the choice; set *apply_all to reuse it for later conflicts. */
  GbkChoice (*conflict) (const char *src, const char *dst, bool dst_is_dir, bool *apply_all,
                         void *data);
  /* Something failed for `path`: GBK_CHOICE_SKIP to continue, GBK_CHOICE_CANCEL to stop. */
  GbkChoice (*error) (const char *path, const GError *error, void *data);
  /* Trash isn't possible for `path` (e.g. no trash on that filesystem): delete instead? */
  bool (*delete_untrashable) (const char *path, void *data);
  void (*progress) (uint64_t bytes_done, uint64_t bytes_total, const char *current, void *data);
  bool (*cancelled) (void *data);
  void *data;
} GbkOpsHooks;

/* Run `kind` on `sources` (absolute paths). `dest_dir` is used by COPY and MOVE.
 * Returns false only on cancellation or a fatal precondition (error set); per-item failures
 * go through hooks->error. `created` (may be nullptr) receives the basenames created in
 * dest_dir (char*, owned) so the caller can select them. */
[[nodiscard]] bool gbk_ops_run (GbkOpKind kind, GPtrArray *sources, const char *dest_dir,
                                const GbkOpsHooks *hooks, GPtrArray *created, GError **error);

/* Rename `path` to `new_name` in the same directory. Fails (G_IO_ERROR_EXISTS) rather than
 * replace an existing file. */
[[nodiscard]] bool gbk_ops_rename (const char *path, const char *new_name, GError **error);

/* Is `name` acceptable as a single filename? (non-empty, no '/', not "." or "..") */
[[nodiscard]] bool gbk_ops_valid_name (const char *name, GError **error);

/* "name (1).ext" style alternative for `name` that doesn't exist in `dir`. */
[[nodiscard]] char *gbk_ops_free_name (const char *dir, const char *name);
/* Pure: the n-th alternative for `name`: "a.txt" -> "a (n).txt", "x.tar.gz" -> "x (n).tar.gz",
 * ".bashrc" -> ".bashrc (n)". */
[[nodiscard]] char *gbk_ops_numbered_name (const char *name, unsigned n);

/* Create an empty file or folder `name` in `dir`. Never replaces anything: an existing
 * name fails with G_IO_ERROR_EXISTS. Permissions follow the umask (0666 / 0777). */
[[nodiscard]] bool gbk_ops_create (const char *dir, const char *name, bool folder, GError **error);

/* Turn execute permission on (for owner/group/others wherever they have read) or off
 * (for everyone). `path` must be, or link to, a regular file. */
[[nodiscard]] bool gbk_ops_set_executable (const char *path, bool executable, GError **error);

/* Set the rwx bits (0777) of `path` (following a symlink); setuid/setgid/sticky are kept. */
[[nodiscard]] bool gbk_ops_set_permissions (const char *path, uint32_t rwx, GError **error);

/* What the Properties window shows. For a symlink, everything but `link_target` describes
 * what it points to (a link's own permissions mean nothing), unless it dangles. */
typedef struct
{
  bool symlink;
  char *link_target;       /* symlinks only; raw bytes */
  bool dangling;           /* symlink whose target can't be read */
  uint32_t mode;           /* st_mode: type and permission bits */
  uint64_t size;
  uint32_t uid, gid;
  char *owner, *group;     /* names, or nullptr when unknown */
  int64_t atime_ns, mtime_ns, ctime_ns, btime_ns;
  bool has_btime;          /* the filesystem records creation time */
} GbkFileInfo;

[[nodiscard]] GbkFileInfo *gbk_ops_info (const char *path, GError **error);
void gbk_file_info_free (GbkFileInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkFileInfo, gbk_file_info_free)

/* Test hooks. Moves act as if source and destination were on different filesystems (the
 * copy-then-delete path); a cross-filesystem move makes its copy durable (syncfs) before
 * deleting the source, and these count those flushes and can make them fail with `err`. */
void gbk_ops_force_exdev_for_test (bool force);
void gbk_ops_fail_flush_for_test (int err);
int gbk_ops_flushes (void);
