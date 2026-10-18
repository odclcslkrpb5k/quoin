#include "fileops.h"

#define COPY_CHUNK (4u << 20)
#define FALLBACK_BUF (1u << 20)
#define TMP_PREFIX ".gbk-tmp-"

typedef struct
{
  const GbkOpsHooks *h;
  bool cancelled;
  bool have_all;          /* "apply to all" was chosen */
  GbkChoice all_choice;
  bool incomplete;        /* something inside the current top-level item was skipped */
  uint64_t done, total;
  GHashTable *links;      /* "dev:ino" -> first copy's path, for sources with nlink > 1 */
  GbkSource *src;         /* where sources are read */
} Ctx;

void
gbk_stat_entry_free (GbkStatEntry *e)
{
  if (e == nullptr)
    return;
  g_free (e->name);
  g_free (e->target);
  g_free (e);
}

GbkStatEntry *
gbk_stat_entry_new (const char *name, const struct stat *st, const char *target)
{
  GbkStatEntry *e = g_new0 (GbkStatEntry, 1);
  e->name = g_strdup (name);
  e->mode = st->st_mode;
  e->uid = st->st_uid;
  e->gid = st->st_gid;
  e->nlink = st->st_nlink;
  e->size = (uint64_t) st->st_size;
  e->dev = st->st_dev;
  e->ino = st->st_ino;
  e->rdev = st->st_rdev;
  e->atime_ns = (int64_t) st->st_atim.tv_sec * 1000000000 + st->st_atim.tv_nsec;
  e->mtime_ns = (int64_t) st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec;
  e->ctime_ns = (int64_t) st->st_ctim.tv_sec * 1000000000 + st->st_ctim.tv_nsec;
  e->target = S_ISLNK (st->st_mode) ? g_strdup (target) : nullptr;
  return e;
}

/* The details as a struct stat: the fields a copy uses. */
static void
entry_stat (const GbkStatEntry *e, struct stat *st)
{
  *st = (struct stat) { 0 };
  st->st_mode = e->mode;
  st->st_uid = e->uid;
  st->st_gid = e->gid;
  st->st_nlink = (nlink_t) e->nlink;
  st->st_size = (off_t) e->size;
  st->st_dev = (dev_t) e->dev;
  st->st_ino = (ino_t) e->ino;
  st->st_rdev = (dev_t) e->rdev;
  st->st_atim = (struct timespec) { e->atime_ns / 1000000000, e->atime_ns % 1000000000 };
  st->st_mtim = (struct timespec) { e->mtime_ns / 1000000000, e->mtime_ns % 1000000000 };
  st->st_ctim = (struct timespec) { e->ctime_ns / 1000000000, e->ctime_ns % 1000000000 };
}

static void
set_errno_gerror (GError **error, int err)
{
  g_set_error_literal (error, G_IO_ERROR, (int) g_io_error_from_errno (err), g_strerror (err));
}

/* ---- this machine as a source ---- */

typedef struct
{
  GbkSourceFile base;
  int fd;
} LocalFile;

