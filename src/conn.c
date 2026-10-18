#include "conn.h"
#include "lifetime.h"

#define READ_CHUNK ((size_t) 64 * 1024)
#define STDERR_KEEP 4096u                /* the end of ssh's messages, for errors */
#define EVENTS_PER_WAKE 32
#define SERVE_MAX (32u << 20)            /* a quoin-serve larger than this isn't ours */
#define INSTALL_OUTPUT_MAX ((size_t) 64 * 1024)
#define CLOSE_GRACE_MS 2000              /* a clean close: time for ssh to finish */
#define INSTALLED_MARK "QUOIN-INSTALLED"

static int test_ping_ms, test_dead_ms, test_connect_ms;   /* atomic */
static int live_conns;                                    /* atomic */
/* Channel numbers, from any thread: never reused in this process. */
static uint64_t
new_channel (void)
{
  static GMutex lock;
  static uint64_t next = 1;
  g_mutex_lock (&lock);
  uint64_t ch = next++;
  g_mutex_unlock (&lock);
  return ch;
}

void
gbk_conn_set_timeouts_for_test (int ping_ms, int dead_ms, int connect_ms)
{
  g_atomic_int_set (&test_ping_ms, ping_ms);
  g_atomic_int_set (&test_dead_ms, dead_ms);
  g_atomic_int_set (&test_connect_ms, connect_ms);
}

int
gbk_conn_live (void)
{
  return g_atomic_int_get (&live_conns);
}

char *
gbk_conn_local_machine_id (void)
{
  g_autofree char *raw = nullptr;
  if (!g_file_get_contents ("/etc/machine-id", &raw, nullptr, nullptr))
    return g_strdup ("");
  g_strstrip (raw);
  for (const char *c = raw; *c != '\0'; c++)
    if (!g_ascii_isxdigit (*c))
      return g_strdup ("");
  return strlen (raw) <= 64 ? g_steal_pointer (&raw) : g_strdup ("");
}

/* ---- shared between the main thread and the I/O thread ---- */

typedef enum
{
  EV_READY,
  EV_SCAN,
  EV_INFO,
  EV_FAILED,
  EV_DEAD,
} EvType;

typedef struct
{
  EvType type;
  uint64_t channel;
  GbkHello hello;          /* READY */
  GbkScanMsg *scan;        /* SCAN */
  GbkFileInfo *info;       /* INFO */
  GError *error;           /* FAILED, DEAD */
  bool needs_login;        /* DEAD */
} Event;

static void
event_free (Event *e)
{
  gbk_hello_clear (&e->hello);
  g_clear_pointer (&e->scan, gbk_scan_msg_free);
  g_clear_pointer (&e->info, gbk_file_info_free);
  g_clear_error (&e->error);
  g_free (e);
}

typedef struct
{
  gatomicrefcount rc;
  char *host;
  GbkTypeTable *types;
  int ping_ms, dead_ms, connect_ms;
  GMutex lock;             /* guards out and closing */
  GByteArray *out;         /* frames for the server, sent once connected */
  bool closing;
  int wake_io;             /* eventfd: main -> I/O */
  GAsyncQueue *events;     /* Event*: I/O -> main */
  int wake_main;           /* eventfd: I/O -> main */
  /* Requests from worker threads (copies): their answers go straight to them. */
  GMutex calls_lock;       /* guards calls, dead, dead_error */
  GHashTable *calls;       /* &Call.channel -> Call* */
  bool dead;
  GError *dead_error;
  int workers;             /* sources in use (atomic): the connection is busy */
  int64_t workers_done_us; /* when the last one finished (under lock) */
} Shared;

static void
shared_unref (Shared *s)
{
  if (!g_atomic_ref_count_dec (&s->rc))
    return;
  g_free (s->host);
  gbk_type_table_unref (s->types);
  g_mutex_clear (&s->lock);
  g_byte_array_unref (s->out);
  close (s->wake_io);
  g_async_queue_unref (s->events);
  close (s->wake_main);
  g_mutex_clear (&s->calls_lock);
  g_hash_table_unref (s->calls);
  g_clear_error (&s->dead_error);
  g_free (s);
}

static void
poke (int fd)
{
  uint64_t one = 1;
  while (write (fd, &one, sizeof one) < 0 && errno == EINTR)
    ;
}

static void
drain_eventfd (int fd)
{
  uint64_t v;
  while (read (fd, &v, sizeof v) < 0 && errno == EINTR)
    ;
}

static void
post (Shared *s, Event *e)
{
  g_async_queue_push (s->events, e);
  poke (s->wake_main);
}

static bool
closing (Shared *s)
{
  g_mutex_lock (&s->lock);
  bool c = s->closing;
  g_mutex_unlock (&s->lock);
  return c;
}

/* ---- the I/O thread: ssh ---- */

typedef struct
{
  GSubprocess *proc;
  int sock;                /* its stdin and stdout */
  int err;                 /* read end of its stderr */
  GString *tail;           /* the last STDERR_KEEP bytes of its stderr */
} Ssh;

static int
remaining_ms (int64_t deadline)
{
  int64_t left = deadline - g_get_monotonic_time ();
  return left <= 0 ? 0 : (int) MIN ((left + 999) / 1000, (int64_t) INT_MAX);
}

static void
set_nonblocking (int fd)
{
  int fl = fcntl (fd, F_GETFL);
  if (fl >= 0)
    fcntl (fd, F_SETFL, fl | O_NONBLOCK);
}

/* The ssh command: QUOIN_SSH split into words (no shell runs it), or "ssh". */
static void
add_ssh (GStrvBuilder *b)
{
  g_auto (GStrv) custom = nullptr;
  const char *env = g_getenv ("QUOIN_SSH");
  if (env != nullptr && g_shell_parse_argv (env, nullptr, &custom, nullptr))
    for (size_t i = 0; custom[i] != nullptr; i++)
      g_strv_builder_add (b, custom[i]);
  else
    g_strv_builder_add (b, "ssh");
}

static char *
remote_serve (void)
{
  return g_strdup_printf (".cache/quoin/serve/%s/quoin-serve", GBK_VERSION);
}

char **
gbk_conn_shell_argv (const char *host, const char *path)
{
  if (!gbk_host_valid (host))
    return nullptr;
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  add_ssh (b);
  GString *hex = g_string_new ("");
  for (const char *c = path; *c != '\0'; c++)
    g_string_append_printf (hex, "%02x", (unsigned) (unsigned char) *c);
  g_autofree char *serve = remote_serve ();
  g_autofree char *hex_s = g_string_free (hex, false);
  const char *const tail[] = { "-t", "--", host, serve, "--shell-at", hex_s, nullptr };
  for (size_t i = 0; tail[i] != nullptr; i++)
    g_strv_builder_add (b, tail[i]);
  return g_strv_builder_end (b);
}

