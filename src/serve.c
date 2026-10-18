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
 * holds up only its own request.
 *
 * The laptop pings every few seconds. Silence for IDLE_MS means it is gone without closing
 * the connection (asleep, or its network changed: TCP may not notice for hours), so this
 * process exits rather than hold its watches open (each holds inotify watches and folders
 * open on this machine). GBK_SERVE_IDLE_MS shortens it, for tests. */
#include "proto.h"

#define READ_CHUNK ((size_t) 64 * 1024)
/* Output waiting for the laptop: above this, scanners aren't drained until it shrinks
 * (their messages wait in the scanners' own queues). */
#define OUT_HIGH ((size_t) 4 * 1024 * 1024)
#define SCAN_MSGS_PER_ROUND 64
#define INFO_MAX 16                      /* file-detail reads in flight */
#define IDLE_MS 30000
#define IDLE_MIN_MS 100

typedef struct
{
  uint64_t channel;
  GbkScanner *scanner;
} Watch;

/* A request run on a worker thread: details, a listing, or a file's contents. The worker
 * hands back encoded frames as it goes (Out), the last one marked `finished`. */
typedef enum
{
  TASK_INFO,
  TASK_LIST,
  TASK_LSTAT,
  TASK_READ,
} TaskKind;

typedef struct
{
  gatomicrefcount rc;     /* the worker's, and the reads table's for a READ */
  TaskKind kind;
  uint64_t channel;
  char *path;
  GAsyncQueue *done;      /* Out*, referenced */
  int done_fd;
  /* READ: how many more bytes the laptop accepts; cancelled by CANCEL or our exit. */
  GMutex lock;
  GCond cond;
  uint64_t credit;
  bool cancelled;
} Task;

typedef struct
{
  uint64_t channel;
  GByteArray *bytes;      /* frames to send */
  bool finished;          /* the task's last */
} Out;

typedef struct
{
  GByteArray *in;
  GByteArray *out;
  GHashTable *watches;   /* &Watch.channel -> Watch*, owned */
  GHashTable *reads;     /* &Task.channel -> Task* (referenced): READs that take credit */
  GbkTypeTable *types;
  GAsyncQueue *done;     /* Out*, from workers */
  int done_fd;           /* eventfd: readable when `done` has something */
  int tasks;             /* in flight (main thread only) */
} Server;

static void
out_free (Out *o)
{
  g_byte_array_unref (o->bytes);
  g_free (o);
}

static void
task_unref (Task *t)
{
  if (!g_atomic_ref_count_dec (&t->rc))
    return;
  g_async_queue_unref (t->done);
  g_free (t->path);
  g_mutex_clear (&t->lock);
  g_cond_clear (&t->cond);
  g_free (t);
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

/* ---- worker threads ---- */

static void
hand_over (Task *t, GByteArray *bytes, bool finished)
{
  Out *o = g_new0 (Out, 1);
  o->channel = t->channel;
  o->bytes = bytes;
  o->finished = finished;
  g_async_queue_push (t->done, o);
  uint64_t one = 1;
  while (write (t->done_fd, &one, sizeof one) < 0 && errno == EINTR)
    ;
}

static void
fail_error (Task *t, const GError *error)
{
  GByteArray *b = g_byte_array_new ();
  gbk_proto_put_failed (b, t->channel, error);
  hand_over (t, b, true);
}

static void
fail_errno (Task *t, int err)
{
  g_autoptr (GError) e
      = g_error_new_literal (G_IO_ERROR, (int) g_io_error_from_errno (err), g_strerror (err));
  GByteArray *b = g_byte_array_new ();
  gbk_proto_put_failed (b, t->channel, e);
  hand_over (t, b, true);
}

static void
run_info (Task *t)
{
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkFileInfo) info = gbk_ops_info (t->path, &error);
  GByteArray *b = g_byte_array_new ();
  if (info != nullptr)
    gbk_proto_put_info (b, t->channel, info);
  else
    gbk_proto_put_failed (b, t->channel, error);
  hand_over (t, b, true);
}

#define LIST_BATCH 1000      /* entries per handed-over batch: bounded memory for huge folders */

