/* quoin-serve: Quoin's remote end (docs/remote-plan.md). The laptop runs it over ssh and
 * talks to it through stdin/stdout with the protocol in proto.h. It lists and watches
 * folders with the same scanner a local window uses, and reads file details, all on this
 * machine. No GTK, no display, no D-Bus.
 *
 *   quoin-serve                  serve on stdin/stdout until stdin closes
 *   quoin-serve --shell-at HEX   start a login shell in the folder whose path is HEX
 *                                (hex-encoded bytes, so no filename ever reaches a shell
 *                                parser on its way here)
 *   quoin-serve --version
 *
 * One thread runs a poll loop over stdin, stdout and the scanners' wake descriptors, with
 * non-blocking I/O both ways: it never blocks on a slow laptop, so it can't deadlock with
 * it. Scanners, requests (peer.h) and jobs run on their own threads, so a hung filesystem
 * here holds up only its own request. Jobs read the laptop's files through it when they
 * copy from there.
 *
 * The laptop pings every few seconds. Silence for IDLE_MS means it is gone without closing
 * the connection (asleep, or its network changed: TCP may not notice for hours), so this
 * process exits rather than hold its watches open (each holds inotify watches and folders
 * open on this machine). GBK_SERVE_IDLE_MS shortens it, for tests. */
#include "peer.h"

#define READ_CHUNK ((size_t) 64 * 1024)
/* Output waiting for the laptop: above this, scanners aren't drained until it shrinks
 * (their messages wait in the scanners' own queues). */
#define OUT_HIGH ((size_t) 4 * 1024 * 1024)
#define SCAN_MSGS_PER_ROUND 64
#define IDLE_MS 30000
#define IDLE_MIN_MS 100
#define JOBS_STOP_MS 2000       /* at exit: time for jobs to remove their temp files */

typedef struct
{
  uint64_t channel;
  GbkScanner *scanner;
} Watch;

typedef struct
{
  GByteArray *in;
  GbkOutbox *out;         /* frames for the laptop, from any thread */
  GHashTable *watches;    /* &Watch.channel -> Watch*, owned */
  GbkTypeTable *types;
  GbkAnswerer *answerer;  /* the laptop's requests about this machine's files */
  GbkAsker *asker;        /* jobs' requests about the laptop's files */
  GbkJobRunner *jobs;
} Server;

static void
watch_free (Watch *w)
{
  gbk_scanner_stop (w->scanner);
  g_free (w);
}

[[noreturn]] static void
protocol_error (const char *what)
{
  fprintf (stderr, "quoin-serve: protocol error: %s\n", what);
  exit (3);
}

/* ---- requests ---- */