GbkStatEntry *
gbk_stat_entry_at (int dirfd, const char *name, const char *entry_name, GError **error)
{
  struct stat st;
  if (fstatat (dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    {
      set_errno_gerror (error, errno);
      return nullptr;
    }
  g_autofree char *target = nullptr;
  if (S_ISLNK (st.st_mode))
    {
      char buf[PATH_MAX];
      ssize_t n = readlinkat (dirfd, name, buf, sizeof buf - 1);
      if (n <= 0)
        {
          set_errno_gerror (error, n < 0 ? errno : EINVAL);
          return nullptr;
        }
      target = g_strndup (buf, (size_t) n);
    }
  return gbk_stat_entry_new (entry_name, &st, target);
}

[[nodiscard]] static GbkStatEntry *
local_lstat (GbkSource *s G_GNUC_UNUSED, const char *path, GError **error)
{
  return gbk_stat_entry_at (AT_FDCWD, path, nullptr, error);
}

/* An entry whose details can't be read is listed without them (mode 0): the copy reads
 * them again, and reports that one item's error. */
[[nodiscard]] static GPtrArray *
local_list (GbkSource *s G_GNUC_UNUSED, const char *path, GError **error)
{
  int fd = open (path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  DIR *d = fd >= 0 ? fdopendir (fd) : nullptr;
  if (d == nullptr)
    {
      int err = errno;
      if (fd >= 0)
        close (fd);
      set_errno_gerror (error, err);
      return nullptr;
    }
  int dfd = dirfd (d);
  if (dfd < 0)
    {
      set_errno_gerror (error, errno);
      closedir (d);
      return nullptr;
    }
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_stat_entry_free);
  const struct dirent *de;
  while ((de = readdir (d)) != nullptr)
    {
      if (g_str_equal (de->d_name, ".") || g_str_equal (de->d_name, ".."))
        continue;
      g_autoptr (GError) e = nullptr;
      GbkStatEntry *entry = gbk_stat_entry_at (dfd, de->d_name, de->d_name, &e);
      if (entry == nullptr && g_error_matches (e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        continue;                        /* gone meanwhile */
      if (entry == nullptr)
        {
          entry = g_new0 (GbkStatEntry, 1);
          entry->name = g_strdup (de->d_name);
        }
      g_ptr_array_add (out, entry);
    }
  closedir (d);
  return out;
}

[[nodiscard]] static GbkSourceFile *
local_open (GbkSource *s, const char *path, GError **error)
{
  int fd = open (path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    {
      set_errno_gerror (error, errno);
      return nullptr;
    }
  LocalFile *f = g_new0 (LocalFile, 1);
  f->base.source = s;
  f->fd = fd;
  return &f->base;
}

[[nodiscard]] static ssize_t
local_read (GbkSourceFile *f, void *buf, size_t len, GError **error)
{
  const LocalFile *lf = (const LocalFile *) (const void *) f;
  for (;;)
    {
      ssize_t n = read (lf->fd, buf, len);
      if (n >= 0)
        return n;
      if (errno != EINTR)
        {
          set_errno_gerror (error, errno);
          return -1;
        }
    }
}

static int
local_fd (const GbkSourceFile *f)
{
  return ((const LocalFile *) (const void *) f)->fd;
}

static void
local_close (GbkSourceFile *f)
{
  LocalFile *lf = (LocalFile *) (void *) f;
  close (lf->fd);
  g_free (lf);
}

static void
local_free (GbkSource *s)
{
  g_free (s);
}

static const GbkSourceOps local_ops = {
  .lstat = local_lstat,
  .list = local_list,
  .open = local_open,
  .read = local_read,
  .fd = local_fd,
  .close = local_close,
  .free = local_free,
};

GbkSource *
gbk_source_local (void)
{
  GbkSource *s = g_new0 (GbkSource, 1);
  s->ops = &local_ops;
  s->local = true;
  return s;
}

void
gbk_source_free (GbkSource *s)
{
  if (s != nullptr)
    s->ops->free (s);
}

/* ---- small helpers ---- */

static bool
cancelled (Ctx *c)
{
  if (!c->cancelled && c->h->cancelled != nullptr && c->h->cancelled (c->h->data))
    c->cancelled = true;
  return c->cancelled;
}

static void
progress (Ctx *c, const char *current)
{
  if (c->h->progress != nullptr)
    c->h->progress (c->done, c->total, current, c->h->data);
}

/* Report errno for `path`; returns true to carry on (skip), false to stop. */
static bool
fail_errno (Ctx *c, const char *path, int err, const char *what)
{
  c->incomplete = true;
  g_autofree char *shown = g_filename_display_name (path);
  g_autoptr (GError) e = g_error_new (G_IO_ERROR, (int) g_io_error_from_errno (err), "%s %s: %s",
                                      what, shown, g_strerror (err));
  GbkChoice ch = c->h->error != nullptr ? c->h->error (path, e, c->h->data) : GBK_CHOICE_CANCEL;
  if (ch == GBK_CHOICE_CANCEL)
    c->cancelled = true;
  return !c->cancelled;
}

/* Report a GError for `path`, as fail_errno does an errno. */
static bool
fail_error (Ctx *c, const char *path, const char *what, const GError *err)
{
  c->incomplete = true;
  g_autofree char *shown = g_filename_display_name (path);
  /* A source that failed without saying why still fails, with a plain word. */
  g_autoptr (GError) e
      = err != nullptr
            ? g_error_new (err->domain, err->code, "%s %s: %s", what, shown, err->message)
            : g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "%s %s", what, shown);
  GbkChoice ch = c->h->error != nullptr ? c->h->error (path, e, c->h->data) : GBK_CHOICE_CANCEL;
  if (ch == GBK_CHOICE_CANCEL)
    c->cancelled = true;
  return !c->cancelled;
}

static bool
fail_msg (Ctx *c, const char *path, GIOErrorEnum code, const char *msg)
{
  c->incomplete = true;
  g_autoptr (GError) e = g_error_new_literal (G_IO_ERROR, (int) code, msg);
  GbkChoice ch = c->h->error != nullptr ? c->h->error (path, e, c->h->data) : GBK_CHOICE_CANCEL;
  if (ch == GBK_CHOICE_CANCEL)
    c->cancelled = true;
  return !c->cancelled;
}

static GbkChoice
ask_conflict (Ctx *c, const char *src, const char *dst, bool dst_is_dir)
{
  if (c->have_all)
    return c->all_choice;
  bool all = false;
  GbkChoice ch = c->h->conflict != nullptr ? c->h->conflict (src, dst, dst_is_dir, &all, c->h->data)
                                           : GBK_CHOICE_SKIP;
  if (ch == GBK_CHOICE_CANCEL)
    c->cancelled = true;
  else if (all)
    {
      c->have_all = true;
      c->all_choice = ch;
    }
  return ch;
}

static bool
same_inode (const struct stat *a, const struct stat *b)
{
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

bool
gbk_ops_valid_name (const char *name, GError **error)
{
  if (name[0] == '\0' || g_str_equal (name, ".") || g_str_equal (name, "..")
      || strchr (name, '/') != nullptr)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                   "\"%s\" is not a valid file name", name);
      return false;
    }
  if (strlen (name) > 255)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG, "Name too long");
      return false;
    }
  return true;
}

char *
gbk_ops_numbered_name (const char *name, unsigned n)
{
  /* Split stem/extension; a leading dot is part of the stem. */
  const char *ext = name[0] != '\0' ? strstr (name + 1, ".tar.") : nullptr;
  if (ext == nullptr && name[0] != '\0')
    ext = strrchr (name + 1, '.');
  if (ext == nullptr)
    ext = name + strlen (name);
  g_autofree char *stem = g_strndup (name, (gsize) (ext - name));
  return g_strdup_printf ("%s (%u)%s", stem, n, ext);
}

char *
gbk_ops_free_name (const char *dir, const char *name)
{
  for (unsigned n = 1; n < 100000; n++)
    {
      g_autofree char *candidate = gbk_ops_numbered_name (name, n);
      g_autofree char *path = g_build_filename (dir, candidate, nullptr);
      struct stat st;
      if (lstat (path, &st) != 0 && errno == ENOENT)
        return g_steal_pointer (&candidate);
    }
  return gbk_ops_numbered_name (name, (unsigned) (g_get_real_time () & 0x7fffffff));
}

/* Total work for progress: bytes of regular files (copy/move) or entries (trash/delete).
 * `known`: its details from the listing, if any. */
static void
measure (Ctx *c, const char *path, const GbkStatEntry *known, bool bytes, int depth)
{
  if (cancelled (c) || depth > GBK_OPS_MAX_DEPTH)
    return;
  g_autoptr (GbkStatEntry) own = nullptr;
  const GbkStatEntry *e = known;
  if (e == nullptr || e->mode == 0)
    {
      own = c->src->ops->lstat (c->src, path, nullptr);
      if (own == nullptr)
        return;
      e = own;
    }
  c->total += bytes ? (S_ISREG (e->mode) ? e->size : 0) : 1;
  if (!S_ISDIR (e->mode))
    return;
  g_autoptr (GPtrArray) kids = c->src->ops->list (c->src, path, nullptr);
  for (guint i = 0; kids != nullptr && i < kids->len; i++)
    {
      const GbkStatEntry *k = g_ptr_array_index (kids, i);
      g_autofree char *child = g_build_filename (path, k->name, nullptr);
      measure (c, child, k, bytes, depth + 1);
    }
}

/* Is `inner` the same as, or inside, `outer`? (After resolving both.) */
static bool
is_within (const char *inner, const char *outer)
{
  g_autofree char *ri = realpath (inner, nullptr);
  g_autofree char *ro = realpath (outer, nullptr);
  if (ri == nullptr || ro == nullptr)
    return false;
  size_t n = strlen (ro);
  return strncmp (ri, ro, n) == 0 && (ri[n] == '\0' || ri[n] == '/' || g_str_equal (ro, "/"));
}

/* ---- copying ---- */

/* ---- keeping metadata, as cp -a does ---- */

/* Owner and group; failing that (not root), the group alone; failing that, nothing. */
static void
keep_owner_fd (int fd, const struct stat *st)
{
  if (fchown (fd, st->st_uid, st->st_gid) != 0)
    (void) !fchown (fd, (uid_t) -1, st->st_gid);
}

/* The same by path, without following a symlink. */
static void
keep_owner_path (const char *path, const struct stat *st)
{
  if (fchownat (AT_FDCWD, path, st->st_uid, st->st_gid, AT_SYMLINK_NOFOLLOW) != 0)
    (void) !fchownat (AT_FDCWD, path, (uid_t) -1, st->st_gid, AT_SYMLINK_NOFOLLOW);
}

/* Every extended attribute (user.*, ACLs, ...) the destination accepts. With fds when
 * given, else by path without following a symlink. */
static void
keep_xattrs (int in, int out, const char *src, const char *dst)
{
  ssize_t n = in >= 0 ? flistxattr (in, nullptr, 0) : llistxattr (src, nullptr, 0);
  if (n <= 0)
    return;
  g_autofree char *names = g_malloc ((size_t) n);
  n = in >= 0 ? flistxattr (in, names, (size_t) n) : llistxattr (src, names, (size_t) n);
  if (n <= 0)
    return;                           /* changed meanwhile, or gone: keep nothing */
  for (const char *name = names; name < names + n; name += strlen (name) + 1)
    {
      ssize_t len = in >= 0 ? fgetxattr (in, name, nullptr, 0) : lgetxattr (src, name, nullptr, 0);
      if (len < 0)
        continue;
      g_autofree char *value = g_malloc ((size_t) len + 1);
      len = in >= 0 ? fgetxattr (in, name, value, (size_t) len)
                    : lgetxattr (src, name, value, (size_t) len);
      if (len < 0)
        continue;
      if (out >= 0)
        (void) !fsetxattr (out, name, value, (size_t) len, 0);
      else
        (void) !lsetxattr (dst, name, value, (size_t) len, 0);
    }
}

/* The permission bits a copy gets: all of them on this machine; from another, not setuid,
 * setgid or sticky (they'd mean something else here). */
static mode_t
kept_mode (const Ctx *c, const struct stat *st)
{
  return st->st_mode & (c->src->local ? 07777 : 0777);
}

/* Owner, attributes, mode, times on a path (folders, links, special files). Order matters:
 * chown clears setuid/setgid, so the mode comes after it. Links have no mode of their own.
 * Owner and attributes only from this machine (see GbkSource). */
static void
keep_metadata (const Ctx *c, const char *src, const char *dst, const struct stat *st)
{
  if (c->src->local)
    {
      keep_owner_path (dst, st);
      keep_xattrs (-1, -1, src, dst);
    }
  if (!S_ISLNK (st->st_mode))
    (void) !fchmodat (AT_FDCWD, dst, kept_mode (c, st), 0);
  const struct timespec times[2] = { st->st_atim, st->st_mtim };
  (void) !utimensat (AT_FDCWD, dst, times, AT_SYMLINK_NOFOLLOW);
}

static char *
inode_key (const struct stat *st)
{
  return g_strdup_printf ("%ju:%ju", (uintmax_t) st->st_dev, (uintmax_t) st->st_ino);
}

/* Copy the bytes of open `in` to open `out`, with cancellation and progress. */
static int
copy_data (Ctx *c, int in, int out, const char *shown)
{
  bool use_cfr = true;
  g_autofree char *buf = nullptr;
  for (;;)
    {
      if (cancelled (c))
        return ECANCELED;
      ssize_t n;
      if (use_cfr)
        {
          n = copy_file_range (in, nullptr, out, nullptr, COPY_CHUNK, 0);
          if (n < 0
              && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP
                  || errno == EBADF))
            {
              use_cfr = false;     /* not supported here: plain read/write */
              continue;
            }
        }
      else
        {
          if (buf == nullptr)
            buf = g_malloc (FALLBACK_BUF);
          n = read (in, buf, FALLBACK_BUF);
          if (n > 0)
            {
              for (ssize_t off = 0; off < n;)
                {
                  ssize_t w = write (out, buf + off, (size_t) (n - off));
                  if (w < 0)
                    {
                      if (errno == EINTR)
                        continue;
                      return errno;
                    }
                  off += w;
                }
            }
        }
      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          return errno;
        }
      if (n == 0)
        return 0;
      c->done += (uint64_t) n;
      progress (c, shown);
    }
}

