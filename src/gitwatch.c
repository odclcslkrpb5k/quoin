#include "gitwatch.h"
#include "inotify-hub.h"
#include <poll.h>
#include <sys/eventfd.h>

#define GIT_DIR_EVENTS (IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE)

struct GbkGitWatch
{
  gatomicrefcount rc;     /* one for the main thread, one for the worker */
  char *dir;
  int wake_fd;            /* eventfd: worker -> main */
  int cancel_fd;          /* eventfd: main -> worker */
  int poke_fd;            /* eventfd: main -> worker */
  GMutex lock;
  GbkGitResult *result;   /* newest, not yet taken */
};

static int live_workers;
static int runs;

int
gbk_git_watch_live_workers (void)
{
  return g_atomic_int_get (&live_workers);
}

int
gbk_git_watch_runs (void)
{
  return g_atomic_int_get (&runs);
}

GbkGitResult *
gbk_git_result_new (bool repo)
{
  GbkGitResult *r = g_new0 (GbkGitResult, 1);
  r->repo = repo;
  r->names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nullptr);
  return r;
}

void
gbk_git_result_free (GbkGitResult *r)
{
  if (r == nullptr)
    return;
  g_free (r->error);
  g_free (r->summary);
  g_free (r->upstream);
  g_hash_table_unref (r->names);
  g_free (r);
}

static GbkGitResult *
result_new (bool repo)
{
  return gbk_git_result_new (repo);
}

static void
watch_unref (GbkGitWatch *w)
{
  if (!g_atomic_ref_count_dec (&w->rc))
    return;
  close (w->wake_fd);
  close (w->cancel_fd);
  close (w->poke_fd);
  g_mutex_clear (&w->lock);
  gbk_git_result_free (w->result);
  g_free (w->dir);
  g_free (w);
}

static void
signal_fd (int fd)
{
  uint64_t one = 1;
  /* Fails only with EINTR, or when the counter would overflow (it is reset long before). */
  while (write (fd, &one, sizeof one) < 0 && errno == EINTR)
    ;
}

static void
reset_fd (int fd)
{
  uint64_t n;
  while (read (fd, &n, sizeof n) < 0 && errno == EINTR)
    ;
}

static bool
readable (int fd)
{
  struct pollfd p = { .fd = fd, .events = POLLIN };
  return poll (&p, 1, 0) > 0 && (p.revents & POLLIN);
}

static void
post (GbkGitWatch *w, GbkGitResult *r)
{
  g_mutex_lock (&w->lock);
  gbk_git_result_free (w->result);
  w->result = r;
  g_mutex_unlock (&w->lock);
  signal_fd (w->wake_fd);
}

static GbkGitResult *
look (const GbkGitWatch *w, const char *dir, const char *top)
{
  g_atomic_int_inc (&runs);
  GbkGitResult *r = result_new (true);
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkGitStatus) s
      = gbk_git_status (top, nullptr, w->cancel_fd, GBK_GIT_TIMEOUT_MS, &error);
  if (s == nullptr && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      /* No git on this machine: nothing to show, rather than the same complaint everywhere. */
      gbk_git_result_free (r);
      return result_new (false);
    }
  if (s == nullptr)
    {
      r->error = g_strdup (error->message);
      return r;
    }
  g_hash_table_unref (r->names);
  r->names = gbk_git_folder_states (s, gbk_git_relative (dir, top), &r->inherit);
  r->summary = gbk_git_summary (s);
  r->upstream = s->upstream != nullptr ? g_utf8_make_valid (s->upstream, -1) : nullptr;
  return r;
}

/* git's own files changed (not its lock files, which come and go around every write). */
static void
on_git_event (int wd G_GNUC_UNUSED, uint32_t mask, const char *name, size_t name_len G_GNUC_UNUSED,
              void *data)
{
  bool *changed = data;
  if ((mask & IN_Q_OVERFLOW) || name == nullptr || !g_str_has_suffix (name, ".lock"))
    *changed = true;
}

static void
watch_git_dir (GbkInotifySub *sub, const char *gitdir)
{
  g_autofree char *logs = g_build_filename (gitdir, "logs", nullptr);
  /* Without these there are no live updates for commits and staging, but the status
   * still follows the folder's own changes and comes back to the window. */
  if (gbk_inotify_sub_watch (sub, gitdir, GIT_DIR_EVENTS) < 0)
    g_debug ("can't watch a git directory: %s", g_strerror (errno));
  if (gbk_inotify_sub_watch (sub, logs, GIT_DIR_EVENTS | IN_MODIFY) < 0 && errno != ENOENT)
    g_debug ("can't watch a git directory's logs: %s", g_strerror (errno));
}