static void
handle (Server *sv, const GbkFrame *f)
{
  bool valid;
  if (gbk_asker_route (sv->asker, f, &valid))
    {
      if (!valid)
        protocol_error ("not an answer");
      return;
    }
  /* CANCEL is for a read or a job: both look. */
  if (gbk_answerer_takes (f->type) && !gbk_answerer_handle (sv->answerer, f))
    protocol_error ("bad request");
  if (gbk_job_runner_takes (f->type) && !gbk_job_runner_handle (sv->jobs, f))
    protocol_error ("bad job");
  if (gbk_answerer_takes (f->type) || gbk_job_runner_takes (f->type))
    return;
  switch (f->type)
    {
    case GBK_MSG_WATCH:
      {
        g_autofree char *path = gbk_proto_get_path (f);
        if (path == nullptr || f->channel == 0)
          protocol_error ("bad watch request");
        if (g_hash_table_contains (sv->watches, &f->channel))
          protocol_error ("channel already in use");
        Watch *w = g_new0 (Watch, 1);
        w->channel = f->channel;
        w->scanner = gbk_scanner_start (path, sv->types);
        g_hash_table_insert (sv->watches, &w->channel, w);
        break;
      }
    case GBK_MSG_UNWATCH:
      if (!gbk_proto_get_empty (f))
        protocol_error ("bad unwatch request");
      g_hash_table_remove (sv->watches, &f->channel);     /* already finished: nothing */
      break;
    case GBK_MSG_PING:
      if (!gbk_proto_get_empty (f))
        protocol_error ("bad ping");
      gbk_proto_put_empty (gbk_outbox_begin (sv->out), GBK_MSG_PONG, f->channel);
      gbk_outbox_end (sv->out);
      break;
    case GBK_MSG_LISTING:              /* answers to a job's request that has ended */
    case GBK_MSG_STATED:
    case GBK_MSG_DATA:
    case GBK_MSG_END:
    case GBK_MSG_REMOVED:
    case GBK_MSG_FAILED: break;
    case GBK_MSG_HELLO:
    case GBK_MSG_SCAN:
    case GBK_MSG_INFO:
    case GBK_MSG_PONG:
    case GBK_MSG_QUESTION:
    case GBK_MSG_PROGRESS:
    case GBK_MSG_CREATED:
    case GBK_MSG_DONE:
    case GBK_MSG_GET_INFO:
    case GBK_MSG_LIST:
    case GBK_MSG_LSTAT:
    case GBK_MSG_READ:
    case GBK_MSG_CREDIT:
    case GBK_MSG_CANCEL:
    case GBK_MSG_REMOVE:
    case GBK_MSG_SESSION:
    case GBK_MSG_FORGET:
    case GBK_MSG_SOURCES:
    case GBK_MSG_JOB:
    case GBK_MSG_ANSWER:
    case GBK_MSG_RESOLVE:
    case GBK_MSG_RESOLVED:
    case GBK_MSG_UPLOADED:
    default: protocol_error ("a message only the server sends");
    }
}

/* Returns false at end of input. */
static bool
read_input (Server *sv, int64_t *last_input)
{
  uint8_t buf[READ_CHUNK];
  ssize_t n = read (STDIN_FILENO, buf, sizeof buf);
  if (n < 0)
    return errno == EAGAIN || errno == EINTR;
  if (n == 0)
    return false;
  *last_input = g_get_monotonic_time ();
  g_byte_array_append (sv->in, buf, (guint) n);
  size_t at = 0;
  for (;;)
    {
      GbkFrame f;
      size_t used;
      int r = gbk_proto_next_frame (sv->in->data + at, sv->in->len - at, &f, &used);
      if (r < 0)
        protocol_error ("malformed frame");
      if (r == 0)
        break;
      handle (sv, &f);
      at += used;
    }
  g_byte_array_remove_range (sv->in, 0, (guint) at);
  return true;
}

/* False: the laptop went away (EPIPE). */
static bool
write_output (Server *sv)
{
  GByteArray *out = gbk_outbox_begin (sv->out);
  ssize_t n = out->len > 0 ? write (STDOUT_FILENO, out->data, out->len) : 0;
  int err = errno;
  if (n > 0)
    g_byte_array_remove_range (out, 0, (guint) n);
  gbk_outbox_end (sv->out);
  return n >= 0 || err == EAGAIN || err == EINTR;
}

static void
drain_scanner (Server *sv, Watch *w)
{
  gbk_scanner_ack (w->scanner);
  for (int i = 0; i < SCAN_MSGS_PER_ROUND; i++)
    {
      g_autoptr (GbkScanMsg) m = gbk_scanner_pop (w->scanner);
      if (m == nullptr)
        return;
      gbk_proto_put_scan (gbk_outbox_begin (sv->out), w->channel, m);
      gbk_outbox_end (sv->out);
      if (m->type == GBK_SCAN_ERROR || m->type == GBK_SCAN_GONE)
        {
          g_hash_table_remove (sv->watches, &w->channel);   /* final: frees `w` */
          return;
        }
    }
  gbk_scanner_rearm (w->scanner);        /* more waiting: come back next round */
}

static void
set_nonblocking (int fd)
{
  int fl = fcntl (fd, F_GETFL);
  if (fl < 0 || fcntl (fd, F_SETFL, fl | O_NONBLOCK) < 0)
    {
      fprintf (stderr, "quoin-serve: can't set up its input and output: %s\n", g_strerror (errno));
      exit (1);
    }
}