/* Copy a source file without a descriptor of this machine (another machine's) to `out`.
 * 0, ECANCELED, a write's errno, or -1 with *read_error. */
[[nodiscard]] static int
copy_stream (Ctx *c, GbkSourceFile *f, int out, const char *shown, GError **read_error)
{
  g_autofree char *buf = g_malloc (FALLBACK_BUF);
  for (;;)
    {
      if (cancelled (c))
        return ECANCELED;
      ssize_t n = c->src->ops->read (f, buf, FALLBACK_BUF, read_error);
      if (n < 0)
        return cancelled (c) ? ECANCELED : -1;
      if (n == 0)
        return 0;
      for (ssize_t off = 0; off < n;)
        {
          ssize_t w = write (out, buf + off, (size_t) (n - off));
          if (w < 0)
            {
              if (errno == EINTR)
                continue;
              return errno;
            }
          off += w;
        }
      c->done += (uint64_t) n;
      progress (c, shown);
    }
}

/* Create `dst` as a copy of regular file `src` via a temp file in dst's directory, then an
 * atomic rename. `replace`: dst exists and the user chose to replace it. */
static bool
copy_file (Ctx *c, const char *src, const char *dst, const struct stat *st, bool replace)
{
  g_autoptr (GError) open_error = nullptr;
  GbkSourceFile *f = c->src->ops->open (c->src, src, &open_error);
  if (f == nullptr)
    return fail_error (c, src, "Can't read", open_error);
  int in = c->src->ops->fd (f);

  g_autofree char *dir = g_path_get_dirname (dst);
  g_autofree char *tmp = g_build_filename (dir, TMP_PREFIX "XXXXXX", nullptr);
  int out = g_mkstemp_full (tmp, O_WRONLY | O_CLOEXEC, 0600);
  if (out < 0)
    {
      int err = errno;
      c->src->ops->close (f);
      return fail_errno (c, dst, err, "Can't create");
    }

  g_autofree char *shown = g_filename_display_name (src);
  g_autoptr (GError) read_error = nullptr;
  int err = in >= 0 ? copy_data (c, in, out, shown) : copy_stream (c, f, out, shown, &read_error);
  if (err == 0 && c->src->local)
    {
      keep_owner_fd (out, st);
      keep_xattrs (in, out, nullptr, nullptr);
    }
  if (err == 0 && fchmod (out, kept_mode (c, st)) != 0)
    err = errno;
  const struct timespec times[2] = { st->st_atim, st->st_mtim };
  if (err == 0 && futimens (out, times) != 0)
    err = errno;
  /* Replacing: make the data durable before the old file disappears. */
  if (err == 0 && replace && fsync (out) != 0)
    err = errno;
  if (close (out) != 0 && err == 0)
    err = errno;
  c->src->ops->close (f);

  if (err == 0)
    {
      int r = replace ? rename (tmp, dst)
                      : renameat2 (AT_FDCWD, tmp, AT_FDCWD, dst, RENAME_NOREPLACE);
      if (r != 0)
        err = errno;
    }
  if (err != 0)
    {
      unlink (tmp);
      if (err == ECANCELED)
        return false;
      if (err < 0)
        return fail_error (c, src, "Can't read", read_error);
      return fail_errno (c, dst, err, "Can't write");
    }
  return true;
}

