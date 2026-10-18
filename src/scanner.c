#include "scanner.h"
#include "inotify-hub.h"

#define WATCH_MASK \
  (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE | IN_MODIFY \
   | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR)
#define SELF_GONE_MASK (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT | IN_IGNORED)

struct GbkScanner
{
  gatomicrefcount rc;     /* one for the main thread, one for the worker */
  int cancelled;          /* atomic */
  GAsyncQueue *queue;     /* GbkScanMsg*; leftovers freed with the queue */
  int wake_fd;            /* eventfd: worker -> main */
  int cancel_fd;          /* eventfd: main -> worker */
  char *path;
  GbkTypeTable *types;
};

static int live_workers;
/* Workers that haven't exited yet (each holds its folder open until then), for
 * gbk_scanner_busy_under. Entries are borrowed: a worker keeps its own reference. */
static GMutex live_lock;
static GPtrArray *live_scanners;      /* GbkScanner*; under live_lock */
static int pending_max = GBK_SCAN_PENDING_MAX;        /* atomic */
static int rescans;                                   /* atomic */
static int release_delay_ms;                          /* atomic; tests */

void
gbk_scanner_set_pending_max_for_test (unsigned n)
{
  g_atomic_int_set (&pending_max, (int) MIN (n, (unsigned) INT_MAX));
}

void
gbk_scanner_set_release_delay_for_test (unsigned ms)
{
  g_atomic_int_set (&release_delay_ms, (int) MIN (ms, (unsigned) INT_MAX));
}

int
gbk_scanner_rescans (void)
{
  return g_atomic_int_get (&rescans);
}

int
gbk_scanner_live_workers (void)
{
  return g_atomic_int_get (&live_workers);
}

static void
live_add (GbkScanner *s)
{
  g_mutex_lock (&live_lock);
  if (live_scanners == nullptr)
    live_scanners = g_ptr_array_new ();
  g_ptr_array_add (live_scanners, s);
  g_mutex_unlock (&live_lock);
}

static void
live_remove (GbkScanner *s)
{
  g_mutex_lock (&live_lock);
  if (live_scanners == nullptr || !g_ptr_array_remove_fast (live_scanners, s))
    gbk_fatal ("scanner missing from the live list");
  g_mutex_unlock (&live_lock);
}