char **
gbk_conn_login_argv (const char *host)
{
  if (!gbk_host_valid (host))
    return nullptr;
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  add_ssh (b);
  const char *const tail[] = { "-o", "AddKeysToAgent=yes", "--", host, "true", nullptr };
  for (size_t i = 0; tail[i] != nullptr; i++)
    g_strv_builder_add (b, tail[i]);
  return g_strv_builder_end (b);
}

static char **
ssh_argv (const char *host, const char *remote_cmd)
{
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  add_ssh (b);
  /* Ours whatever ~/.ssh/config says (docs/remote-plan.md, section 5): no terminal, no
   * escape character, no prompts, no sharing a master connection that may be dead, no
   * forwarding (nothing exposed on a long-lived background session), no RemoteCommand
   * replacing ours, no new host keys accepted without a person. */
  static const char *const forced[] = {
    "-T",
    "-e",
    "none",
    "-o",
    "BatchMode=yes",
    "-o",
    "ControlMaster=no",
    "-o",
    "ControlPath=none",
    "-o",
    "ClearAllForwardings=yes",
    "-o",
    "ForwardAgent=no",
    "-o",
    "ForwardX11=no",
    "-o",
    "RemoteCommand=none",
    "-o",
    "RequestTTY=no",
    "-o",
    "ConnectTimeout=10",
    "-o",
    "StrictHostKeyChecking=yes",
    "-o",
    "ServerAliveInterval=10",
    "-o",
    "ServerAliveCountMax=3",
    "--",
  };
  for (size_t i = 0; i < G_N_ELEMENTS (forced); i++)
    g_strv_builder_add (b, forced[i]);
  g_strv_builder_add (b, host);
  g_strv_builder_add (b, remote_cmd);
  return g_strv_builder_end (b);
}

[[nodiscard]] static bool
ssh_start (const Shared *s, const char *remote_cmd, Ssh *ssh, GError **error)
{
  *ssh = (Ssh) { .sock = -1, .err = -1 };
  int pair[2], errp[2];
  if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
    {
      g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (errno), "socketpair: %s",
                   g_strerror (errno));
      return false;
    }
  if (!g_unix_open_pipe (errp, O_CLOEXEC, error))
    {
      close (pair[0]);
      close (pair[1]);
      return false;
    }
  int child_in = dup (pair[1]);
  if (child_in < 0)
    {
      g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (errno), "dup: %s",
                   g_strerror (errno));
      close (pair[0]);
      close (pair[1]);
      close (errp[0]);
      close (errp[1]);
      return false;
    }
  g_autoptr (GSubprocessLauncher) l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_take_stdin_fd (l, child_in);
  g_subprocess_launcher_take_stdout_fd (l, pair[1]);
  g_subprocess_launcher_take_stderr_fd (l, errp[1]);
  g_auto (GStrv) argv = ssh_argv (s->host, remote_cmd);
  ssh->proc = g_subprocess_launcher_spawnv (l, (const char *const *) argv, error);
  if (ssh->proc == nullptr)
    {
      close (pair[0]);
      close (errp[0]);
      return false;
    }
  ssh->sock = pair[0];
  ssh->err = errp[0];
  ssh->tail = g_string_new ("");
  set_nonblocking (ssh->sock);
  set_nonblocking (ssh->err);
  return true;
}

static void
ssh_drain_stderr (Ssh *ssh)
{
  char buf[4096];
  ssize_t n;
  while (ssh->err >= 0 && ssh->tail != nullptr && (n = read (ssh->err, buf, sizeof buf)) > 0)
    {
      g_string_append_len (ssh->tail, buf, n);
      if (ssh->tail->len > STDERR_KEEP)
        g_string_erase (ssh->tail, 0, (gssize) (ssh->tail->len - STDERR_KEEP));
    }
}

/* Stop it: kill if it hasn't exited (a dead network can keep ssh waiting), reap it, close
 * everything. Returns its exit status, or -1 if it was killed or didn't exit normally. */
static int
ssh_finish (Ssh *ssh, bool kill)
{
  if (ssh->tail == nullptr)
    return -1;                           /* never started */
  int status = -1;
  if (ssh->proc != nullptr)
    {
      if (kill)
        g_subprocess_force_exit (ssh->proc);
      if (g_subprocess_wait (ssh->proc, nullptr, nullptr) && g_subprocess_get_if_exited (ssh->proc))
        status = g_subprocess_get_exit_status (ssh->proc);
      g_clear_object (&ssh->proc);
    }
  ssh_drain_stderr (ssh);
  if (ssh->sock >= 0)
    close (ssh->sock);
  if (ssh->err >= 0)
    close (ssh->err);
  ssh->sock = ssh->err = -1;
  return status;
}

static void
ssh_free (Ssh *ssh)
{
  ssh_finish (ssh, true);
  if (ssh->tail != nullptr)
    g_string_free (ssh->tail, true);
  ssh->tail = nullptr;
}

/* The last line ssh (or the remote shell) wrote to stderr, for the user. g_free. */
static char *
last_line (const GString *tail)
{
  if (tail == nullptr)
    return nullptr;
  g_autofree char *text = g_utf8_make_valid (tail->str, (gssize) tail->len);
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  for (size_t i = g_strv_length (lines); i > 0; i--)
    {
      g_strstrip (lines[i - 1]);
      if (lines[i - 1][0] != '\0')
        return g_utf8_substring (lines[i - 1], 0, 300);
    }
  return nullptr;
}

/* ssh's own words mean a person has to step in once: a key to unlock or a host key to
 * accept. Matched on ssh's messages (no codes exist): only to offer "log in in a terminal",
 * never to decide anything that matters. */
static bool
login_needed (const GString *tail)
{
  if (tail == nullptr)
    return false;
  static const char *const signs[] = {
    "Permission denied",
    "Host key verification failed",
    "HOST IDENTIFICATION HAS CHANGED",
    "host key",
    "passphrase",
  };
  for (size_t i = 0; i < G_N_ELEMENTS (signs); i++)
    if (strstr (tail->str, signs[i]) != nullptr)
      return true;
  return false;
}

/* ---- the I/O thread: connecting ---- */

typedef enum
{
  HELLO_OK,
  HELLO_NOT_INSTALLED,     /* the remote command isn't there (or isn't our build) */
  HELLO_FAILED,            /* *error says why */
} HelloResult;

static char *
remote_dir (void)
{
  g_autofree char *serve = remote_serve ();
  return g_path_get_dirname (serve);
}