/* `target`: what the link says, from the source's details. */
static bool
copy_symlink (Ctx *c, const char *src, const char *dst, const struct stat *st, const char *target,
              bool replace)
{
  if (target == nullptr)
    return fail_errno (c, src, EIO, "Can't read link");
  if (!replace)
    {
      if (symlink (target, dst) != 0)
        return fail_errno (c, dst, errno, "Can't create link");
      keep_metadata (c, src, dst, st);
      return true;
    }
  g_autofree char *dir = g_path_get_dirname (dst);
  g_autofree char *base = g_strdup_printf (TMP_PREFIX "%" G_GINT64_FORMAT, g_get_real_time ());
  g_autofree char *tmp = g_build_filename (dir, base, nullptr);
  if (symlink (target, tmp) != 0)
    return fail_errno (c, dst, errno, "Can't create link");
  keep_metadata (c, src, tmp, st);
  if (rename (tmp, dst) != 0)
    {
      int err = errno;
      unlink (tmp);
      return fail_errno (c, dst, err, "Can't replace");
    }
  return true;
}

/* FIFOs, sockets, device nodes (devices need root: an error otherwise). */
static bool
copy_special (Ctx *c, const char *src, const char *dst, const struct stat *st, bool replace)
{
  /* Replacing: made under a temporary name beside it, then renamed over it. */
  g_autofree char *tmp = nullptr;
  if (replace)
    {
      g_autofree char *dir = g_path_get_dirname (dst);
      g_autofree char *base = g_strdup_printf (TMP_PREFIX "%" G_GINT64_FORMAT, g_get_real_time ());
      tmp = g_build_filename (dir, base, nullptr);
    }
  const char *make = tmp != nullptr ? tmp : dst;
  if (mknod (make, st->st_mode & (S_IFMT | 0600), st->st_rdev) != 0)
    return fail_errno (c, dst, errno, "Can't create");
  keep_metadata (c, src, make, st);
  if (tmp != nullptr && rename (tmp, dst) != 0)
    {
      int err = errno;
      unlink (tmp);
      return fail_errno (c, dst, err, "Can't replace");
    }
  return true;
}