static char *
machine_id (void)
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

static int64_t
idle_limit_us (void)
{
  const char *v = g_getenv ("GBK_SERVE_IDLE_MS");
  int64_t ms = IDLE_MS;
  if (v != nullptr && !g_ascii_string_to_signed (v, 10, IDLE_MIN_MS, IDLE_MS, &ms, nullptr))
    ms = IDLE_MS;
  return ms * 1000;
}

static int
serve (void)
{
  const int64_t idle_us = idle_limit_us ();
  int64_t last_input = g_get_monotonic_time ();
  int status = 0;
  signal (SIGPIPE, SIG_IGN);               /* a closed pipe is EPIPE, handled above */
  g_setenv ("GIO_USE_VFS", "local", true); /* no gvfs: there's no session bus here */
  set_nonblocking (STDIN_FILENO);
  set_nonblocking (STDOUT_FILENO);

  Server sv = {
    .in = g_byte_array_new (),
    .out = gbk_outbox_new (),
    .watches
    = g_hash_table_new_full (g_int64_hash, g_int64_equal, nullptr, (GDestroyNotify) watch_free),
    .types = gbk_type_builder_finish (gbk_type_builder_new ()),
  };
  sv.answerer = gbk_answerer_new (false, sv.out);
  sv.asker = gbk_asker_new (true, sv.out);
  sv.jobs = gbk_job_runner_new (sv.asker, sv.out);
  GbkHello hello = {
    .version = GBK_PROTO_VERSION,
    .build = g_strdup (GBK_VERSION),
    .machine_id = machine_id (),
    .uid = (uint32_t) getuid (),
    .home = g_strdup (g_get_home_dir ()),
  };
  GByteArray *first = gbk_outbox_begin (sv.out);
  gbk_proto_put_magic (first, GBK_VERSION);
  gbk_proto_put_hello (first, &hello);
  gbk_outbox_end (sv.out);
  gbk_hello_clear (&hello);

  g_autoptr (GArray) fds = g_array_new (false, false, sizeof (struct pollfd));
  g_autoptr (GArray) polled
      = g_array_new (false, false, sizeof (uint64_t)); /* channels of fds[3..] */
  for (;;)
    {
      g_array_set_size (fds, 0);
      g_array_set_size (polled, 0);
      size_t waiting = gbk_outbox_len (sv.out);
      struct pollfd in = { STDIN_FILENO, POLLIN, 0 };
      struct pollfd out = { STDOUT_FILENO, waiting > 0 ? POLLOUT : 0, 0 };
      struct pollfd woken = { gbk_outbox_wake_fd (sv.out), POLLIN, 0 };
      g_array_append_val (fds, in);
      g_array_append_val (fds, out);
      g_array_append_val (fds, woken);
      if (waiting < OUT_HIGH)
        {
          GHashTableIter it;
          void *w;
          g_hash_table_iter_init (&it, sv.watches);
          while (g_hash_table_iter_next (&it, nullptr, &w))
            {
              struct pollfd p = { gbk_scanner_wake_fd (((Watch *) w)->scanner), POLLIN, 0 };
              g_array_append_val (fds, p);
              g_array_append_val (polled, ((Watch *) w)->channel);
            }
        }
      int64_t left_us = last_input + idle_us - g_get_monotonic_time ();
      if (left_us <= 0)
        {
          fprintf (stderr, "quoin-serve: nothing from the other end for a while; stopping\n");
          status = 4;
          break;
        }
      int wait_ms = (int) MIN ((left_us + 999) / 1000, (int64_t) INT_MAX);
      if (poll ((struct pollfd *) (void *) fds->data, fds->len, wait_ms) < 0)
        {
          if (errno == EINTR)
            continue;
          fprintf (stderr, "quoin-serve: poll: %s\n", g_strerror (errno));
          status = 1;
          break;
        }
      const struct pollfd *p = (const struct pollfd *) (const void *) fds->data;
      if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) && !read_input (&sv, &last_input))
        break;                           /* stdin closed: the laptop is done */
      if (p[1].revents & (POLLERR | POLLHUP))
        break;
      if (p[2].revents & POLLIN)
        gbk_outbox_woken (sv.out);       /* new output: written below */
      /* Draining can end a watch (or the laptop's UNWATCH, read above, already did): look
       * each one up by channel, never through a pointer taken before. */
      for (guint i = 0; i < polled->len; i++)
        {
          if (!(p[3 + i].revents & POLLIN))
            continue;
          uint64_t channel = g_array_index (polled, uint64_t, i);
          Watch *w = g_hash_table_lookup (sv.watches, &channel);
          if (w != nullptr)
            drain_scanner (&sv, w);
        }
      if (!write_output (&sv))
        break;
    }
  /* Jobs stop, removing their temp files (asking the laptop anything fails now); reads and
   * scanners still running finish on their own threads, which hold what they use. */
  g_autoptr (GError) gone
      = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "The laptop went away");
  gbk_asker_dead (sv.asker, gone);
  gbk_answerer_stop (sv.answerer);
  gbk_job_runner_stop (sv.jobs, JOBS_STOP_MS);
  gbk_job_runner_unref (sv.jobs);
  gbk_asker_unref (sv.asker);
  gbk_answerer_unref (sv.answerer);
  g_hash_table_unref (sv.watches);
  gbk_type_table_unref (sv.types);
  g_byte_array_unref (sv.in);
  gbk_outbox_unref (sv.out);
  return status;
}