/* Read the magic line and HELLO from a just-started ssh. Bytes after them stay in `in`. */
[[nodiscard]] static HelloResult
await_hello (Shared *s, Ssh *ssh, int64_t deadline, GByteArray *in, GbkHello *hello, GError **error)
{
  bool magic = false;
  for (;;)
    {
      if (!magic)
        {
          uint32_t version;
          g_autofree char *build = nullptr;
          size_t used;
          int r = gbk_proto_magic (in->data, in->len, &version, &build, &used);
          if (r < 0)
            {
              g_autofree char *junk
                  = g_utf8_make_valid ((const char *) in->data, (gssize) MIN (in->len, 120u));
              g_autofree char *first = g_strdelimit (g_strdup (junk), "\r\n", ' ');
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "%s printed “%s” before Quoin could start there. A shell startup "
                           "file prints that even when not logging in interactively; it must "
                           "stay quiet then.",
                           s->host, g_strstrip (first));
              return HELLO_FAILED;
            }
          if (r == 1)
            {
              if (version != GBK_PROTO_VERSION || !g_str_equal (build, GBK_VERSION))
                return HELLO_NOT_INSTALLED;       /* another build answered: replace it */
              g_byte_array_remove_range (in, 0, (guint) used);
              magic = true;
            }
        }
      if (magic)
        {
          GbkFrame f;
          size_t used;
          int r = gbk_proto_next_frame (in->data, in->len, &f, &used);
          if (r < 0 || (r == 1 && !gbk_proto_get_hello (&f, hello)))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "%s answered with something Quoin doesn't understand", s->host);
              return HELLO_FAILED;
            }
          if (r == 1)
            {
              g_byte_array_remove_range (in, 0, (guint) used);
              return HELLO_OK;
            }
        }

      if (closing (s))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Closed");
          return HELLO_FAILED;
        }
      int wait = remaining_ms (deadline);
      if (wait == 0)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "%s didn't answer in time",
                       s->host);
          return HELLO_FAILED;
        }
      struct pollfd p[3]
          = { { ssh->sock, POLLIN, 0 }, { ssh->err, POLLIN, 0 }, { s->wake_io, POLLIN, 0 } };
      if (poll (p, 3, wait) < 0 && errno != EINTR)
        {
          g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (errno), "poll: %s",
                       g_strerror (errno));
          return HELLO_FAILED;
        }
      if (p[2].revents & POLLIN)
        drain_eventfd (s->wake_io);
      if (p[1].revents)
        ssh_drain_stderr (ssh);
      if (p[0].revents)
        {
          uint8_t buf[READ_CHUNK];
          ssize_t n = read (ssh->sock, buf, sizeof buf);
          if (n > 0)
            g_byte_array_append (in, buf, (guint) n);
          else if (n == 0 || (errno != EAGAIN && errno != EINTR))
            {
              /* It ended before saying hello: ssh failed, or the command isn't there. */
              int status = ssh_finish (ssh, false);
              if (status == 127 || status == 126)
                return HELLO_NOT_INSTALLED;
              g_autofree char *why = last_line (ssh->tail);
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't connect to %s: %s",
                           s->host, why != nullptr ? why : "ssh ended without a word");
              return HELLO_FAILED;
            }
        }
    }
}

/* The quoin-serve to send: QUOIN_SERVE, or the one next to this program. */
static char *
local_serve_path (void)
{
  const char *env = g_getenv ("QUOIN_SERVE");
  if (env != nullptr)
    return g_strdup (env);
  g_autofree char *self = g_file_read_link ("/proc/self/exe", nullptr);
  if (self == nullptr)
    return nullptr;
  g_autofree char *dir = g_path_get_dirname (self);
  return g_build_filename (dir, "quoin-serve", nullptr);
}

[[nodiscard]] static GBytes *
gzip (const void *data, size_t len, GError **error)
{
  g_autoptr (GZlibCompressor) z = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, 9);
  g_autoptr (GInputStream) raw = g_memory_input_stream_new_from_data (data, (gssize) len, nullptr);
  g_autoptr (GInputStream) packed = g_converter_input_stream_new (raw, G_CONVERTER (z));
  g_autoptr (GOutputStream) out = g_memory_output_stream_new_resizable ();
  if (g_output_stream_splice (out, packed, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, nullptr, error) < 0)
    return nullptr;
  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (out));
}

/* The install script (POSIX sh, run with `exec sh` on the other machine): the binary goes
 * to a temporary name, is checked against its SHA-256, then renamed into place. Everything
 * in it is ours: the build hash, a checksum and base64. */
[[nodiscard]] static GString *
install_script (GError **error)
{
  g_autofree char *path = local_serve_path ();
  g_autofree char *data = nullptr;
  gsize len = 0;
  if (path == nullptr || !g_file_get_contents (path, &data, &len, error))
    {
      if (error != nullptr && *error == nullptr)
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             "Can't find quoin-serve next to quoin");
      return nullptr;
    }
  if (len == 0 || len > SERVE_MAX)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "The quoin-serve next to quoin isn't a usable program");
      return nullptr;
    }
  g_autofree char *sum
      = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) data, len);
  g_autoptr (GBytes) packed = gzip (data, len, error);
  if (packed == nullptr)
    return nullptr;
  gsize plen;
  const guchar *p = g_bytes_get_data (packed, &plen);
  g_autofree char *dir = remote_dir ();

  GString *s = g_string_new ("");
  g_string_append_printf (s,
                          "set -eu\n"
                          "umask 077\n"
                          "d=%s\n"
                          "mkdir -p \"$d\"\n"
                          "t=\"$d/.quoin-serve.$$\"\n"
                          "trap 'rm -f \"$t\"' EXIT\n"
                          "{ base64 -d | gzip -dc; } > \"$t\" <<'QUOIN_PAYLOAD_END'\n",
                          dir);
  /* base64 in 76-character lines. */
  gsize state_len = plen / 3 * 4 + plen / 57 + 8;
  g_autofree char *b64 = g_malloc (state_len + 8);
  int state = 0, save = 0;
  gsize n = g_base64_encode_step (p, plen, true, b64, &state, &save);
  n += g_base64_encode_close (true, b64 + n, &state, &save);
  g_string_append_len (s, b64, (gssize) n);
  if (s->str[s->len - 1] != '\n')
    g_string_append_c (s, '\n');
  g_string_append_printf (s,
                          "QUOIN_PAYLOAD_END\n"
                          "echo \"%s  $t\" | sha256sum -c --status\n"
                          "chmod 700 \"$t\"\n"
                          "mv -f \"$t\" \"$d/quoin-serve\"\n"
                          "trap - EXIT\n"
                          /* Builds installed over 30 days ago, other than this one. */
                          "find .cache/quoin/serve -mindepth 1 -maxdepth 1 -type d ! -name %s "
                          "-mtime +30 -exec rm -rf {} + 2>/dev/null || true\n"
                          "echo " INSTALLED_MARK "\n",
                          sum, GBK_VERSION);
  return s;
}