static bool copy_into (Ctx *c, const char *src, const char *dst_dir, const char *name, int depth,
                       GPtrArray *created, const GbkStatEntry *known);

static bool
copy_dir (Ctx *c, const char *src, const char *dst, const struct stat *st, bool merge, int depth)
{
  g_autoptr (GError) e = nullptr;
  g_autoptr (GPtrArray) entries = c->src->ops->list (c->src, src, &e);
  if (entries == nullptr)
    return fail_error (c, src, "Can't read folder", e);
  if (!merge && mkdir (dst, 0700) != 0)     /* owner-only while we fill it */
    return fail_errno (c, dst, errno, "Can't create folder");
  for (guint i = 0; i < entries->len; i++)
    {
      const GbkStatEntry *entry = g_ptr_array_index (entries, i);
      g_autofree char *child = g_build_filename (src, entry->name, nullptr);
      if (!copy_into (c, child, dst, entry->name, depth + 1, nullptr, entry))
        return false;
    }
  if (!merge)
    keep_metadata (c, src, dst, st);    /* last: filling it changed its times */
  return true;
}

/* Copy `src` to dst_dir/name, resolving conflicts. Returns false to stop everything.
 * `known`: its details from the folder's listing, if any. */
static bool
copy_into (Ctx *c, const char *src, const char *dst_dir, const char *name, int depth,
           GPtrArray *created, const GbkStatEntry *known)
{
  if (cancelled (c))
    return false;
  if (depth > GBK_OPS_MAX_DEPTH)
    return fail_msg (c, src, G_IO_ERROR_FAILED, "Folder structure is too deep");

  g_autoptr (GbkStatEntry) own = nullptr;
  const GbkStatEntry *entry = known;
  if (entry == nullptr || entry->mode == 0)
    {
      g_autoptr (GError) e = nullptr;
      own = c->src->ops->lstat (c->src, src, &e);
      if (own == nullptr)
        return fail_error (c, src, "Can't read", e);
      entry = own;
    }
  struct stat st, dst_st;
  entry_stat (entry, &st);

  g_autofree char *final_name = g_strdup (name);
  g_autofree char *dst = g_build_filename (dst_dir, name, nullptr);
  bool replace = false;
  if (lstat (dst, &dst_st) == 0)
    {
      GbkChoice ch = c->src->local && same_inode (&st, &dst_st)
                         ? GBK_CHOICE_KEEP_BOTH       /* pasted where it came from: a copy */
                         : ask_conflict (c, src, dst, S_ISDIR (dst_st.st_mode));
      switch (ch)
        {
        case GBK_CHOICE_CANCEL: return false;
        case GBK_CHOICE_SKIP: c->incomplete = true; return true;
        case GBK_CHOICE_KEEP_BOTH:
          g_free (final_name);
          final_name = gbk_ops_free_name (dst_dir, name);
          g_free (dst);
          dst = g_build_filename (dst_dir, final_name, nullptr);
          break;
        case GBK_CHOICE_REPLACE:
          if (S_ISDIR (st.st_mode) != S_ISDIR (dst_st.st_mode))
            return fail_msg (c, dst, G_IO_ERROR_EXISTS,
                             S_ISDIR (st.st_mode) ? "A file is in the way of this folder"
                                                  : "A folder is in the way of this file");
          replace = true;
          break;
        default: gbk_fatal ("bad choice %d", (int) ch);
        }
    }

  /* Hard links among what's being copied stay hard links (the copies share one inode). */
  g_autofree char *key = !S_ISDIR (st.st_mode) && st.st_nlink > 1 ? inode_key (&st) : nullptr;
  const char *first_copy = key != nullptr ? g_hash_table_lookup (c->links, key) : nullptr;
  bool ok;
  if (first_copy != nullptr && !replace && link (first_copy, dst) == 0)
    {
      ok = true;
      c->done += S_ISREG (st.st_mode) ? (uint64_t) st.st_size : 0;   /* counted by measure() */
    }
  else if (S_ISDIR (st.st_mode))
    ok = copy_dir (c, src, dst, &st, replace, depth);
  else if (S_ISLNK (st.st_mode))
    ok = copy_symlink (c, src, dst, &st, entry->target, replace);
  else if (S_ISREG (st.st_mode))
    ok = copy_file (c, src, dst, &st, replace);
  else
    ok = copy_special (c, src, dst, &st, replace);
  if (ok && key != nullptr && first_copy == nullptr)
    g_hash_table_insert (c->links, g_steal_pointer (&key), g_strdup (dst));

  if (ok && !cancelled (c) && created != nullptr)
    g_ptr_array_add (created, g_strdup (final_name));
  return ok && !cancelled (c);
}

/* ---- deleting ---- */

