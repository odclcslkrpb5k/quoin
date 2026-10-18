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
 * it. Scanners and file-detail reads run on their own threads, so a hung filesystem here
 * holds up only its own request. */
#include "proto.h"

#define READ_CHUNK (64u * 1024u)
/* Output waiting for the laptop: above this, scanners aren't drained until it shrinks
 * (their messages wait in the scanners' own queues). */
#define OUT_HIGH (4u * 1024u * 1024u)
#define SCAN_MSGS_PER_ROUND 64
#define INFO_MAX 16                      /* file-detail reads in flight */

typedef struct
{
  uint64_t channel;
  GbkScanner *scanner;
} Watch;

typedef struct
{
  uint64_t channel;
  char *path;
  GbkFileInfo *info;     /* result, or nullptr with error */
  GError *error;
  GAsyncQueue *done;     /* while on the worker: where to put the result (referenced) */
  int done_fd;
} InfoReq;

typedef struct
{
  GByteArray *in;
  GByteArray *out;
  GHashTable *watches;   /* &Watch.channel -> Watch*, owned */
  GbkTypeTable *types;
  GAsyncQueue *done;     /* InfoReq*, finished */
  int done_fd;           /* eventfd: readable when `done` has results */
  int infos;             /* reads in flight (main thread only) */
} Server;

static void
info_req_free (InfoReq *q)
{
  g_clear_pointer (&q->done, g_async_queue_unref);
  g_free (q->path);
  g_clear_pointer (&q->info, gbk_file_info_free);
  g_clear_error (&q->error);
  g_free (q);
}

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

/* ---- file details, on worker threads ---- */

static void *
info_worker (void *data)
{
  InfoReq *req = data;
  req->info = gbk_ops_info (req->path, &req->error);
  /* Hand the request over and drop our reference: a request waiting in the queue must not
   * hold the queue alive (it would never be freed). */
  GAsyncQueue *done = g_steal_pointer (&req->done);
  int fd = req->done_fd;
  g_async_queue_push (done, req);
  uint64_t one = 1;
  while (write (fd, &one, sizeof one) < 0 && errno == EINTR)
    ;
  g_async_queue_unref (done);
  return nullptr;
}

static void
start_info (Server *sv, uint64_t channel, char *path)
{
  InfoReq *req = g_new0 (InfoReq, 1);
  req->channel = channel;
  req->path = path;
  if (sv->infos >= INFO_MAX)
    {
      g_autoptr (GError) busy
          = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BUSY, "Too many requests at once");
      gbk_proto_put_failed (sv->out, channel, busy);
      info_req_free (req);
      return;
    }
  req->done = g_async_queue_ref (sv->done);
  req->done_fd = sv->done_fd;
  g_autoptr (GError) error = nullptr;
  GThread *t = g_thread_try_new ("serve-info", info_worker, req, &error);
  if (t == nullptr)
    {
      gbk_proto_put_failed (sv->out, channel, error);
      info_req_free (req);
      return;
    }
  g_thread_unref (t);
  sv->infos++;
}

static void
drain_infos (Server *sv)
{
  uint64_t v;
  while (read (sv->done_fd, &v, sizeof v) < 0 && errno == EINTR)
    ;
  InfoReq *req;
  while ((req = g_async_queue_try_pop (sv->done)) != nullptr)
    {
      sv->infos--;
      if (req->info != nullptr)
        gbk_proto_put_info (sv->out, req->channel, req->info);
      else
        gbk_proto_put_failed (sv->out, req->channel, req->error);
      info_req_free (req);
    }
}

/* ---- requests ---- */

static void
handle (Server *sv, const GbkFrame *f)
{
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
    case GBK_MSG_GET_INFO:
      {
        char *path = gbk_proto_get_path (f);
        if (path == nullptr)
          protocol_error ("bad info request");
        start_info (sv, f->channel, path);
        break;
      }
    case GBK_MSG_PING:
      if (!gbk_proto_get_empty (f))
        protocol_error ("bad ping");
      gbk_proto_put_empty (sv->out, GBK_MSG_PONG, f->channel);
      break;
    case GBK_MSG_HELLO:
    case GBK_MSG_SCAN:
    case GBK_MSG_INFO:
    case GBK_MSG_FAILED:
    case GBK_MSG_PONG:
    default: protocol_error ("a message only the server sends");
    }
}