static void *
worker (void *data)
{
  GbkGitWatch *w = data;
  g_autofree char *dir = g_canonicalize_filename (w->dir, nullptr);
  g_autofree char *top = nullptr;
  g_autofree char *gitdir = nullptr;
  GbkInotifySub *sub = nullptr;
  bool posted_none = false;
  int64_t now = g_get_monotonic_time ();
  bool dirty = true; /* look at once */
  int64_t first_change = now - GBK_GIT_MAX_DELAY_MS * INT64_C (1000);
  int64_t last_change = first_change;
  int64_t not_before = 0;
  for (;;)
    {
      now = g_get_monotonic_time ();
      int timeout_ms = -1;
      if (dirty)
        {
          int64_t due = MIN (last_change + GBK_GIT_QUIET_MS * INT64_C (1000),
                             first_change + GBK_GIT_MAX_DELAY_MS * INT64_C (1000));
          due = MAX (due, not_before);
          if (now >= due)
            {
              dirty = false;
              if (top == nullptr && gbk_git_find (dir, &top, &gitdir))
                {
                  sub = gbk_inotify_sub_new ();
                  watch_git_dir (sub, gitdir);
                }
              if (top == nullptr)
                {
                  if (!posted_none)
                    post (w, result_new (false));
                  posted_none = true;
                  continue;
                }
              GbkGitResult *r = look (w, dir, top);
              if (readable (w->cancel_fd))
                {
                  gbk_git_result_free (r);
                  break;
                }
              int64_t took = g_get_monotonic_time () - now;
              not_before = g_get_monotonic_time () + took;
              post (w, r);
              continue;
            }
          timeout_ms = (int) MIN ((due - now) / 1000 + 1, 60000);
        }
      struct pollfd p[3] = {
        { .fd = w->cancel_fd, .events = POLLIN },
        { .fd = w->poke_fd, .events = POLLIN },
        { .fd = sub != nullptr ? gbk_inotify_sub_fd (sub) : -1, .events = POLLIN },
      };
      if (poll (p, 3, timeout_ms) < 0 && errno != EINTR)
        break;
      if (p[0].revents & POLLIN)
        break;
      bool changed = false;
      if (p[1].revents & POLLIN)
        {
          reset_fd (w->poke_fd);
          changed = true;
        }
      if (sub != nullptr && (p[2].revents & POLLIN))
        gbk_inotify_sub_take (sub, on_git_event, &changed);
      if (changed)
        {
          now = g_get_monotonic_time ();
          if (!dirty)
            first_change = now;
          last_change = now;
          dirty = true;
        }
    }
  if (sub != nullptr)
    gbk_inotify_sub_free (sub);
  g_atomic_int_dec_and_test (&live_workers);
  watch_unref (w);
  return nullptr;
}

GbkGitWatch *
gbk_git_watch_start (const char *dir)
{
  GbkGitWatch *w = g_new0 (GbkGitWatch, 1);
  g_atomic_ref_count_init (&w->rc);
  w->dir = g_strdup (dir);
  g_mutex_init (&w->lock);
  w->wake_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  w->cancel_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  w->poke_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (w->wake_fd < 0 || w->cancel_fd < 0 || w->poke_fd < 0)
    gbk_fatal ("eventfd: %s", g_strerror (errno)); /* fd exhaustion: nothing sane to do */
  g_atomic_ref_count_inc (&w->rc); /* the worker's */
  g_atomic_int_inc (&live_workers);
  g_autoptr (GError) error = nullptr;
  GThread *t = g_thread_try_new ("gbk-git", worker, w, &error);
  if (t == nullptr)
    {
      /* No thread: no git status for this folder, which is all that's lost. */
      g_warning ("no git status thread: %s", error->message);
      g_atomic_int_dec_and_test (&live_workers);
      watch_unref (w);
      return w;
    }
  g_thread_unref (t);
  return w;
}

int
gbk_git_watch_wake_fd (const GbkGitWatch *w)
{
  return w->wake_fd;
}

GbkGitResult *
gbk_git_watch_take (GbkGitWatch *w)
{
  reset_fd (w->wake_fd);
  g_mutex_lock (&w->lock);
  GbkGitResult *r = g_steal_pointer (&w->result);
  g_mutex_unlock (&w->lock);
  return r;
}

void
gbk_git_watch_poke (const GbkGitWatch *w)
{
  signal_fd (w->poke_fd);
}

void
gbk_git_watch_stop (GbkGitWatch *w)
{
  signal_fd (w->cancel_fd);
  watch_unref (w);
}