static bool
delete_at (Ctx *c, int parent, const char *name, const char *path, int depth)
{
  if (cancelled (c))
    return false;
  if (depth > GBK_OPS_MAX_DEPTH)
    return fail_msg (c, path, G_IO_ERROR_FAILED, "Folder structure is too deep");
  struct stat st;
  if (fstatat (parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    return errno == ENOENT ? true : fail_errno (c, path, errno, "Can't delete");

  if (S_ISDIR (st.st_mode))
    {
      int fd = openat (parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      DIR *d = fd >= 0 ? fdopendir (fd) : nullptr;
      if (d == nullptr)
        {
          int err = errno;
          if (fd >= 0)
            close (fd);
          return fail_errno (c, path, err, "Can't delete");
        }
      const struct dirent *de;
      bool ok = true;
      while (ok && (de = readdir (d)) != nullptr)
        {
          if (g_str_equal (de->d_name, ".") || g_str_equal (de->d_name, ".."))
            continue;
          g_autofree char *child = g_build_filename (path, de->d_name, nullptr);
          ok = delete_at (c, fd, de->d_name, child, depth + 1);   /* d's own fd */
        }
      closedir (d);
      if (!ok)
        return false;
      if (unlinkat (parent, name, AT_REMOVEDIR) != 0)
        return fail_errno (c, path, errno, "Can't delete");
    }
  else if (unlinkat (parent, name, 0) != 0)
    return fail_errno (c, path, errno, "Can't delete");

  c->done++;
  progress (c, path);
  return true;
}

static bool
delete_path (Ctx *c, const char *path)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *base = g_path_get_basename (path);
  int parent = open (dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent < 0)
    return fail_errno (c, path, errno, "Can't delete");
  bool ok = delete_at (c, parent, base, path, 0);
  close (parent);
  return ok;
}

/* ---- moving ---- */

static int force_exdev;     /* test hooks (atomic: jobs run on worker threads) */
static int flush_error;
static int flushes;

void
gbk_ops_force_exdev_for_test (bool force)
{
  g_atomic_int_set (&force_exdev, force ? 1 : 0);
}

void
gbk_ops_fail_flush_for_test (int err)
{
  g_atomic_int_set (&flush_error, err);
}

int
gbk_ops_flushes (void)
{
  return g_atomic_int_get (&flushes);
}

/* rename(2) / renameat2(2), or EXDEV when a test forces the copy path. */
static int
move_rename (const char *src, const char *dst, bool replace)
{
  if (g_atomic_int_get (&force_exdev))
    {
      errno = EXDEV;
      return -1;
    }
  return replace ? rename (src, dst) : renameat2 (AT_FDCWD, src, AT_FDCWD, dst, RENAME_NOREPLACE);
}

/* A copy made by a move must be on disk before the source is deleted: otherwise a crash or
 * power cut right after the move loses the file in both places. syncfs covers the data,
 * the metadata and the directory entries of everything just copied, in one call. Returns
 * true if the source may now be deleted; on failure the error is reported and the source
 * is kept. */
static bool
make_durable (Ctx *c, const char *dst_dir)
{
  int err = g_atomic_int_get (&flush_error);
  if (err == 0)
    {
      int fd = open (dst_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (fd < 0 || syncfs (fd) != 0)
        err = errno;
      if (fd >= 0)
        close (fd);
    }
  g_atomic_int_inc (&flushes);
  if (err == 0)
    return true;
  fail_errno (c, dst_dir, err, "The original was kept: the copy couldn't be saved to disk in");
  return false;
}

static bool move_into (Ctx *c, const char *src, const char *dst_dir, const char *name, int depth,
                       GPtrArray *created);

/* Different filesystem: copy, and delete the source only if the copy was complete. */
static bool
move_by_copy (Ctx *c, const char *src, const char *dst_dir, const char *name, int depth)
{
  bool was_incomplete = c->incomplete;
  c->incomplete = false;
  bool ok = copy_into (c, src, dst_dir, name, depth, nullptr, nullptr);
  bool complete = ok && !c->incomplete;
  c->incomplete = c->incomplete || was_incomplete;
  if (!complete)
    return ok;
  if (!make_durable (c, dst_dir))
    return !c->cancelled;
  return delete_path (c, src);
}

static bool
move_into (Ctx *c, const char *src, const char *dst_dir, const char *name, int depth,
           GPtrArray *created)
{
  if (cancelled (c))
    return false;
  if (depth > GBK_OPS_MAX_DEPTH)
    return fail_msg (c, src, G_IO_ERROR_FAILED, "Folder structure is too deep");
  struct stat st, dst_st;
  if (lstat (src, &st) != 0)
    return fail_errno (c, src, errno, "Can't move");

  g_autofree char *final_name = g_strdup (name);
  g_autofree char *dst = g_build_filename (dst_dir, name, nullptr);
  if (lstat (dst, &dst_st) == 0)
    {
      if (same_inode (&st, &dst_st))
        return true;                          /* already there */
      GbkChoice ch = ask_conflict (c, src, dst, S_ISDIR (dst_st.st_mode));
      switch (ch)
        {
        case GBK_CHOICE_CANCEL: return false;
        case GBK_CHOICE_SKIP: c->incomplete = true; return true;
        case GBK_CHOICE_KEEP_BOTH:
          g_free (final_name);
          final_name = gbk_ops_free_name (dst_dir, name);
          g_free (dst);
          dst = g_build_filename (dst_dir, final_name, nullptr);
          break;
        case GBK_CHOICE_REPLACE:
          if (S_ISDIR (st.st_mode) != S_ISDIR (dst_st.st_mode))
            return fail_msg (c, dst, G_IO_ERROR_EXISTS,
                             S_ISDIR (st.st_mode) ? "A file is in the way of this folder"
                                                  : "A folder is in the way of this file");
          if (S_ISDIR (st.st_mode))
            {
              /* Merge: move each child in, then remove the source if it ended up empty. */
              DIR *d = opendir (src);
              if (d == nullptr)
                return fail_errno (c, src, errno, "Can't read folder");
              bool ok = true;
              const struct dirent *de;
              while (ok && (de = readdir (d)) != nullptr)
                {
                  if (g_str_equal (de->d_name, ".") || g_str_equal (de->d_name, ".."))
                    continue;
                  g_autofree char *cs = g_build_filename (src, de->d_name, nullptr);
                  ok = move_into (c, cs, dst, de->d_name, depth + 1, nullptr);
                }
              closedir (d);
              if (!ok)
                return false;
              rmdir (src); /* fails harmlessly if something was skipped */
              if (created != nullptr)
                g_ptr_array_add (created, g_strdup (final_name));
              return true;
            }
          /* File over file: rename(2) replaces atomically. */
          if (move_rename (src, dst, true) == 0)
            {
              c->done += (uint64_t) st.st_size;
              if (created != nullptr)
                g_ptr_array_add (created, g_strdup (final_name));
              return true;
            }
          if (errno != EXDEV)
            return fail_errno (c, src, errno, "Can't move");
          if (!copy_file (c, src, dst, &st, true))
            return false;
          if (created != nullptr)
            g_ptr_array_add (created, g_strdup (final_name));
          if (!make_durable (c, dst_dir))
            return !c->cancelled;
          return delete_path (c, src);
        default: gbk_fatal ("bad choice %d", (int) ch);
        }
    }

  if (move_rename (src, dst, false) == 0)
    {
      c->done += S_ISREG (st.st_mode) ? (uint64_t) st.st_size : 0;
      progress (c, src);
    }
  else if (errno == EXDEV)
    {
      if (!move_by_copy (c, src, dst_dir, final_name, depth))
        return false;
    }
  else
    return fail_errno (c, src, errno, "Can't move");

  if (created != nullptr)
    g_ptr_array_add (created, g_strdup (final_name));
  return !cancelled (c);
}

/* ---- trash ---- */

static bool
trash_path (Ctx *c, const char *path)
{
  g_autoptr (GFile) f = g_file_new_for_path (path);
  g_autoptr (GError) e = nullptr;
  if (g_file_trash (f, nullptr, &e))
    {
      c->done++;
      progress (c, path);
      return true;
    }
  if (g_error_matches (e, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      if (c->h->delete_untrashable != nullptr && c->h->delete_untrashable (path, c->h->data))
        return delete_path (c, path);
      c->incomplete = true;
      return !cancelled (c);
    }
  c->incomplete = true;
  GbkChoice ch = c->h->error != nullptr ? c->h->error (path, e, c->h->data) : GBK_CHOICE_CANCEL;
  if (ch == GBK_CHOICE_CANCEL)
    c->cancelled = true;
  return !c->cancelled;
}

/* ---- entry points ---- */

bool
gbk_ops_run (GbkOpKind kind, GPtrArray *sources, const char *dest_dir, const GbkOpsHooks *hooks,
             GPtrArray *created, GError **error)
{
  g_autoptr (GbkSource) here = gbk_source_local ();
  return gbk_ops_run_from (here, kind, sources, dest_dir, hooks, created, error);
}

bool
gbk_ops_run_from (GbkSource *src, GbkOpKind kind, GPtrArray *sources, const char *dest_dir,
                  const GbkOpsHooks *hooks, GPtrArray *created, GError **error)
{
  if (!src->local && kind != GBK_OP_COPY)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Only copying from another machine is possible yet");
      return false;
    }
  src->cancelled = hooks->cancelled;
  src->cancel_data = hooks->data;
  g_autoptr (GHashTable) links = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  Ctx c = { .h = hooks, .links = links, .src = src };
  bool bytes = kind == GBK_OP_COPY || kind == GBK_OP_MOVE;

  if (bytes)
    {
      struct stat st;
      if (stat (dest_dir, &st) != 0 || !S_ISDIR (st.st_mode))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, "Destination is not a folder");
          return false;
        }
      /* A folder into itself: only possible on one machine. */
      for (guint i = 0; i < sources->len && src->local; i++)
        {
          const char *path = g_ptr_array_index (sources, i);
          struct stat sst;
          if (lstat (path, &sst) == 0 && S_ISDIR (sst.st_mode) && is_within (dest_dir, path))
            {
              g_autofree char *shown = g_filename_display_name (path);
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Can't put \"%s\" inside itself", shown);
              return false;
            }
        }
    }

  for (guint i = 0; i < sources->len; i++)
    measure (&c, g_ptr_array_index (sources, i), nullptr, bytes, 0);
  progress (&c, "");

  for (guint i = 0; i < sources->len && !cancelled (&c); i++)
    {
      const char *path = g_ptr_array_index (sources, i);
      g_autofree char *name = g_path_get_basename (path);
      c.incomplete = false;
      switch (kind)
        {
        case GBK_OP_COPY: copy_into (&c, path, dest_dir, name, 0, created, nullptr); break;
        case GBK_OP_MOVE: move_into (&c, path, dest_dir, name, 0, created); break;
        case GBK_OP_TRASH: trash_path (&c, path); break;
        case GBK_OP_DELETE: delete_path (&c, path); break;
        default: gbk_fatal ("bad op %d", (int) kind);
        }
    }

  if (c.cancelled)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return false;
    }
  return true;
}

