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

/* One file's details as a copy needs them (lstat: a link isn't followed). */
typedef struct
{
  char *name;             /* LISTING: a valid filename; STATED: nullptr */
  uint32_t mode;          /* type and permission bits */
  uint32_t uid, gid;
  uint64_t nlink, size, dev, ino, rdev;
  int64_t atime_ns, mtime_ns, ctime_ns;
  char *target;           /* links: what the link says (bytes); otherwise nullptr */
} GbkStatEntry;

void gbk_stat_entry_free (GbkStatEntry *e);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkStatEntry, gbk_stat_entry_free)
/* From a struct stat (and the link's target, for a link). Owned copies. */
[[nodiscard]] GbkStatEntry *gbk_stat_entry_new (const char *name, const struct stat *st,
                                                const char *target);

/* Where a copy reads from: this machine (gbk_source_local) or another one (conn.h). Paths
 * are that machine's. Every call runs on the job's worker thread and may block; a remote
 * source checks `cancelled` while it waits. */
typedef struct GbkSource GbkSource;

/* An open source file. Each source's own file type starts with this. */
typedef struct
{
  GbkSource *source;
} GbkSourceFile;

typedef struct
{
  /* A file's details, a link not followed. */
  GbkStatEntry *(*lstat) (GbkSource *s, const char *path, GError **error);
  /* A folder's entries with their details (GbkStatEntry*, names valid, each once). */
  GPtrArray *(*list) (GbkSource *s, const char *path, GError **error);
  /* A regular file, to read from the start. */
  GbkSourceFile *(*open) (GbkSource *s, const char *path, GError **error);
  /* Up to `len` bytes: 0 at the end, -1 with *error (a remote file that changed while it
   * was read fails here, at the end). */
  ssize_t (*read) (GbkSourceFile *f, void *buf, size_t len, GError **error);
  /* This machine's file descriptor (for copy_file_range and attributes), or -1. */
  int (*fd) (const GbkSourceFile *f);
  void (*close) (GbkSourceFile *f);
  /* After a move's copy is safely stored: delete `path` (a source given to the move), but
   * only what is still as it was when it was listed; anything changed since is kept, and
   * that fails. nullptr where moving from it isn't possible. */
  bool (*remove) (GbkSource *s, const char *path, GError **error);
  void (*free) (GbkSource *s);
} GbkSourceOps;

struct GbkSource
{
  const GbkSourceOps *ops;
  /* This machine: owner, group, ACLs and other attributes are kept (cp -a), and "the same
   * file" checks apply. From another machine none of that travels: numeric owners and
   * ACL entries mean other people there; setuid, setgid and sticky bits are dropped too. */
  bool local;
  bool (*cancelled) (void *data);    /* set by gbk_ops_run_from */
  void *cancel_data;
};

/* `name` relative to `dirfd` (AT_FDCWD: a path), by lstat, with the link's target for a
 * link; named `entry_name` (nullptr: unnamed). nullptr with *error. An empty `name` means
 * `dirfd` itself (an O_PATH descriptor, which may be a link). */
[[nodiscard]] GbkStatEntry *gbk_stat_entry_at (int dirfd, const char *name, const char *entry_name,
                                               GError **error);

/* Is this file as it was when it was listed? `path` is the name it was listed under. */
typedef bool (*GbkUnchangedFn) (const char *path, const struct stat *st, void *data);

/* Delete `name` in folder `dirfd`, listed as `path`, and everything under it, but only
 * what `unchanged` vouches for: anything else is kept (and so are the folders it is in),
 * and that fails with G_IO_ERROR_FAILED saying how many were kept. Never follows a
 * symlink. For a move from another machine (GbkSourceOps.remove), on the machine that
 * holds the source. */
[[nodiscard]] bool gbk_ops_remove_unchanged (int dirfd, const char *name, const char *path,
                                             GbkUnchangedFn unchanged, void *data, GError **error);

[[nodiscard]] GbkSource *gbk_source_local (void);
void gbk_source_free (GbkSource *s);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkSource, gbk_source_free)

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
/* The same, reading the sources from `src` (another machine's paths). From another machine
 * only COPY and MOVE are possible (others fail with G_IO_ERROR_NOT_SUPPORTED). A MOVE copies
 * each source, makes the copy durable, then has the source remove it (GbkSourceOps.remove),
 * only if nothing in it was skipped or failed. */
[[nodiscard]] bool gbk_ops_run_from (GbkSource *src, GbkOpKind kind, GPtrArray *sources,
                                     const char *dest_dir, const GbkOpsHooks *hooks,
                                     GPtrArray *created, GError **error);

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