/* Send quoin-serve to the other machine. */
[[nodiscard]] static bool
install (Shared *s, int64_t deadline, GError **error)
{
  g_autoptr (GString) script = install_script (error);
  if (script == nullptr)
    return false;
  Ssh ssh;
  if (!ssh_start (s, "exec sh", &ssh, error))
    return false;
  size_t sent = 0;
  g_autoptr (GString) output = g_string_new ("");
  bool eof = false;
  while (!eof)
    {
      int wait = remaining_ms (deadline);
      if (wait == 0 || closing (s))
        break;
      struct pollfd p[2] = { { ssh.sock, (short) (POLLIN | (sent < script->len ? POLLOUT : 0)), 0 },
                             { ssh.err, POLLIN, 0 } };
      if (poll (p, 2, wait) < 0 && errno != EINTR)
        break;
      if (p[1].revents)
        ssh_drain_stderr (&ssh);
      if ((p[0].revents & POLLOUT) && sent < script->len)
        {
          ssize_t n = send (ssh.sock, script->str + sent, script->len - sent, MSG_NOSIGNAL);
          if (n > 0)
            sent += (size_t) n;
          else if (n < 0 && errno != EAGAIN && errno != EINTR)
            break;
          if (sent == script->len)
            shutdown (ssh.sock, SHUT_WR); /* the end of the script: sh finishes */
        }
      if (p[0].revents & (POLLIN | POLLHUP))
        {
          char buf[4096];
          ssize_t n = read (ssh.sock, buf, sizeof buf);
          if (n > 0 && output->len < INSTALL_OUTPUT_MAX)
            g_string_append_len (output, buf, n);
          else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
            eof = true;
        }
    }
  int status = ssh_finish (&ssh, !eof);
  bool ok = eof && status == 0 && strstr (output->str, INSTALLED_MARK) != nullptr;
  if (!ok)
    {
      g_autofree char *why = last_line (ssh.tail);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Couldn't set up Quoin on %s: %s", s->host,
                   why != nullptr ? why : "the install step failed");
    }
  ssh_free (&ssh);
  return ok;
}

/* ---- the I/O thread: connected ---- */

typedef struct
{
  Shared *s;
  Ssh ssh;
  GByteArray *in;
} Live;

/* ---- worker-thread requests ---- */

typedef struct
{
  GbkMsgType type;
  GBytes *payload;
  GError *error; /* the connection died */
} CallItem;

typedef struct
{
  Shared *s; /* referenced */
  uint64_t channel;
  GAsyncQueue *q; /* CallItem* */
} Call;

static void
call_item_free (CallItem *it)
{
  g_clear_pointer (&it->payload, g_bytes_unref);
  g_clear_error (&it->error);
  g_free (it);
}

/* An answer for a worker's request: queued for it. True if the channel is one. */
static bool
route_to_call (Shared *s, const GbkFrame *f, bool *valid)
{
  g_mutex_lock (&s->calls_lock);
  Call *call = g_hash_table_lookup (s->calls, &f->channel);
  if (call != nullptr)
    {
      CallItem *it = g_new0 (CallItem, 1);
      it->type = f->type;
      it->payload = g_bytes_new (f->payload, f->len);
      g_async_queue_push (call->q, it);
    }
  g_mutex_unlock (&s->calls_lock);
  *valid = f->type == GBK_MSG_LISTING || f->type == GBK_MSG_STATED || f->type == GBK_MSG_DATA
           || f->type == GBK_MSG_END || f->type == GBK_MSG_FAILED;
  return call != nullptr;
}

/* The connection ended: every waiting request hears it now. */
static void
calls_dead (Shared *s, const GError *error)
{
  g_mutex_lock (&s->calls_lock);
  s->dead = true;
  s->dead_error = g_error_copy (error);
  GHashTableIter it;
  void *call;
  g_hash_table_iter_init (&it, s->calls);
  while (g_hash_table_iter_next (&it, nullptr, &call))
    {
      CallItem *item = g_new0 (CallItem, 1);
      item->error = g_error_copy (error);
      g_async_queue_push (((Call *) call)->q, item);
    }
  g_mutex_unlock (&s->calls_lock);
}

[[nodiscard]] static Call *
call_start (Shared *s, GError **error)
{
  g_mutex_lock (&s->calls_lock);
  if (s->dead)
    {
      g_propagate_error (error, g_error_copy (s->dead_error));
      g_mutex_unlock (&s->calls_lock);
      return nullptr;
    }
  Call *call = g_new0 (Call, 1);
  g_atomic_ref_count_inc (&s->rc);
  call->s = s;
  call->channel = new_channel ();
  call->q = g_async_queue_new_full ((GDestroyNotify) call_item_free);
  g_hash_table_insert (s->calls, &call->channel, call);
  g_mutex_unlock (&s->calls_lock);
  return call;
}

static void
call_end (Call *call)
{
  g_mutex_lock (&call->s->calls_lock);
  g_hash_table_remove (call->s->calls, &call->channel);
  g_mutex_unlock (&call->s->calls_lock);
  g_async_queue_unref (call->q);
  shared_unref (call->s);
  g_free (call);
}

static void
call_send (const Call *call, GByteArray *frame)
{
  g_mutex_lock (&call->s->lock);
  g_byte_array_append (call->s->out, frame->data, frame->len);
  g_mutex_unlock (&call->s->lock);
  poke (call->s->wake_io);
}

/* The next answer, checking `src`'s cancellation while waiting. nullptr with *error when
 * cancelled or the connection died. */
[[nodiscard]] static CallItem *
call_wait (Call *call, const GbkSource *src, GError **error)
{
  for (;;)
    {
      CallItem *it = g_async_queue_timeout_pop (call->q, G_USEC_PER_SEC / 10);
      if (it != nullptr && it->error != nullptr)
        {
          g_propagate_error (error, g_steal_pointer (&it->error));
          call_item_free (it);
          return nullptr;
        }
      if (it != nullptr)
        return it;
      if (src->cancelled != nullptr && src->cancelled (src->cancel_data))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          return nullptr;
        }
    }
}

static GbkFrame
item_frame (const CallItem *it, uint64_t channel)
{
  gsize len;
  const uint8_t *data = g_bytes_get_data (it->payload, &len);
  return (GbkFrame) { it->type, channel, data, len };
}

/* An answer that isn't one of the expected kinds: the server's FAILED, or a broken reply. */
static void
unexpected (const CallItem *it, uint64_t channel, GError **error)
{
  GbkFrame f = item_frame (it, channel);
  if (f.type == GBK_MSG_FAILED && gbk_proto_get_failed (&f, error))
    return;
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       "The other machine answered with something Quoin doesn't understand");
}

/* ---- another machine as a copy's source ---- */

#define READ_WINDOW (1u << 20) /* bytes a READ may send ahead of what's been copied */

typedef struct
{
  GbkSource base;
  Shared *s; /* referenced */
} RemoteSource;

typedef struct
{
  GbkSourceFile base;
  Call *call;
  GBytes *chunk; /* the DATA being copied out */
  size_t off;
  uint32_t unacked; /* bytes copied out since the last CREDIT */
  bool ended;
} RemoteFile;

static Shared *
shared_of (const GbkSource *src)
{
  return ((const RemoteSource *) (const void *) src)->s;
}