bool
gbk_ops_rename (const char *path, const char *new_name, GError **error)
{
  if (!gbk_ops_valid_name (new_name, error))
    return false;
  g_autofree char *dir = g_path_get_dirname (path);
  g_autofree char *dst = g_build_filename (dir, new_name, nullptr);
  if (g_str_equal (dst, path))
    return true;

  struct stat a, b;
  bool same_file = lstat (path, &a) == 0 && lstat (dst, &b) == 0 && same_inode (&a, &b);
  /* Same file under another spelling (case change on a case-insensitive filesystem): a
   * plain rename is safe. Otherwise never replace. */
  int r = same_file ? rename (path, dst)
                    : renameat2 (AT_FDCWD, path, AT_FDCWD, dst, RENAME_NOREPLACE);
  if (r != 0)
    {
      int err = errno;
      g_autofree char *shown = g_filename_display_name (new_name);
      if (err == EEXIST)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "\"%s\" already exists", shown);
      else
        g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (err),
                     "Can't rename to \"%s\": %s", shown, g_strerror (err));
      return false;
    }
  return true;
}

/* ---- create, permissions, info ---- */

static void
set_errno_error (GError **error, int err, const char *what, const char *path)
{
  g_autofree char *shown = g_filename_display_name (path);
  g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (err), "%s \"%s\": %s", what, shown,
               g_strerror (err));
}

