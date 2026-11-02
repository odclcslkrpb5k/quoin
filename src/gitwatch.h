/* One folder's git status, kept current: a worker thread per folder, like scanner.h.
 *
 * The worker finds the work tree (git.h: only ones git may be run in), runs `git status`
 * and posts the states of the folder's contents. It runs it again, after a short quiet
 * spell, when git's own files change (the index, HEAD, refs: a commit, `git add`, a fetch,
 * a checkout, in any program) and when poked (the folder's contents changed, the window
 * came back into use). A slow repository is never run more than half the time.
 *
 * Only the newest result is kept; the wake fd is readable while one is waiting. Stopping
 * never blocks: a running git is killed and the worker cleans up on its own. */
#pragma once
#include "git.h"

typedef struct
{
  bool repo;            /* false: not in a work tree git may be run in; show nothing */
  char *error;          /* git failed or took too long, or nullptr */
  char *summary;        /* gbk_git_summary; nullptr unless git answered */
  char *upstream;       /* the branch's upstream, or nullptr */
  GHashTable *names;    /* gbk_git_folder_states (raw name -> state); never nullptr */
  GbkGitState inherit;  /* the state of anything not in `names` */
} GbkGitResult;

/* Empty: no names, nothing inherited, no text. */
[[nodiscard]] GbkGitResult *gbk_git_result_new (bool repo);
void gbk_git_result_free (GbkGitResult *r);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkGitResult, gbk_git_result_free)

typedef struct GbkGitWatch GbkGitWatch;

/* Start watching folder `dir` (absolute, on this machine). */
[[nodiscard]] GbkGitWatch *gbk_git_watch_start (const char *dir);
int gbk_git_watch_wake_fd (const GbkGitWatch *w);
/* The newest result since the last call, or nullptr; makes the wake fd unreadable. */
[[nodiscard]] GbkGitResult *gbk_git_watch_take (GbkGitWatch *w);
/* Something may have changed: look again soon. */
void gbk_git_watch_poke (const GbkGitWatch *w);
/* Stop and release the caller's reference. `w` must not be used afterwards. */
void gbk_git_watch_stop (GbkGitWatch *w);

/* Tests: workers not yet exited; `git status` runs started, ever. */
int gbk_git_watch_live_workers (void);
int gbk_git_watch_runs (void);

#define GBK_GIT_QUIET_MS 200      /* run once changes have stopped this long... */
#define GBK_GIT_MAX_DELAY_MS 1000 /* ...but never wait longer than this */