[[nodiscard]] static GbkStatEntry *
remote_lstat (GbkSource *src, const char *path, GError **error)
{
  Call *call = call_start (shared_of (src), error);
  if (call == nullptr)
    return nullptr;
  g_autoptr (GByteArray) req = g_byte_array_new ();
  gbk_proto_put_path (req, GBK_MSG_LSTAT, call->channel, path);
  call_send (call, req);
  GbkStatEntry *e = nullptr;
  CallItem *it = call_wait (call, src, error);
  if (it != nullptr)
    {
      GbkFrame f = item_frame (it, call->channel);
      e = f.type == GBK_MSG_STATED ? gbk_proto_get_stated (&f) : nullptr;
      if (e == nullptr)
        unexpected (it, call->channel, error);
      call_item_free (it);
    }
  call_end (call);
  return e;
}

[[nodiscard]] static GPtrArray *
remote_list (GbkSource *src, const char *path, GError **error)
{
  Call *call = call_start (shared_of (src), error);
  if (call == nullptr)
    return nullptr;
  g_autoptr (GByteArray) req = g_byte_array_new ();
  gbk_proto_put_path (req, GBK_MSG_LIST, call->channel, path);
  call_send (call, req);
  g_autoptr (GPtrArray) all = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_stat_entry_free);
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  bool last = false, ok = true;
  while (ok && !last)
    {
      CallItem *it = call_wait (call, src, error);
      if (it == nullptr)
        break;
      GbkFrame f = item_frame (it, call->channel);
      GPtrArray *part = nullptr;
      ok = f.type == GBK_MSG_LISTING && gbk_proto_get_listing (&f, &part, &last);
      if (!ok)
        unexpected (it, call->channel, error);
      for (guint i = 0; ok && i < part->len; i++)
        {
          GbkStatEntry *e = g_ptr_array_index (part, i);
          if (!g_hash_table_add (seen, e->name))
            {
              ok = false; /* the same name in two parts */
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                   "The other machine listed a name twice");
              break;
            }
          g_ptr_array_add (all, e);
          part->pdata[i] = nullptr; /* moved */
        }
      if (part != nullptr)
        g_ptr_array_unref (part);
      call_item_free (it);
    }
  call_end (call);
  return ok && last ? g_steal_pointer (&all) : nullptr;
}

[[nodiscard]] static GbkSourceFile *
remote_open (GbkSource *src, const char *path, GError **error)
{
  Call *call = call_start (shared_of (src), error);
  if (call == nullptr)
    return nullptr;
  g_autoptr (GByteArray) req = g_byte_array_new ();
  gbk_proto_put_read (req, call->channel, path, READ_WINDOW);
  call_send (call, req);
  RemoteFile *f = g_new0 (RemoteFile, 1);
  f->base.source = src;
  f->call = call;
  return &f->base;
}

[[nodiscard]] static ssize_t
remote_read (GbkSourceFile *file, void *buf, size_t len, GError **error)
{
  RemoteFile *f = (RemoteFile *) (void *) file;
  while (f->chunk == nullptr || f->off == g_bytes_get_size (f->chunk))
    {
      g_clear_pointer (&f->chunk, g_bytes_unref);
      if (f->ended)
        return 0;
      CallItem *it = call_wait (f->call, file->source, error);
      if (it == nullptr)
        return -1;
      GbkFrame frame = item_frame (it, f->call->channel);
      const uint8_t *data;
      size_t n;
      bool changed;
      if (gbk_proto_get_data (&frame, &data, &n))
        {
          f->chunk = g_bytes_ref (it->payload);
          f->off = 0;
        }
      else if (gbk_proto_get_end (&frame, &changed))
        {
          f->ended = true;
          if (changed)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "It changed while it was being copied");
              call_item_free (it);
              return -1;
            }
        }
      else
        {
          f->ended = true; /* FAILED ends it on the other side too */
          unexpected (it, f->call->channel, error);
          call_item_free (it);
          return -1;
        }
      call_item_free (it);
    }
  size_t take = MIN (len, g_bytes_get_size (f->chunk) - f->off);
  memcpy (buf, (const uint8_t *) g_bytes_get_data (f->chunk, nullptr) + f->off, take);
  f->off += take;
  f->unacked += (uint32_t) take;
  if (f->unacked >= READ_WINDOW / 2)
    {
      g_autoptr (GByteArray) credit = g_byte_array_new ();
      gbk_proto_put_credit (credit, f->call->channel, f->unacked);
      call_send (f->call, credit);
      f->unacked = 0;
    }
  return (ssize_t) take;
}

static int
remote_fd (const GbkSourceFile *f G_GNUC_UNUSED)
{
  return -1;
}

static void
remote_close (GbkSourceFile *file)
{
  RemoteFile *f = (RemoteFile *) (void *) file;
  if (!f->ended)
    {
      g_autoptr (GByteArray) cancel = g_byte_array_new ();
      gbk_proto_put_empty (cancel, GBK_MSG_CANCEL, f->call->channel);
      call_send (f->call, cancel);
    }
  g_clear_pointer (&f->chunk, g_bytes_unref);
  call_end (f->call);
  g_free (f);
}

static void
remote_free (GbkSource *src)
{
  RemoteSource *r = (RemoteSource *) (void *) src;
  Shared *s = r->s;
  g_mutex_lock (&s->lock);
  if (g_atomic_int_dec_and_test (&s->workers))
    s->workers_done_us = g_get_monotonic_time ();
  g_mutex_unlock (&s->lock);
  shared_unref (s);
  g_free (r);
}

static const GbkSourceOps remote_ops = {
  .lstat = remote_lstat,
  .list = remote_list,
  .open = remote_open,
  .read = remote_read,
  .fd = remote_fd,
  .close = remote_close,
  .free = remote_free,
};

/* Decode one frame from the server into an event. False: it broke the protocol. */
static bool
on_frame (Live *l, const GbkFrame *f)
{
  bool valid;
  if (route_to_call (l->s, f, &valid))
    return valid;
  Event *e = g_new0 (Event, 1);
  e->channel = f->channel;
  switch (f->type)
    {
    case GBK_MSG_SCAN:
      e->type = EV_SCAN;
      e->scan = gbk_proto_get_scan (f, l->s->types);
      if (e->scan == nullptr || f->channel == 0)
        break;
      post (l->s, e);
      return true;
    case GBK_MSG_INFO:
      e->type = EV_INFO;
      e->info = gbk_proto_get_info (f);
      if (e->info == nullptr)
        break;
      post (l->s, e);
      return true;
    case GBK_MSG_FAILED:
      e->type = EV_FAILED;
      if (!gbk_proto_get_failed (f, &e->error))
        break;
      post (l->s, e);
      return true;
    case GBK_MSG_PONG: event_free (e); return gbk_proto_get_empty (f);
    case GBK_MSG_LISTING: /* answers to a worker's request that has ended */
    case GBK_MSG_STATED:
    case GBK_MSG_DATA:
    case GBK_MSG_END: event_free (e); return true;
    case GBK_MSG_LIST:
    case GBK_MSG_LSTAT:
    case GBK_MSG_READ:
    case GBK_MSG_CREDIT:
    case GBK_MSG_CANCEL:
    case GBK_MSG_HELLO:
    case GBK_MSG_WATCH:
    case GBK_MSG_UNWATCH:
    case GBK_MSG_GET_INFO:
    case GBK_MSG_PING:
    default: break;
    }
  event_free (e);
  return false;
}