static void
run_list (Task *t)
{
  int fd = open (t->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  DIR *d = fd >= 0 ? fdopendir (fd) : nullptr;
  if (d == nullptr)
    {
      int err = errno;
      if (fd >= 0)
        close (fd);
      fail_errno (t, err);
      return;
    }
  int dfd = dirfd (d);
  if (dfd < 0)
    {
      int err = errno;
      closedir (d);
      fail_errno (t, err);
      return;
    }
  g_autoptr (GPtrArray) batch
      = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_stat_entry_free);
  const struct dirent *de;
  g_autoptr (GError) stat_error = nullptr;
  int err = 0;
  errno = 0;
  while ((de = readdir (d)) != nullptr)
    {
      if (g_str_equal (de->d_name, ".") || g_str_equal (de->d_name, ".."))
        continue;
      g_autoptr (GError) e = nullptr;
      GbkStatEntry *entry = gbk_stat_entry_at (dfd, de->d_name, de->d_name, &e);
      if (entry == nullptr)
        {
          /* Gone meanwhile: not part of it any more. Anything else fails the listing. */
          if (!g_error_matches (e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            stat_error = g_steal_pointer (&e);
          if (stat_error != nullptr)
            break;
          continue;
        }
      g_ptr_array_add (batch, entry);
      if (batch->len == LIST_BATCH)
        {
          GByteArray *b = g_byte_array_new ();
          gbk_proto_put_listing (b, t->channel, batch, false);
          hand_over (t, b, false);
          g_ptr_array_set_size (batch, 0);
        }
      errno = 0;
    }
  if (stat_error == nullptr && errno != 0)
    err = errno;                         /* readdir failed */
  closedir (d);
  /* The laptop drops what it has: a listing is whole or nothing. */
  if (stat_error != nullptr)
    {
      fail_error (t, stat_error);
      return;
    }
  if (err != 0)
    {
      fail_errno (t, err);
      return;
    }
  GByteArray *b = g_byte_array_new ();
  gbk_proto_put_listing (b, t->channel, batch, true);
  hand_over (t, b, true);
}

static void
run_lstat (Task *t)
{
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkStatEntry) e = gbk_stat_entry_at (AT_FDCWD, t->path, nullptr, &error);
  if (e == nullptr)
    {
      fail_error (t, error);
      return;
    }
  GByteArray *b = g_byte_array_new ();
  gbk_proto_put_stated (b, t->channel, e);
  hand_over (t, b, true);
}

static bool
same_file_state (const struct stat *a, const struct stat *b)
{
  return a->st_ino == b->st_ino && a->st_size == b->st_size
         && a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec
         && a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

/* A regular file's bytes, no faster than the laptop's credit allows; at the end, whether it
 * changed while being read (then the copy is useless, and the laptop says so). */
static void
run_read (Task *t)
{
  /* O_NONBLOCK: a FIFO in its place mustn't hang the open; a regular file ignores it. */
  int fd = open (t->path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  struct stat before;
  if (fd < 0 || fstat (fd, &before) != 0)
    {
      int err = errno;
      if (fd >= 0)
        close (fd);
      fail_errno (t, err);
      return;
    }
  if (!S_ISREG (before.st_mode))
    {
      close (fd);
      fail_errno (t, EINVAL);
      return;
    }
  g_autofree uint8_t *buf = g_malloc (GBK_PROTO_DATA_MAX);
  for (;;)
    {
      g_mutex_lock (&t->lock);
      while (t->credit == 0 && !t->cancelled)
        g_cond_wait (&t->cond, &t->lock);
      bool stop = t->cancelled;
      size_t take = (size_t) MIN (t->credit, (uint64_t) GBK_PROTO_DATA_MAX);
      g_mutex_unlock (&t->lock);
      if (stop)
        {
          close (fd);
          hand_over (t, g_byte_array_new (), true);   /* nothing more to say */
          return;
        }
      ssize_t n = read (fd, buf, take);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        {
          int err = errno;
          close (fd);
          fail_errno (t, err);
          return;
        }
      if (n == 0)
        break;
      GByteArray *b = g_byte_array_new ();
      gbk_proto_put_data (b, t->channel, buf, (size_t) n);
      g_mutex_lock (&t->lock);
      t->credit -= (uint64_t) n;
      g_mutex_unlock (&t->lock);
      hand_over (t, b, false);
    }
  struct stat after;
  bool changed = fstat (fd, &after) != 0 || !same_file_state (&before, &after);
  close (fd);
  GByteArray *b = g_byte_array_new ();
  gbk_proto_put_end (b, t->channel, changed);
  hand_over (t, b, true);
}

static void *
task_worker (void *data)
{
  Task *t = data;
  switch (t->kind)
    {
    case TASK_INFO: run_info (t); break;
    case TASK_LIST: run_list (t); break;
    case TASK_LSTAT: run_lstat (t); break;
    case TASK_READ: run_read (t); break;
    default: gbk_fatal ("bad task %d", (int) t->kind);
    }
  task_unref (t);
  return nullptr;
}

static void
start_task (Server *sv, TaskKind kind, uint64_t channel, char *path, uint32_t credit)
{
  if (sv->tasks >= INFO_MAX)
    {
      g_free (path);
      g_autoptr (GError) busy
          = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BUSY, "Too many requests at once");
      gbk_proto_put_failed (sv->out, channel, busy);
      return;
    }
  Task *t = g_new0 (Task, 1);
  g_atomic_ref_count_init (&t->rc);
  t->kind = kind;
  t->channel = channel;
  t->path = path;
  t->done = g_async_queue_ref (sv->done);
  t->done_fd = sv->done_fd;
  g_mutex_init (&t->lock);
  g_cond_init (&t->cond);
  t->credit = credit;
  if (kind == TASK_READ)
    {
      if (g_hash_table_contains (sv->reads, &t->channel))
        protocol_error ("channel already in use");
      g_atomic_ref_count_inc (&t->rc);
      g_hash_table_insert (sv->reads, &t->channel, t);
    }
  g_autoptr (GError) error = nullptr;
  GThread *thread = g_thread_try_new ("serve-task", task_worker, t, &error);
  if (thread == nullptr)
    {
      gbk_proto_put_failed (sv->out, channel, error);
      g_hash_table_remove (sv->reads, &channel);
      task_unref (t);
      return;
    }
  g_thread_unref (thread);
  sv->tasks++;
}

static void
drain_tasks (Server *sv)
{
  uint64_t v;
  while (read (sv->done_fd, &v, sizeof v) < 0 && errno == EINTR)
    ;
  Out *o;
  while ((o = g_async_queue_try_pop (sv->done)) != nullptr)
    {
      g_byte_array_append (sv->out, o->bytes->data, o->bytes->len);
      if (o->finished)
        {
          sv->tasks--;
          g_hash_table_remove (sv->reads, &o->channel);
        }
      out_free (o);
    }
}

/* CREDIT and CANCEL for a READ (one already finished: nothing to do). */
static void
steer_read (Server *sv, uint64_t channel, uint32_t credit, bool cancel)
{
  Task *t = g_hash_table_lookup (sv->reads, &channel);
  if (t == nullptr)
    return;
  g_mutex_lock (&t->lock);
  if (cancel)
    t->cancelled = true;
  else
    t->credit = MIN (t->credit + credit, (uint64_t) UINT32_MAX * 4);
  g_cond_signal (&t->cond);
  g_mutex_unlock (&t->lock);
}

static void
cancel_read (void *key G_GNUC_UNUSED, void *value, void *data G_GNUC_UNUSED)
{
  Task *t = value;
  g_mutex_lock (&t->lock);
  t->cancelled = true;
  g_cond_signal (&t->cond);
  g_mutex_unlock (&t->lock);
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
    case GBK_MSG_LIST:
    case GBK_MSG_LSTAT:
      {
        char *path = gbk_proto_get_path (f);
        if (path == nullptr)
          protocol_error ("bad request");
        TaskKind kind = f->type == GBK_MSG_GET_INFO ? TASK_INFO
                        : f->type == GBK_MSG_LIST   ? TASK_LIST
                                                    : TASK_LSTAT;
        start_task (sv, kind, f->channel, path, 0);
        break;
      }
    case GBK_MSG_READ:
      {
        char *path;
        uint32_t credit;
        if (!gbk_proto_get_read (f, &path, &credit) || f->channel == 0)
          protocol_error ("bad read request");
        start_task (sv, TASK_READ, f->channel, path, credit);
        break;
      }
    case GBK_MSG_CREDIT:
      {
        uint32_t bytes;
        if (!gbk_proto_get_credit (f, &bytes))
          protocol_error ("bad credit");
        steer_read (sv, f->channel, bytes, false);
        break;
      }
    case GBK_MSG_CANCEL:
      if (!gbk_proto_get_empty (f))
        protocol_error ("bad cancel");
      steer_read (sv, f->channel, 0, true);
      break;
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
    case GBK_MSG_LISTING:
    case GBK_MSG_STATED:
    case GBK_MSG_DATA:
    case GBK_MSG_END:
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
    .out = g_byte_array_new (),
    .watches
    = g_hash_table_new_full (g_int64_hash, g_int64_equal, nullptr, (GDestroyNotify) watch_free),
    .types = gbk_type_builder_finish (gbk_type_builder_new ()),
    .reads
    = g_hash_table_new_full (g_int64_hash, g_int64_equal, nullptr, (GDestroyNotify) task_unref),
    .done = g_async_queue_new_full ((GDestroyNotify) out_free),
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
          return 1;
        }
      const struct pollfd *p = (const struct pollfd *) (const void *) fds->data;
      if ((p[0].revents & (POLLIN | POLLHUP | POLLERR)) && !read_input (&sv, &last_input))
        break;                           /* stdin closed: the laptop is done */
      if (p[1].revents & (POLLERR | POLLHUP))
        break;
      if (p[2].revents & POLLIN)
        drain_tasks (&sv);
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
  g_hash_table_foreach (sv.reads, cancel_read, nullptr);   /* their workers end now */
  g_hash_table_unref (sv.reads);
  g_async_queue_unref (sv.done);
  g_hash_table_unref (sv.watches);
  gbk_type_table_unref (sv.types);
  g_byte_array_unref (sv.in);
  g_byte_array_unref (sv.out);
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