/* Returns false at end of input. */
static bool
read_input (Server *sv)
{
  uint8_t buf[READ_CHUNK];
  ssize_t n = read (STDIN_FILENO, buf, sizeof buf);
  if (n < 0)
    return errno == EAGAIN || errno == EINTR;
  if (n == 0)
    return false;
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

static void
write_output (Server *sv)
{
  ssize_t n = write (STDOUT_FILENO, sv->out->data, sv->out->len);
  if (n < 0)
    {
      if (errno == EAGAIN || errno == EINTR)
        return;
      exit (0);                          /* the laptop went away (EPIPE): nothing to do */
    }
  g_byte_array_remove_range (sv->out, 0, (guint) n);
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
      gbk_proto_put_scan (sv->out, w->channel, m);
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

static int
serve (void)
{
  signal (SIGPIPE, SIG_IGN);               /* a closed pipe is EPIPE, handled above */
  g_setenv ("GIO_USE_VFS", "local", true); /* no gvfs: there's no session bus here */
  set_nonblocking (STDIN_FILENO);
  set_nonblocking (STDOUT_FILENO);

  Server sv = {
    .in = g_byte_array_new (),
    .out = g_byte_array_new (),
    .watches
    = g_hash_table_new_full (g_int64_hash, g_int64_equal, nullptr, (GDestroyNotify) watch_free),
    .types = gbk_type_builder_finish (gbk_type_builder_new ()),
    .done = g_async_queue_new_full ((GDestroyNotify) info_req_free),
    .done_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK),
  };
  if (sv.done_fd < 0)
    {
      fprintf (stderr, "quoin-serve: eventfd: %s\n", g_strerror (errno));
      return 1;
    }
  GbkHello hello = {
    .version = GBK_PROTO_VERSION,
    .build = g_strdup (GBK_VERSION),
    .machine_id = machine_id (),
    .uid = (uint32_t) getuid (),
    .home = g_strdup (g_get_home_dir ()),
  };
  gbk_proto_put_magic (sv.out, GBK_VERSION);
  gbk_proto_put_hello (sv.out, &hello);
  gbk_hello_clear (&hello);

  g_autoptr (GArray) fds = g_array_new (false, false, sizeof (struct pollfd));
  g_autoptr (GArray) polled
      = g_array_new (false, false, sizeof (uint64_t)); /* channels of fds[3..] */
  for (;;)
    {
      g_array_set_size (fds, 0);
      g_array_set_size (polled, 0);
      struct pollfd in = { STDIN_FILENO, POLLIN, 0 };
      struct pollfd out = { STDOUT_FILENO, sv.out->len > 0 ? POLLOUT : 0, 0 };
      struct pollfd done = { sv.done_fd, POLLIN, 0 };
      g_array_append_val (fds, in);
      g_array_append_val (fds, out);
      g_array_append_val (fds, done);
      if (sv.out->len < OUT_HIGH)
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
      if (poll ((struct pollfd *) (void *) fds->data, fds->len, -1) < 0)
        {
          if (errno == EINTR)
            continue;
          fprintf (stderr, "quoin-serve: poll: %s\n", g_strerror (errno));
          return 1;
        }
      const struct pollfd *p = (const struct pollfd *) (const void *) fds->data;
      if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) && !read_input (&sv))
        break;                           /* stdin closed: the laptop is done */
      if (p[1].revents & (POLLERR | POLLHUP))
        break;
      if (p[2].revents & POLLIN)
        drain_infos (&sv);
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
      if (sv.out->len > 0)
        write_output (&sv);
    }
  /* Scanners and detail reads still running finish on their own threads; nothing waits.
   * (Readers in flight hold their own reference to `done`, and still write to done_fd,
   * which therefore stays open until the process ends.) */
  g_async_queue_unref (sv.done);
  g_hash_table_unref (sv.watches);
  gbk_type_table_unref (sv.types);
  g_byte_array_unref (sv.in);
  g_byte_array_unref (sv.out);
  return 0;
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
