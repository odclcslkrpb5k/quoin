#include "capture.h"
#include <poll.h>
#include <spawn.h>

#define WAIT_POLL_MS 50

extern char **environ;

void
gbk_captured_clear (GbkCaptured *c)
{
  g_clear_pointer (&c->out, g_byte_array_unref);
  g_clear_pointer (&c->err_tail, g_byte_array_unref);
  c->status = 0;
}

[[nodiscard]] static bool
set_errno_error (GError **error, int err)
{
  g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (err), "%s", g_strerror (err));
  return false;
}

[[nodiscard]] static bool
spawn (const char *const *argv, const char *const *envp, const char *cwd, int out_fd, int err_fd,
       pid_t *pid, GError **error)
{
  if (argv[0] == nullptr)
    gbk_fatal ("a command without a program");
  /* posix_spawn wants char *const[]: copies, so nothing const is cast away. */
  g_autoptr (GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
  for (size_t i = 0; argv[i] != nullptr; i++)
    g_ptr_array_add (args, g_strdup (argv[i]));
  g_ptr_array_add (args, nullptr);
  g_autoptr (GPtrArray) env = g_ptr_array_new_with_free_func (g_free);
  for (size_t i = 0; envp != nullptr && envp[i] != nullptr; i++)
    g_ptr_array_add (env, g_strdup (envp[i]));
  g_ptr_array_add (env, nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init (&fa);
  posix_spawnattr_init (&attr);
  posix_spawn_file_actions_addopen (&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2 (&fa, out_fd, 1);
  posix_spawn_file_actions_adddup2 (&fa, err_fd, 2);
  if (cwd != nullptr)
    posix_spawn_file_actions_addchdir_np (&fa, cwd);
  /* A session of its own, so one signal ends whatever it started too. */
  sigset_t none, defaults;
  sigemptyset (&none);
  sigemptyset (&defaults);
  sigaddset (&defaults, SIGPIPE);
  posix_spawnattr_setsigmask (&attr, &none);
  posix_spawnattr_setsigdefault (&attr, &defaults);
  posix_spawnattr_setflags (&attr,
                            POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  int err = posix_spawnp (pid, argv[0], &fa, &attr, (char *const *) args->pdata,
                          envp != nullptr ? (char *const *) env->pdata : environ);
  posix_spawnattr_destroy (&attr);
  posix_spawn_file_actions_destroy (&fa);
  if (err != 0)
    g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (err), "Couldn't run %s: %s",
                 argv[0], g_strerror (err));
  return err == 0;
}

/* Read what's there; false at end of file (or an error). */
static bool
drain (int fd, GByteArray *into, size_t keep_last)
{
  uint8_t buf[65536];
  for (;;)
    {
      ssize_t n = read (fd, buf, sizeof buf);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && errno == EAGAIN)
        return true;
      if (n <= 0)
        return false;
      g_byte_array_append (into, buf, (guint) n);
      if (keep_last > 0 && into->len > keep_last)
        g_byte_array_remove_range (into, 0, into->len - (guint) keep_last);
    }
}

typedef enum
{
  RUN_DONE,
  RUN_CANCELLED,
  RUN_TIMED_OUT,
  RUN_TOO_BIG,
} RunEnd;

bool
gbk_capture (const char *const *argv, const char *const *envp, const char *cwd, int cancel_fd,
             int timeout_ms, size_t out_max, GbkCaptured *out, GError **error)
{
  memset (out, 0, sizeof *out);
  int o[2], e[2];
  if (pipe2 (o, O_CLOEXEC) != 0)
    return set_errno_error (error, errno);
  if (pipe2 (e, O_CLOEXEC) != 0)
    {
      int err = errno;
      close (o[0]);
      close (o[1]);
      return set_errno_error (error, err);
    }
  pid_t pid;
  bool started = spawn (argv, envp, cwd, o[1], e[1], &pid, error);
  close (o[1]);
  close (e[1]);
  if (!started)
    {
      close (o[0]);
      close (e[0]);
      return false;
    }
  fcntl (o[0], F_SETFL, O_NONBLOCK);
  fcntl (e[0], F_SETFL, O_NONBLOCK);
  g_autoptr (GByteArray) data = g_byte_array_new ();
  g_autoptr (GByteArray) said = g_byte_array_new ();
  int64_t deadline = g_get_monotonic_time () + (int64_t) timeout_ms * 1000;
  bool out_open = true, err_open = true;
  RunEnd end = RUN_DONE;
  int status = 0;
  bool reaped = false;
  while (end == RUN_DONE && !reaped)
    {
      int64_t left_ms = (deadline - g_get_monotonic_time ()) / 1000;
      if (left_ms <= 0)
        {
          end = RUN_TIMED_OUT;
          break;
        }
      struct pollfd p[3] = {
        { .fd = out_open ? o[0] : -1, .events = POLLIN },
        { .fd = err_open ? e[0] : -1, .events = POLLIN },
        { .fd = cancel_fd, .events = POLLIN },
      };
      /* With both pipes closed, only the exit is left to wait for: look now and then. */
      int wait_ms = out_open || err_open ? (int) MIN (left_ms, 1000) : WAIT_POLL_MS;
      int n = poll (p, 3, wait_ms);
      if (n < 0 && errno != EINTR)
        {
          end = RUN_CANCELLED; /* can't wait any more: stop it */
          break;
        }
      if (cancel_fd >= 0 && (p[2].revents & POLLIN))
        end = RUN_CANCELLED;
      if (out_open && (p[0].revents & (POLLIN | POLLHUP | POLLERR)))
        out_open = drain (o[0], data, 0);
      if (err_open && (p[1].revents & (POLLIN | POLLHUP | POLLERR)))
        err_open = drain (e[0], said, GBK_CAPTURE_ERR_TAIL);
      if (data->len > out_max)
        end = RUN_TOO_BIG;
      if (!out_open && !err_open && end == RUN_DONE)
        {
          pid_t w = waitpid (pid, &status, WNOHANG);
          if (w == pid)
            reaped = true;
          else if (w < 0 && errno != EINTR)
            {
              int err = errno;
              close (o[0]);
              close (e[0]);
              return set_errno_error (error, err);
            }
        }
    }
  close (o[0]);
  close (e[0]);
  if (!reaped)
    {
      kill (-pid, SIGKILL);
      while (waitpid (pid, &status, 0) < 0 && errno == EINTR)
        ;
    }
  g_autofree char *prog = g_path_get_basename (argv[0]);
  switch (end)
    {
    case RUN_CANCELLED:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return false;
    case RUN_TIMED_OUT:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "%s took more than %d s", prog,
                   timeout_ms / 1000);
      return false;
    case RUN_TOO_BIG:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "%s printed too much", prog);
      return false;
    case RUN_DONE: break;
    default: gbk_fatal ("bad run end %d", (int) end);
    }
  out->out = g_steal_pointer (&data);
  out->err_tail = g_steal_pointer (&said);
  out->status = status;
  return true;
}

char *
gbk_captured_last_line (const GbkCaptured *c)
{
  if (c->err_tail == nullptr)
    return g_strdup ("");
  g_autofree char *text
      = g_utf8_make_valid ((const char *) c->err_tail->data, (gssize) c->err_tail->len);
  g_strstrip (text);
  const char *last = strrchr (text, '\n');
  return g_strdup (last != nullptr ? last + 1 : text);
}