/* ---- --shell-at ---- */

static int
hex_value (char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

/* Lowercase hex to bytes; nullptr if it isn't, or if it holds a NUL. */
static char *
unhex (const char *hex)
{
  size_t len = strlen (hex);
  if (len == 0 || len % 2 != 0)
    return nullptr;
  g_autofree char *out = g_malloc (len / 2 + 1);
  for (size_t i = 0; i < len / 2; i++)
    {
      int hi = hex_value (hex[2 * i]), lo = hex_value (hex[2 * i + 1]);
      if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
        return nullptr;
      out[i] = (char) (hi * 16 + lo);
    }
  out[len / 2] = '\0';
  return g_steal_pointer (&out);
}

/* $SHELL, else the account's shell, else /bin/sh. g_free. */
static char *
login_shell (void)
{
  const char *env = g_getenv ("SHELL");
  if (env != nullptr && env[0] == '/')
    return g_strdup (env);
  struct passwd pw, *found = nullptr;
  char buf[16384];
  if (getpwuid_r (getuid (), &pw, buf, sizeof buf, &found) == 0 && found != nullptr
      && pw.pw_shell != nullptr && pw.pw_shell[0] == '/')
    return g_strdup (pw.pw_shell);
  return g_strdup ("/bin/sh");
}

static int
shell_at (const char *hex)
{
  g_autofree char *dir = unhex (hex);
  if (dir == nullptr)
    {
      fprintf (stderr, "quoin-serve: --shell-at needs a hex-encoded folder\n");
      return 2;
    }
  if (chdir (dir) != 0)
    fprintf (stderr, "quoin-serve: can't open that folder (%s); starting in your home folder\n",
             g_strerror (errno));
  g_autofree char *shell = login_shell ();
  /* A login shell, by the convention every shell knows: argv[0] starts with '-'. */
  g_autofree char *base = g_path_get_basename (shell);
  g_autofree char *argv0 = g_strconcat ("-", base, nullptr);
  char *const argv[] = { argv0, nullptr };
  execv (shell, argv);
  fprintf (stderr, "quoin-serve: can't start the shell: %s\n", g_strerror (errno));
  return 127;
}

int
main (int argc, char **argv)
{
  if (argc == 2 && g_str_equal (argv[1], "--version"))
    {
      printf ("quoin-serve %s (protocol %d)\n", GBK_VERSION, GBK_PROTO_VERSION);
      return 0;
    }
  if (argc == 3 && g_str_equal (argv[1], "--shell-at"))
    return shell_at (argv[2]);
  if (argc != 1)
    {
      fprintf (stderr, "usage: quoin-serve [--shell-at HEX | --version]\n");
      return 2;
    }
  return serve ();
}