bool
gbk_path_is_under (const char *path, const char *root)
{
  size_t n = strlen (root);
  if (n > 0 && root[n - 1] == '/')
    n--;                                   /* "/" itself, or a trailing slash */
  return strncmp (path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

bool
gbk_scanner_busy_under (const char *root)
{
  bool busy = false;
  g_mutex_lock (&live_lock);
  for (guint i = 0; live_scanners != nullptr && i < live_scanners->len && !busy; i++)
    busy = gbk_path_is_under (((GbkScanner *) live_scanners->pdata[i])->path, root);
  g_mutex_unlock (&live_lock);
  return busy;
}

void
gbk_scan_msg_free (GbkScanMsg *m)
{
  if (m == nullptr)
    return;
  g_ptr_array_unref (m->upserts);
  g_ptr_array_unref (m->removes);
  g_free (m);
}

GbkScanMsg *
gbk_scan_msg_new (GbkScanMsgType type)
{
  GbkScanMsg *m = g_new0 (GbkScanMsg, 1);
  m->type = type;
  m->upserts = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_entry_free);
  m->removes = g_ptr_array_new_with_free_func (g_free);
  return m;
}

static void
scanner_unref (GbkScanner *s)
{
  if (!g_atomic_ref_count_dec (&s->rc))
    return;
  g_async_queue_unref (s->queue);
  close (s->wake_fd);
  close (s->cancel_fd);
  g_free (s->path);
  gbk_type_table_unref (s->types);
  g_free (s);
}

static bool
is_cancelled (GbkScanner *s)
{
  return g_atomic_int_get (&s->cancelled) != 0;
}

static void
eventfd_signal (int fd)
{
  uint64_t one = 1;
  /* Can only fail if the counter would overflow (it won't: consumers reset it) or with
   * EINTR; either way the fd is already readable or the next signal will make it so. */
  while (write (fd, &one, sizeof one) < 0 && errno == EINTR)
    ;
}

static void
push (GbkScanner *s, GbkScanMsg *m)
{
  if (is_cancelled (s))
    {
      gbk_scan_msg_free (m);
      return;
    }
  g_async_queue_push (s->queue, m);
  eventfd_signal (s->wake_fd);
}

static void
push_error (GbkScanner *s, GbkScanMsgType type, int error)
{
  GbkScanMsg *m = gbk_scan_msg_new (type);
  m->error = error;
  push (s, m);
}

/* Free space, read on the worker: statvfs can block on a dead network mount. */
static void
set_free_space (GbkScanMsg *m, int dirfd)
{
  struct statvfs sv;
  if (fstatvfs (dirfd, &sv) != 0)
    return;
  uint64_t bytes;
  if (ckd_mul (&bytes, (uint64_t) sv.f_bavail, (uint64_t) sv.f_frsize))
    bytes = UINT64_MAX;
  m->free_bytes = bytes;
  m->has_free = true;
}

/* ---- Full scan ---- */

/* Sends the directory as a CHUNK sequence. Returns false if cancelled or unreadable (an
 * ERROR has been sent in the latter case). */
static bool
full_scan (GbkScanner *s, int dirfd, bool live)
{
  /* A fresh open file description, not dup(): a dup shares the directory offset, so a
   * rescan would start reading where the previous scan ended (i.e. see nothing). */
  int fd = openat (dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  DIR *dir = fd >= 0 ? fdopendir (fd) : nullptr;
  if (dir == nullptr)
    {
      int err = errno;
      if (fd >= 0)
        close (fd);
      push_error (s, GBK_SCAN_ERROR, err);
      return false;
    }

  GbkScanMsg *chunk = gbk_scan_msg_new (GBK_SCAN_CHUNK);
  chunk->first = true;
  chunk->live = live;
  size_t limit = GBK_SCAN_FIRST_CHUNK;
  bool ok = true;

  for (;;)
    {
      errno = 0;
      const struct dirent *d = readdir (dir);
      if (d == nullptr)
        {
          if (errno != 0)
            {
              /* Mid-listing failure (e.g. EIO). Send what we have as a complete snapshot
               * rather than nothing; a later event or rescan can fill the rest. */
              g_warning ("readdir: %s", g_strerror (errno));
            }
          break;
        }
      if (is_cancelled (s))
        {
          ok = false;
          break;
        }
      if (g_str_equal (d->d_name, ".") || g_str_equal (d->d_name, ".."))
        continue;

      GbkEntry *e = gbk_entry_new_at (dirfd, d->d_name, d->d_type, s->types);
      if (e == nullptr)
        continue; /* vanished since readdir */
      g_ptr_array_add (chunk->upserts, e);

      if (chunk->upserts->len >= limit)
        {
          push (s, chunk);
          chunk = gbk_scan_msg_new (GBK_SCAN_CHUNK);
          chunk->live = live;
          limit = GBK_SCAN_CHUNK_MAX;
        }
    }
  closedir (dir);

  if (!ok)
    {
      gbk_scan_msg_free (chunk);
      return false;
    }
  chunk->last = true;
  set_free_space (chunk, dirfd);
  push (s, chunk);
  return true;
}

/* ---- Live updates ---- */

typedef struct
{
  GHashTable *names; /* owned raw names whose state must be re-read */
  int self_wd; /* watch on the directory itself */
  int parent_wd; /* watch on its parent (-1 if none), for our own deletion/move */
  const char *basename; /* our name in the parent */
  bool overflow;
  bool gone;
  bool maybe_gone; /* something happened to us that may mean deleted or moved */
} Pending;

static void
on_inotify_event (int wd, uint32_t mask, const char *name, size_t name_len, void *data)
{
  Pending *p = data;
  if (mask & IN_Q_OVERFLOW)
    {
      p->overflow = true;
      p->maybe_gone = true; /* lost events may include our own deletion */
      return;
    }
  if (wd == p->parent_wd)
    {
      /* Only our own name matters here; everything else in the parent is noise. */
      if (name != nullptr && (mask & (IN_DELETE | IN_MOVED_FROM))
          && strlen (p->basename) == name_len && memcmp (p->basename, name, name_len) == 0)
        p->maybe_gone = true;
      return;
    }
  if (wd != p->self_wd)
    return; /* a removed watch's stragglers */
  if (mask & SELF_GONE_MASK)
    p->gone = true;
  if (name == nullptr && (mask & IN_ATTRIB))
    p->maybe_gone = true;
  if (name != nullptr && !p->overflow)
    {
      if (g_hash_table_size (p->names) >= (guint) g_atomic_int_get (&pending_max))
        p->overflow = true; /* a storm: one rescan is cheaper than N statx */
      else
        g_hash_table_add (p->names, g_strndup (name, name_len));
    }
}

/* Takes everything the hub has buffered for us. */
static void
drain_inotify (GbkInotifySub *sub, Pending *p)
{
  gbk_inotify_sub_take (sub, on_inotify_event, p);
}

static void
send_delta (GbkScanner *s, int dirfd, Pending *p)
{
  GbkScanMsg *m = gbk_scan_msg_new (GBK_SCAN_DELTA);
  GHashTableIter it;
  void *key;
  g_hash_table_iter_init (&it, p->names);
  while (g_hash_table_iter_next (&it, &key, nullptr))
    {
      const char *name = key;
      GbkEntry *e = gbk_entry_new_at (dirfd, name, DT_UNKNOWN, s->types);
      if (e != nullptr)
        g_ptr_array_add (m->upserts, e);
      else
        g_ptr_array_add (m->removes, g_strdup (name));
    }
  g_hash_table_remove_all (p->names);
  set_free_space (m, dirfd);
  if (m->upserts->len > 0 || m->removes->len > 0)
    push (s, m);
  else
    gbk_scan_msg_free (m);
}

static int64_t
now_ms (void)
{
  return g_get_monotonic_time () / 1000;
}

/* Waits for inotify activity or cancellation. timeout_ms < 0 waits forever.
 * Returns false if cancelled. */
static bool
wait_events (GbkScanner *s, const GbkInotifySub *sub, int timeout_ms)
{
  struct pollfd fds[2] = {
    { .fd = s->cancel_fd, .events = POLLIN },
    { .fd = gbk_inotify_sub_fd (sub), .events = POLLIN },
  };
  while (poll (fds, 2, timeout_ms) < 0 && errno == EINTR)
    ;
  return !is_cancelled (s);
}

/* Is the directory we hold still at its path? False if deleted (link count 0) or if the
 * path now names something else (moved away, replaced). */
static bool
still_here (const char *path, int dirfd)
{
  struct stat held, at_path;
  if (fstat (dirfd, &held) != 0 || held.st_nlink == 0)
    return false;
  if (stat (path, &at_path) != 0)
    return errno != ENOENT && errno != ENOTDIR; /* other errors: assume still there */
  return held.st_dev == at_path.st_dev && held.st_ino == at_path.st_ino;
}

static void
watch_loop (GbkScanner *s, GbkInotifySub *sub, int self_wd, int parent_wd, int dirfd)
{
  g_autofree char *base = g_path_get_basename (s->path);
  Pending p = {
    .names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nullptr),
    .self_wd = self_wd,
    .parent_wd = parent_wd,
    .basename = base,
  };

  while (!is_cancelled (s))
    {
      if (!wait_events (s, sub, -1))
        break;
      drain_inotify (sub, &p);

      /* Coalesce: keep collecting until things go quiet or the oldest event is too old. */
      int64_t deadline = now_ms () + GBK_SCAN_MAX_DELAY_MS;
      while (!p.gone && !p.overflow)
        {
          int64_t left = deadline - now_ms ();
          if (left <= 0)
            break;
          struct pollfd pf = { .fd = gbk_inotify_sub_fd (sub), .events = POLLIN };
          int r = poll (&pf, 1, (int) MIN (left, GBK_SCAN_QUIET_MS));
          if (r <= 0)
            break; /* quiet (or error): flush now */
          drain_inotify (sub, &p);
        }
      if (is_cancelled (s))
        break;

      /* While anyone holds the directory open (we do), deleting it doesn't free the inode,
       * so IN_DELETE_SELF is deferred and nothing arrives on our own watch. The parent's
       * watch does see our name deleted or moved away: confirm, then report GONE. */
      if (p.maybe_gone)
        {
          p.maybe_gone = false;
          if (!still_here (s->path, dirfd))
            p.gone = true;
        }

      if (p.gone)
        {
          push_error (s, GBK_SCAN_GONE, 0);
          break;
        }
      if (p.overflow)
        {
          /* Drop everything queued so far; the rescan supersedes it. Events arriving after
           * this drain are picked up by the next loop iteration. */
          drain_inotify (sub, &p);
          p.overflow = false;
          g_hash_table_remove_all (p.names);
          if (p.gone)
            {
              push_error (s, GBK_SCAN_GONE, 0);
              break;
            }
          g_atomic_int_inc (&rescans);
          if (!full_scan (s, dirfd, true))
            break;
          continue;
        }
      send_delta (s, dirfd, &p);
    }

  g_hash_table_unref (p.names);
}

static void *
worker (void *data)
{
  GbkScanner *s = data;

  /* Watch before scanning, so nothing created during the scan is missed. */
  GbkInotifySub *sub = gbk_inotify_sub_new ();
  int self_wd = gbk_inotify_sub_watch (sub, s->path, WATCH_MASK);
  bool live = self_wd >= 0;
  /* No watch (e.g. inotify limits exhausted): still list, just not live. */
  int parent_wd = -1;
  if (live && !g_str_equal (s->path, "/"))
    {
      /* Our own deletion/move is only reliably visible from the parent (see watch_loop).
       * Best effort: an unwatchable parent just means we notice later. */
      g_autofree char *parent = g_path_get_dirname (s->path);
      parent_wd = gbk_inotify_sub_watch (sub, parent, IN_DELETE | IN_MOVED_FROM | IN_ONLYDIR);
      if (parent_wd == self_wd)
        parent_wd = -1; /* same inode (bind mounts): don't confuse the two */
    }

  int dirfd = open (s->path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dirfd < 0)
    push_error (s, GBK_SCAN_ERROR, errno);
  else
    {
      if (full_scan (s, dirfd, live) && live)
        watch_loop (s, sub, self_wd, parent_wd, dirfd);
      int delay = g_atomic_int_get (&release_delay_ms);
      if (delay > 0)
        g_usleep ((gulong) delay * 1000); /* a slow device: still holding the folder */
      close (dirfd);
    }

  gbk_inotify_sub_free (sub);
  live_remove (s); /* holds nothing open now */
  scanner_unref (s);
  g_atomic_int_dec_and_test (&live_workers);
  return nullptr;
}

GbkScanner *
gbk_scanner_start (const char *path, GbkTypeTable *types)
{
  GbkScanner *s = g_new0 (GbkScanner, 1);
  g_atomic_ref_count_init (&s->rc);
  s->queue = g_async_queue_new_full ((GDestroyNotify) gbk_scan_msg_free);
  s->path = g_strdup (path);
  s->types = gbk_type_table_ref (types);
  s->wake_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  s->cancel_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (s->wake_fd < 0 || s->cancel_fd < 0)
    gbk_fatal ("eventfd: %s", g_strerror (errno)); /* fd exhaustion: nothing sane to do */

  g_atomic_ref_count_inc (&s->rc); /* the worker's reference */
  g_atomic_int_inc (&live_workers);
  live_add (s);
  g_autoptr (GError) error = nullptr;
  GThread *t = g_thread_try_new ("gbk-scan", worker, s, &error);
  if (t == nullptr)
    {
      live_remove (s);
      g_atomic_int_dec_and_test (&live_workers);
      /* The worker's reference, never used. The caller's is still held, so this can't be
       * the last one: say so, rather than call scanner_unref and use `s` after it. */
      if (g_atomic_ref_count_dec (&s->rc))
        gbk_fatal ("scanner: reference count reached zero while still held");
      push_error (s, GBK_SCAN_ERROR, EAGAIN);
    }
  else
    g_thread_unref (t); /* detached */
  return s;
}

int
gbk_scanner_wake_fd (const GbkScanner *s)
{
  return s->wake_fd;
}

void
gbk_scanner_ack (const GbkScanner *s)
{
  uint64_t v;
  while (read (s->wake_fd, &v, sizeof v) < 0 && errno == EINTR)
    ;
}

GbkScanMsg *
gbk_scanner_pop (GbkScanner *s)
{
  return g_async_queue_try_pop (s->queue);
}

void
gbk_scanner_rearm (const GbkScanner *s)
{
  eventfd_signal (s->wake_fd);
}

void
gbk_scanner_stop (GbkScanner *s)
{
  if (s == nullptr)
    return;
  g_atomic_int_set (&s->cancelled, 1);
  eventfd_signal (s->cancel_fd);
  scanner_unref (s);
}
