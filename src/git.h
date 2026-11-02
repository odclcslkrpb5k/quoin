/* Git status of files, as the `git` program reports it (`git status --porcelain=v2 -z`):
 * never a git library inside Quoin, so a crash or hang in git can't take a window with it,
 * and the user's own git settings apply. No GTK: quoin-serve uses this too.
 *
 * Safety: a repository's own .git/config can make plain `git status` run commands
 * (core.fsmonitor; a "clean" filter). Repositories you cloned are safe (clone copies no
 * config); one that arrives whole (a tarball, a USB stick, a share) may not be. So git is
 * run only in repositories owned by this user, on this machine's own disks (not network,
 * FUSE or removable-media filesystems, nor under /media or /run/media), always with
 * core.fsmonitor off. A hostile clean filter in a repository you own could still run: the
 * same exposure as a shell prompt that shows git status (user's decision, 2026-11-01).
 *
 * Everything here that touches the filesystem or runs git blocks: worker threads only. */
#pragma once

/* In order of urgency: a folder shows the most urgent state of anything inside it. */
typedef enum
{
  GBK_GIT_NONE,         /* unchanged, or not in a repository */
  GBK_GIT_IGNORED,      /* matched by .gitignore (never rolls up into folders) */
  GBK_GIT_UNTRACKED,
  GBK_GIT_ADDED,        /* new, staged */
  GBK_GIT_STAGED,       /* changes staged, none unstaged */
  GBK_GIT_MODIFIED,     /* changes not staged (maybe staged ones too) */
  GBK_GIT_CONFLICT,     /* unmerged */
  GBK_GIT_N_STATES
} GbkGitState;

/* "Untracked", "New, staged", ...; for a folder, what it contains. */
const char *gbk_git_state_label (GbkGitState state, bool folder);

typedef struct
{
  GbkGitState state;
  char path[];          /* relative to the top of the work tree; folders end in '/' */
} GbkGitPath;

/* One `git status` answer. */
typedef struct
{
  char *head;           /* branch, or nullptr when detached */
  char *oid;            /* commit, or nullptr before the first commit */
  char *upstream;       /* nullptr when there is none */
  bool has_ab;          /* ahead and behind are known */
  uint64_t ahead, behind;
  unsigned n_conflict, n_modified, n_staged, n_untracked;
  GPtrArray *paths;     /* GbkGitPath*; renames also list the old path (as staged) */
} GbkGitStatus;

/* Parse `git status --porcelain=v2 -z --branch` output. Strict: anything unexpected is
 * an error (fuzzed). */
[[nodiscard]] GbkGitStatus *gbk_git_parse (const char *buf, size_t len, GError **error);
void gbk_git_status_free (GbkGitStatus *s);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkGitStatus, gbk_git_status_free)

/* The states of the things in folder `rel` (relative to the top; "" for the top): raw name
 * -> GUINT_TO_POINTER (GbkGitState), for anything not unchanged. Folders take the most
 * urgent state inside them. *inherit: the state of anything not in the table (everything
 * in an untracked or ignored folder is too). */
[[nodiscard]] GHashTable *gbk_git_folder_states (const GbkGitStatus *s, const char *rel,
                                                 GbkGitState *inherit);
/* The state of `name` in such a table, or `otherwise` when it isn't there. */
GbkGitState gbk_git_state_in (GHashTable *states, const char *name, GbkGitState otherwise);
/* The state of `rel` itself ("" for the top: the most urgent state in the whole tree). */
GbkGitState gbk_git_state_of (const GbkGitStatus *s, const char *rel);

/* The branch and how far it is from its upstream: "main ↑1 ↓2", "detached at 1a2b3c4",
 * "main (no commits yet)". */
[[nodiscard]] char *gbk_git_branch (const GbkGitStatus *s);
/* For a status bar: "main ↑1 ↓2 · 1 conflict · 3 modified · 2 staged · 4 untracked",
 * "main · clean", "detached at 1a2b3c4 · …", "main (no commits yet) · …". */
[[nodiscard]] char *gbk_git_summary (const GbkGitStatus *s);

/* The work tree holding `dir` (absolute): its top and its git directory, both absolute.
 * False when `dir` is in none, is inside a git directory, or is in one git mustn't be run
 * in (see the top of this file). Blocking. */
[[nodiscard]] bool gbk_git_find (const char *dir, char **top, char **gitdir);
/* `path` inside work tree `top` (it must be `top` or under it): "" for the top itself. */
const char *gbk_git_relative (const char *path, const char *top);
/* The filesystem rules alone, by statfs f_type and path (tests). */
bool gbk_git_fs_refused (int64_t f_type, const char *path);

/* Run `git status` in the work tree `top`, for all of it or just `pathspec` (relative to
 * top; taken literally). Ends early when `cancel_fd` (an eventfd, or -1) becomes readable
 * (G_IO_ERROR_CANCELLED) or after `timeout_ms` (G_IO_ERROR_TIMED_OUT). Blocking. */
[[nodiscard]] GbkGitStatus *gbk_git_status (const char *top, const char *pathspec, int cancel_fd,
                                            int timeout_ms, GError **error);

typedef struct
{
  char *hash;           /* abbreviated */
  char *author;
  int64_t time;         /* Unix seconds */
  char *subject;
} GbkGitCommit;

void gbk_git_commit_clear (GbkGitCommit *c);

/* The last commit that changed `rel` (relative to top). False with no error when there is
 * none (untracked, or no commits yet). Blocking. */
[[nodiscard]] bool gbk_git_last_commit (const char *top, const char *rel, int timeout_ms,
                                        GbkGitCommit *out, GError **error);

/* What the Properties window shows about one file or folder. */
typedef struct
{
  bool known;           /* it's in a work tree git may be run in */
  char *error;          /* git failed or took too long: nothing more is known */
  GbkGitState state;    /* for a folder, the most urgent state inside it */
  char *branch;         /* gbk_git_branch */
  GbkGitCommit commit;  /* the last commit that changed it; hash nullptr when none */
} GbkGitFileInfo;

/* Fill `out` for `path` (absolute; `folder`: it is one). Blocking. */
void gbk_git_file_info (const char *path, bool folder, GbkGitFileInfo *out);
void gbk_git_file_info_clear (GbkGitFileInfo *info);

#define GBK_GIT_OUTPUT_MAX (UINT64_C (64) << 20) /* more than this: "too many changes" */
#define GBK_GIT_TIMEOUT_MS 15000                 /* a folder's status */
#define GBK_GIT_INFO_TIMEOUT_MS 5000             /* one file's, for Properties */