/* Run the connection until it ends; *error says how. */
static void
run (Live *l, GError **error)
{
  Shared *s = l->s;
  int64_t now = g_get_monotonic_time ();
  int64_t last_rx = now, next_ping = now + (int64_t) s->ping_ms * 1000;
  int64_t close_by = 0; /* closing: when to stop waiting for ssh */
  for (;;)
    {
      /* Whatever arrived: frames to events. */
      size_t at = 0;
      for (;;)
        {
          GbkFrame f;
          size_t used;
          int r = gbk_proto_next_frame (l->in->data + at, l->in->len - at, &f, &used);
          if (r == 0)
            break;
          if (r < 0 || !on_frame (l, &f))
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                           "%s sent something Quoin doesn't understand", s->host);
              return;
            }
          at += used;
        }
      g_byte_array_remove_range (l->in, 0, (guint) at);

      now = g_get_monotonic_time ();
      if (close_by == 0 && closing (s))
        {
          shutdown (l->ssh.sock, SHUT_WR); /* the server stops at end of input */
          close_by = now + (int64_t) CLOSE_GRACE_MS * 1000;
        }
      if (close_by != 0 && now >= close_by)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Closed");
          return;
        }
      if (close_by == 0 && now - last_rx >= (int64_t) s->dead_ms * 1000)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "No answer from %s for %d seconds",
                       s->host, s->dead_ms / 1000);
          return;
        }
      if (close_by == 0 && now >= next_ping)
        {
          g_mutex_lock (&s->lock);
          gbk_proto_put_empty (s->out, GBK_MSG_PING, 0);
          g_mutex_unlock (&s->lock);
          next_ping = now + (int64_t) s->ping_ms * 1000;
        }

      g_mutex_lock (&s->lock);
      bool pending = s->out->len > 0 && close_by == 0;
      g_mutex_unlock (&s->lock);
      int64_t until
          = close_by != 0 ? close_by : MIN (next_ping, last_rx + (int64_t) s->dead_ms * 1000);
      struct pollfd p[3] = {
        { l->ssh.sock, (short) (POLLIN | (pending ? POLLOUT : 0)), 0 },
        { l->ssh.err, POLLIN, 0 },
        { s->wake_io, POLLIN, 0 },
      };
      if (poll (p, 3, remaining_ms (until)) < 0 && errno != EINTR)
        {
          g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (errno), "poll: %s",
                       g_strerror (errno));
          return;
        }
      if (p[2].revents & POLLIN)
        drain_eventfd (s->wake_io);
      if (p[1].revents)
        ssh_drain_stderr (&l->ssh);
      if (p[0].revents & POLLOUT)
        {
          g_mutex_lock (&s->lock);
          ssize_t n = send (l->ssh.sock, s->out->data, s->out->len, MSG_NOSIGNAL);
          int err = errno;
          if (n > 0)
            g_byte_array_remove_range (s->out, 0, (guint) n);
          g_mutex_unlock (&s->lock);
          if (n < 0 && err != EAGAIN && err != EINTR)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                           "The connection to %s ended", s->host);
              return;
            }
        }
      if (p[0].revents & (POLLIN | POLLHUP | POLLERR))
        {
          uint8_t buf[READ_CHUNK];
          ssize_t n = read (l->ssh.sock, buf, sizeof buf);
          if (n > 0)
            {
              g_byte_array_append (l->in, buf, (guint) n);
              last_rx = g_get_monotonic_time ();
            }
          else if (n == 0 || (errno != EAGAIN && errno != EINTR))
            {
              if (close_by != 0)
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Closed");
              else
                {
                  ssh_drain_stderr (&l->ssh);
                  g_autofree char *why = last_line (l->ssh.tail);
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                               "The connection to %s ended%s%s", s->host,
                               why != nullptr ? ": " : "", why != nullptr ? why : "");
                }
              return;
            }
        }
    }
}

static void *
io_main (void *data)
{
  Shared *s = data;
  int64_t deadline = g_get_monotonic_time () + (int64_t) s->connect_ms * 1000;
  g_autoptr (GError) error = nullptr;
  Live l = { .s = s, .in = g_byte_array_new () };
  GbkHello hello = { 0 };
  bool connected = false, needs_login = false;
  for (int attempt = 0; attempt < 2 && !connected && error == nullptr; attempt++)
    {
      g_autofree char *cmd = remote_serve ();
      if (!ssh_start (s, cmd, &l.ssh, &error))
        break;
      HelloResult r = await_hello (s, &l.ssh, deadline, l.in, &hello, &error);
      if (r == HELLO_OK)
        connected = true;
      else
        {
          ssh_finish (&l.ssh, true); /* first: it collects the last of stderr */
          needs_login = login_needed (l.ssh.tail);
          ssh_free (&l.ssh);
          g_byte_array_set_size (l.in, 0);
          if (r == HELLO_NOT_INSTALLED && attempt == 0 && !install (s, deadline, &error))
            break;
          if (r == HELLO_NOT_INSTALLED && attempt == 1 && error == nullptr)
            g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Quoin on %s didn't start after it was set up", s->host);
        }
    }

  if (connected)
    {
      Event *ready = g_new0 (Event, 1);
      ready->type = EV_READY;
      ready->hello = hello; /* moved */
      post (s, ready);
      run (&l, &error);
      needs_login = false;
    }
  ssh_free (&l.ssh);
  Event *dead = g_new0 (Event, 1);
  dead->type = EV_DEAD;
  dead->error = g_steal_pointer (&error);
  if (dead->error == nullptr)
    dead->error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "The connection ended");
  calls_dead (s, dead->error);
  dead->needs_login = needs_login;
  post (s, dead);
  g_byte_array_unref (l.in);
  shared_unref (s);
  g_atomic_int_add (&live_conns, -1);
  return nullptr;
}

/* ---- main thread ---- */

struct GbkConnWatch
{
  GbkConn *conn; /* referenced */
  uint64_t channel;
  GWeakRef owner;
  GbkConnScanFn fn;
  void *data;
  bool ended; /* told the connection died */
  guint idle; /* the "died" notice for a watch made on a dead connection */
};

typedef struct
{
  GbkConn *conn; /* referenced: a pending request keeps its connection */
  GWeakRef owner;
  GbkConnInfoFn fn;
  void *data;
} InfoReq;