bool
gbk_ops_create (const char *dir, const char *name, bool folder, GError **error)
{
  if (!gbk_ops_valid_name (name, error))
    return false;
  int dirfd = open (dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dirfd < 0)
    {
      set_errno_error (error, errno, "Can't open", dir);
      return false;
    }
  int r;
  if (folder)
    r = mkdirat (dirfd, name, 0777);
  else
    {
      r = openat (dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
      if (r >= 0)
        r = close (r) == 0 || errno == EINTR ? 0 : -1;
    }
  int err = errno;
  close (dirfd);
  if (r != 0)
    {
      g_autofree char *shown = g_filename_display_name (name);
      if (err == EEXIST)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "\"%s\" already exists", shown);
      else
        g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (err),
                     "Can't create \"%s\": %s", shown, g_strerror (err));
      return false;
    }
  return true;
}

/* chmod `path` (following a symlink) to `make(old mode)`, if that changes anything. */
static bool
change_mode (const char *path, bool regular_only, mode_t (*make) (mode_t old, uint32_t arg),
             uint32_t arg, GError **error)
{
  struct stat st;
  if (stat (path, &st) != 0)
    {
      set_errno_error (error, errno, "Can't read", path);
      return false;
    }
  if (regular_only && !S_ISREG (st.st_mode))
    {
      g_autofree char *shown = g_filename_display_name (path);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE, "\"%s\" is not a regular file",
                   shown);
      return false;
    }
  mode_t want = make (st.st_mode & 07777, arg);
  if (want == (st.st_mode & 07777))
    return true;
  if (chmod (path, want) != 0)
    {
      set_errno_error (error, errno, "Can't change permissions of", path);
      return false;
    }
  return true;
}

static mode_t
exec_mode (mode_t old, uint32_t on)
{
  return on ? old | ((old & 0444) >> 2) : old & (mode_t) ~0111;
}

static mode_t
rwx_mode (mode_t old, uint32_t rwx)
{
  return (old & 07000) | (rwx & 0777);
}

bool
gbk_ops_set_executable (const char *path, bool executable, GError **error)
{
  return change_mode (path, true, exec_mode, executable, error);
}

bool
gbk_ops_set_permissions (const char *path, uint32_t rwx, GError **error)
{
  return change_mode (path, false, rwx_mode, rwx, error);
}

static int64_t
ts_ns (const struct statx_timestamp *t)
{
  return t->tv_sec * INT64_C (1000000000) + t->tv_nsec;
}

/* User and group names (NSS: may consult the network, so worker only). */
static char *
user_name (uid_t uid)
{
  char buf[4096];
  struct passwd pw, *res = nullptr;
  return getpwuid_r (uid, &pw, buf, sizeof buf, &res) == 0 && res != nullptr
             ? g_strdup (res->pw_name)
             : nullptr;
}

static char *
group_name (gid_t gid)
{
  char buf[4096];
  struct group gr, *res = nullptr;
  return getgrgid_r (gid, &gr, buf, sizeof buf, &res) == 0 && res != nullptr
             ? g_strdup (res->gr_name)
             : nullptr;
}

GbkFileInfo *
gbk_ops_info (const char *path, GError **error)
{
  const unsigned want = STATX_BASIC_STATS | STATX_BTIME;
  struct statx st;
  if (statx (AT_FDCWD, path, AT_NO_AUTOMOUNT | AT_SYMLINK_NOFOLLOW, want, &st) != 0)
    {
      set_errno_error (error, errno, "Can't read", path);
      return nullptr;
    }
  GbkFileInfo *info = g_new0 (GbkFileInfo, 1);
  if (S_ISLNK (st.stx_mode))
    {
      info->symlink = true;
      info->link_target = g_file_read_link (path, nullptr);
      struct statx target;
      if (statx (AT_FDCWD, path, AT_NO_AUTOMOUNT, want, &target) == 0)
        st = target;
      else
        info->dangling = true;
    }
  info->mode = st.stx_mode;
  info->size = st.stx_size;
  info->uid = st.stx_uid;
  info->gid = st.stx_gid;
  info->owner = user_name (st.stx_uid);
  info->group = group_name (st.stx_gid);
  info->atime_ns = ts_ns (&st.stx_atime);
  info->mtime_ns = ts_ns (&st.stx_mtime);
  info->ctime_ns = ts_ns (&st.stx_ctime);
  info->has_btime = (st.stx_mask & STATX_BTIME) != 0;
  info->btime_ns = info->has_btime ? ts_ns (&st.stx_btime) : 0;
  return info;
}

void
gbk_file_info_free (GbkFileInfo *info)
{
  if (info == nullptr)
    return;
  g_free (info->link_target);
  g_free (info->owner);
  g_free (info->group);
  g_free (info);
}
