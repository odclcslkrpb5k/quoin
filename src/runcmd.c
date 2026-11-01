#include "runcmd.h"
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

#define POLL_MS 100
#define KILL_AFTER_MS 2000

extern char **environ;

/* The last GBK_RUN_TAIL bytes read from `fd` (non-blocking), appended to `tail`. */
static void
drain (int fd, GString *tail)
{
  char buf[4096];
  for (;;)
    {
      ssize_t n = read (fd, buf, sizeof buf);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return;
      g_string_append_len (tail, buf, n);
      if (tail->len > GBK_RUN_TAIL)
        g_string_erase (tail, 0, (gssize) (tail->len - GBK_RUN_TAIL));
    }
}

/* Its last few lines of output, as valid UTF-8, or nullptr if it printed nothing. */
static char *
last_lines (const GString *tail)
{
  g_autofree char *valid = g_utf8_make_valid (tail->str, (gssize) tail->len);
  g_strstrip (valid);
  if (valid[0] == '\0')
    return nullptr;
  g_auto (GStrv) lines = g_strsplit (valid, "\n", -1);
  guint n = g_strv_length (lines);
  guint from = n > 3 ? n - 3 : 0;
  return g_strjoinv ("\n", lines + from);
}

[[nodiscard]] static bool
spawn (const GPtrArray *argv, const char *cwd, int err_fd, pid_t *pid, GError **error)
{
  g_autoptr (GPtrArray) args = g_ptr_array_new ();
  for (guint i = 0; i < argv->len; i++)
    g_ptr_array_add (args, g_ptr_array_index (argv, i));
  g_ptr_array_add (args, nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init (&fa);
  posix_spawnattr_init (&attr);
  posix_spawn_file_actions_addopen (&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen (&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_adddup2 (&fa, err_fd, 2);
  posix_spawn_file_actions_addchdir_np (&fa, cwd);
  /* A session of its own (one signal ends it all), default signals (quoin-serve ignores
   * SIGPIPE), nothing blocked. */
  sigset_t none, defaults;
  sigemptyset (&none);
  sigemptyset (&defaults);
  sigaddset (&defaults, SIGPIPE);
  posix_spawnattr_setsigmask (&attr, &none);
  posix_spawnattr_setsigdefault (&attr, &defaults);
  posix_spawnattr_setflags (&attr,
                            POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  int err = posix_spawnp (pid, g_ptr_array_index (argv, 0), &fa, &attr, (char *const *) args->pdata,
                          environ);
  posix_spawnattr_destroy (&attr);
  posix_spawn_file_actions_destroy (&fa);
  if (err != 0)
    g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (err), "Couldn't run %s: %s",
                 (const char *) g_ptr_array_index (argv, 0), g_strerror (err));
  return err == 0;
}

bool
gbk_run_command (const GPtrArray *argv, const char *cwd, const GbkOpsHooks *hooks, GError **error)
{
  if (argv->len == 0)
    gbk_fatal ("a command without a program");
  const char *prog = g_ptr_array_index (argv, 0);
  int fds[2];
  if (pipe2 (fds, O_CLOEXEC | O_NONBLOCK) != 0)
    {
      g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (errno), "%s",
                   g_strerror (errno));
      return false;
    }
  pid_t pid;
  bool started = spawn (argv, cwd, fds[1], &pid, error);
  close (fds[1]);
  if (!started)
    {
      close (fds[0]);
      return false;
    }
  hooks->progress (0, 0, prog, hooks->data);
  g_autoptr (GString) tail = g_string_new ("");
  int64_t killed_at = 0;   /* monotonic µs of the SIGTERM, 0: not yet */
  int status = 0;
  for (;;)
    {
      pid_t w = waitpid (pid, &status, WNOHANG);
      if (w == pid)
        break;
      if (w < 0 && errno != EINTR)
        {
          g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (errno), "%s",
                       g_strerror (errno));
          close (fds[0]);
          return false;
        }
      struct pollfd p = { .fd = fds[0], .events = POLLIN };
      if (poll (&p, 1, POLL_MS) > 0)
        drain (fds[0], tail);
      if (killed_at == 0 && hooks->cancelled (hooks->data))
        {
          kill (-pid, SIGTERM);
          killed_at = g_get_monotonic_time ();
        }
      else if (killed_at != 0
               && g_get_monotonic_time () - killed_at > (int64_t) KILL_AFTER_MS * 1000)
        kill (-pid, SIGKILL);
    }
  drain (fds[0], tail);
  close (fds[0]);
  if (killed_at != 0)
    {
      kill (-pid, SIGKILL);              /* anything it left behind in its session */
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return false;
    }
  if (WIFEXITED (status) && WEXITSTATUS (status) == 0)
    return true;
  g_autofree char *how = WIFEXITED (status)
                             ? g_strdup_printf ("exit status %d", WEXITSTATUS (status))
                             : g_strdup_printf ("%s", g_strsignal (WTERMSIG (status)));
  g_autofree char *said = last_lines (tail);
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s failed (%s)%s%s", prog, how,
               said != nullptr ? ":\n" : "", said != nullptr ? said : "");
  return false;
}