struct _GbkConn
{
  GObject parent_instance;
  Shared *s;
  guint watch;
  GbkConnState state;
  GbkHello hello;
  bool have_hello;
  bool this_machine;
  char *error;
  bool needs_login;
  GHashTable *watches; /* &channel -> GbkConnWatch* (borrowed: owned by the caller) */
  GHashTable *infos; /* &channel (owned key) -> InfoReq* (owned) */
  int64_t unused_since; /* monotonic µs; 0 while a watch or request is open */
};

G_DEFINE_FINAL_TYPE (GbkConn, gbk_conn, G_TYPE_OBJECT)

enum
{
  SIGNAL_STATE_CHANGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void
info_req_free (InfoReq *r)
{
  g_weak_ref_clear (&r->owner);
  g_object_unref (r->conn);
  g_free (r);
}

static void
deliver_info (InfoReq *r, GbkFileInfo *info, const GError *error)
{
  g_autoptr (GObject) owner = g_weak_ref_get (&r->owner);
  if (owner != nullptr)
    r->fn (info, error, r->data);
  else
    gbk_file_info_free (info);
}

static void
watch_ended (GbkConnWatch *w)
{
  if (w->ended)
    return;
  w->ended = true;
  g_autoptr (GObject) owner = g_weak_ref_get (&w->owner);
  if (owner != nullptr)
    w->fn (nullptr, w->data);
}

/* Keep unused_since current: call after any watch or request starts or ends. */
static void
note_use (GbkConn *c)
{
  bool used = g_hash_table_size (c->watches) > 0 || g_hash_table_size (c->infos) > 0;
  if (used)
    c->unused_since = 0;
  else if (c->unused_since == 0)
    c->unused_since = g_get_monotonic_time ();
}

int64_t
gbk_conn_unused_since (const GbkConn *c)
{
  /* A copy reading through it is a use too. */
  if (g_atomic_int_get (&c->s->workers) > 0 || c->unused_since == 0)
    return 0;
  g_mutex_lock (&c->s->lock);
  int64_t done = c->s->workers_done_us;
  g_mutex_unlock (&c->s->lock);
  return MAX (c->unused_since, done);
}

static void
set_dead (GbkConn *c, const GError *error, bool needs_login)
{
  c->state = GBK_CONN_DEAD;
  g_free (c->error);
  c->error = g_strdup (error->message);
  c->needs_login = needs_login;
  /* Callbacks may unwatch (any watch) or start other requests: work from a copy of the
   * channels, and look each one up again before using it. */
  g_autoptr (GArray) channels = g_array_new (false, false, sizeof (uint64_t));
  GHashTableIter wit;
  void *w;
  g_hash_table_iter_init (&wit, c->watches);
  while (g_hash_table_iter_next (&wit, nullptr, &w))
    g_array_append_val (channels, ((GbkConnWatch *) w)->channel);
  for (guint i = 0; i < channels->len; i++)
    {
      GbkConnWatch *live = g_hash_table_lookup (c->watches, &g_array_index (channels, uint64_t, i));
      if (live != nullptr)
        watch_ended (live);
    }
  g_autoptr (GHashTable) infos = g_steal_pointer (&c->infos);
  c->infos
      = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, (GDestroyNotify) info_req_free);
  GHashTableIter it;
  void *req;
  g_hash_table_iter_init (&it, infos);
  while (g_hash_table_iter_next (&it, nullptr, &req))
    deliver_info (req, nullptr, error);
  note_use (c);
  g_signal_emit (c, signals[SIGNAL_STATE_CHANGED], 0);
}

static void
dispatch (GbkConn *c, Event *e)
{
  switch (e->type)
    {
    case EV_READY:
      {
        c->hello = e->hello; /* moved */
        e->hello = (GbkHello) { 0 };
        c->have_hello = true;
        g_autofree char *mine = gbk_conn_local_machine_id ();
        c->this_machine = mine[0] != '\0' && g_str_equal (mine, c->hello.machine_id);
        c->state = GBK_CONN_READY;
        g_signal_emit (c, signals[SIGNAL_STATE_CHANGED], 0);
        break;
      }
    case EV_SCAN:
      {
        GbkConnWatch *w = g_hash_table_lookup (c->watches, &e->channel);
        if (w == nullptr)
          break; /* unwatched meanwhile */
        g_autoptr (GObject) owner = g_weak_ref_get (&w->owner);
        if (owner != nullptr)
          w->fn (g_steal_pointer (&e->scan), w->data);
        break;
      }
    case EV_INFO:
    case EV_FAILED:
      {
        uint64_t ch = e->channel;
        InfoReq *r = nullptr;
        void *key;
        if (!g_hash_table_steal_extended (c->infos, &ch, &key, (void **) &r))
          break; /* not ours: ignore it */
        g_free (key);
        note_use (c);
        deliver_info (r, g_steal_pointer (&e->info), e->error);
        info_req_free (r);
        break;
      }
    case EV_DEAD: set_dead (c, e->error, e->needs_login); break;
    default: gbk_fatal ("bad connection event %d", (int) e->type);
    }
}

static bool
on_events (int fd G_GNUC_UNUSED, GIOCondition cond G_GNUC_UNUSED, void *data)
{
  GbkConn *c = data;
  drain_eventfd (c->s->wake_main);
  g_object_ref (c); /* a callback may drop the last other reference */
  for (int i = 0; i < EVENTS_PER_WAKE; i++)
    {
      Event *e = g_async_queue_try_pop (c->s->events);
      if (e == nullptr)
        break;
      dispatch (c, e);
      event_free (e);
      if (i == EVENTS_PER_WAKE - 1)
        poke (c->s->wake_main); /* more waiting: next main loop round */
    }
  g_object_unref (c);
  return true;
}

static void
send_frame (GbkConn *c, void (*put) (GByteArray *, void *), void *arg)
{
  g_mutex_lock (&c->s->lock);
  put (c->s->out, arg);
  g_mutex_unlock (&c->s->lock);
  poke (c->s->wake_io);
}

typedef struct
{
  GbkMsgType type;
  uint64_t channel;
  const char *path;
} PathFrame;

static void
put_path_frame (GByteArray *out, void *arg)
{
  const PathFrame *f = arg;
  if (f->path != nullptr)
    gbk_proto_put_path (out, f->type, f->channel, f->path);
  else
    gbk_proto_put_empty (out, f->type, f->channel);
}

static bool
ended_idle (void *data)
{
  GbkConnWatch *w = data;
  w->idle = 0;
  watch_ended (w);
  return false;
}

GbkConnWatch *
gbk_conn_watch (GbkConn *c, const char *path, GObject *owner, GbkConnScanFn fn, void *data)
{
  GbkConnWatch *w = g_new0 (GbkConnWatch, 1);
  w->conn = g_object_ref (c);
  w->channel = new_channel ();
  g_weak_ref_init (&w->owner, owner);
  w->fn = fn;
  w->data = data;
  if (c->state == GBK_CONN_DEAD)
    {
      /* Owned by the connection, which the watch keeps alive; unwatching cancels it. */
      w->idle = gbk_idle_add (G_OBJECT (c), ended_idle, w);
      return w;
    }
  g_hash_table_insert (c->watches, &w->channel, w);
  note_use (c);
  PathFrame f = { GBK_MSG_WATCH, w->channel, path };
  send_frame (c, put_path_frame, &f);
  return w;
}

void
gbk_conn_unwatch (GbkConnWatch *w)
{
  if (w == nullptr)
    return;
  gbk_source_cancel (&w->idle);
  GbkConn *c = w->conn;
  if (g_hash_table_remove (c->watches, &w->channel) && c->state != GBK_CONN_DEAD)
    {
      PathFrame f = { GBK_MSG_UNWATCH, w->channel, nullptr };
      send_frame (c, put_path_frame, &f);
    }
  note_use (c);
  g_weak_ref_clear (&w->owner);
  g_object_unref (c);
  g_free (w);
}

typedef struct
{
  InfoReq *req;
  GError *error;
} DeadInfo;

static bool
dead_info_idle (void *data)
{
  DeadInfo *d = data;
  deliver_info (d->req, nullptr, d->error);
  info_req_free (d->req);
  g_error_free (d->error);
  g_free (d);
  return false;
}

void
gbk_conn_info (GbkConn *c, const char *path, GObject *owner, GbkConnInfoFn fn, void *data)
{
  InfoReq *r = g_new0 (InfoReq, 1);
  r->conn = g_object_ref (c);
  g_weak_ref_init (&r->owner, owner);
  r->fn = fn;
  r->data = data;
  if (c->state == GBK_CONN_DEAD)
    {
      /* Answer from an idle, as for a live connection (never inside this call). The idle
       * belongs to the connection, which the request keeps alive until it has run. */
      DeadInfo *d = g_new0 (DeadInfo, 1);
      d->req = r;
      d->error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, c->error);
      gbk_idle_add (G_OBJECT (c), dead_info_idle, d);
      return;
    }
  uint64_t *key = g_new (uint64_t, 1);
  *key = new_channel ();
  g_hash_table_insert (c->infos, key, r);
  note_use (c);
  PathFrame f = { GBK_MSG_GET_INFO, *key, path };
  send_frame (c, put_path_frame, &f);
}

GbkConn *
gbk_conn_new (const char *host, GbkTypeTable *types, GError **error)
{
  if (!gbk_host_valid (host))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "“%s” isn't a machine name ssh would take", host);
      return nullptr;
    }
  Shared *s = g_new0 (Shared, 1);
  g_atomic_ref_count_init (&s->rc);
  s->host = g_strdup (host);
  s->types = gbk_type_table_ref (types);
  int ping = g_atomic_int_get (&test_ping_ms), dead = g_atomic_int_get (&test_dead_ms),
      conn = g_atomic_int_get (&test_connect_ms);
  s->ping_ms = ping > 0 ? ping : GBK_CONN_PING_MS;
  s->dead_ms = dead > 0 ? dead : GBK_CONN_DEAD_MS;
  s->connect_ms = conn > 0 ? conn : GBK_CONN_CONNECT_MS;
  g_mutex_init (&s->lock);
  s->out = g_byte_array_new ();
  s->wake_io = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  s->wake_main = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  s->events = g_async_queue_new_full ((GDestroyNotify) event_free);
  g_mutex_init (&s->calls_lock);
  s->calls = g_hash_table_new (g_int64_hash, g_int64_equal);
  if (s->wake_io < 0 || s->wake_main < 0)
    gbk_fatal ("eventfd: %s", g_strerror (errno));

  GbkConn *c = g_object_new (GBK_TYPE_CONN, nullptr);
  c->s = s;
  c->state = GBK_CONN_CONNECTING;
  c->unused_since = g_get_monotonic_time ();
  c->watch = gbk_fd_watch (G_OBJECT (c), s->wake_main, G_IO_IN, on_events, c);
  g_atomic_ref_count_inc (&s->rc); /* the I/O thread's */
  g_atomic_int_inc (&live_conns);
  g_autoptr (GError) thread_error = nullptr;
  GThread *t = g_thread_try_new ("gbk-conn", io_main, s, &thread_error);
  if (t == nullptr)
    {
      Event *e = g_new0 (Event, 1);
      e->type = EV_DEAD;
      e->error = g_steal_pointer (&thread_error);
      post (s, e); /* delivered the usual way */
      g_atomic_int_add (&live_conns, -1);
      shared_unref (s); /* the thread's reference, last: the connection still holds one */
    }
  else
    g_thread_unref (t);
  return c;
}

const char *
gbk_conn_host (const GbkConn *c)
{
  return c->s->host;
}

GbkConnState
gbk_conn_state (const GbkConn *c)
{
  return c->state;
}

const GbkHello *
gbk_conn_hello (const GbkConn *c)
{
  return c->have_hello ? &c->hello : nullptr;
}

bool
gbk_conn_is_this_machine (const GbkConn *c)
{
  return c->this_machine;
}

const char *
gbk_conn_error (const GbkConn *c)
{
  return c->error;
}

bool
gbk_conn_needs_login (const GbkConn *c)
{
  return c->needs_login;
}

void
gbk_conn_close (GbkConn *c)
{
  g_mutex_lock (&c->s->lock);
  c->s->closing = true;
  g_mutex_unlock (&c->s->lock);
  poke (c->s->wake_io);
}

static void
gbk_conn_dispose (GObject *o)
{
  GbkConn *c = GBK_CONN (o);
  if (c->s != nullptr)
    gbk_conn_close (c); /* the I/O thread finishes on its own */
  G_OBJECT_CLASS (gbk_conn_parent_class)->dispose (o);
}

static void
gbk_conn_finalize (GObject *o)
{
  GbkConn *c = GBK_CONN (o);
  /* Watches hold a reference, so none is left; pending requests hold one too. */
  g_hash_table_unref (c->watches);
  g_hash_table_unref (c->infos);
  gbk_hello_clear (&c->hello);
  g_free (c->error);
  shared_unref (c->s);
  G_OBJECT_CLASS (gbk_conn_parent_class)->finalize (o);
}

static void
gbk_conn_class_init (GbkConnClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = gbk_conn_dispose;
  G_OBJECT_CLASS (klass)->finalize = gbk_conn_finalize;
  signals[SIGNAL_STATE_CHANGED]
      = g_signal_new ("state-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                      nullptr, nullptr, G_TYPE_NONE, 0);
}

static void
gbk_conn_init (GbkConn *c)
{
  c->watches = g_hash_table_new (g_int64_hash, g_int64_equal);
  c->infos
      = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, (GDestroyNotify) info_req_free);
}

GbkSource *
gbk_conn_source (GbkConn *c)
{
  RemoteSource *r = g_new0 (RemoteSource, 1);
  r->base.ops = &remote_ops;
  r->base.local = false;
  g_atomic_ref_count_inc (&c->s->rc);
  r->s = c->s;
  g_atomic_int_inc (&c->s->workers);
  return &r->base;
}
