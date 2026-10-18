#include "entry.h"
#include "natsort.h"

#define NAME_MAX_BYTES 255

static bool
valid_name (const char *name, size_t len)
{
  if (len == 0 || len > NAME_MAX_BYTES)
    return false;
  if (memchr (name, '/', len) != nullptr || memchr (name, '\0', len) != nullptr)
    return false;
  /* "." and ".." name the folder itself and its parent: acting on them as entries (delete
   * "..") would reach outside the folder. */
  if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
    return false;
  return true;
}

GbkEntry *
gbk_entry_new (const char *name, size_t name_len, const GbkEntryInfo *info,
               const GbkTypeTable *types)
{
  if (!valid_name (name, name_len))
    return nullptr;

  /* name is not NUL-terminated by contract; make a terminated copy for the GLib calls. */
  g_autofree char *raw = g_strndup (name, name_len);
  g_autofree char *display = nullptr;
  const char *shown = raw;
  size_t shown_len = name_len;
  if (!g_utf8_validate_len (raw, name_len, nullptr))
    {
      display = g_filename_display_name (raw);
      shown = display;
      shown_len = strlen (display);
    }
  size_t key_len;
  g_autofree char *key = gbk_sort_key_new (shown, shown_len, &key_len);

  /* Layout: name\0 [display\0] key\0 — sizes are small, but check anyway. */
  size_t data_len = name_len + 1;
  if (display != nullptr && ckd_add (&data_len, data_len, shown_len + 1))
    gbk_fatal ("entry size overflow");
  size_t key_off = data_len;
  size_t total;
  if (ckd_add (&data_len, data_len, key_len + 1) || ckd_add (&total, sizeof (GbkEntry), data_len)
      || data_len > UINT32_MAX)
    gbk_fatal ("entry size overflow");

  GbkEntry *e = g_malloc (total);
  memcpy (e->data, raw, name_len + 1);
  e->name_len = (uint32_t) name_len;
  e->display_off = 0;
  if (display != nullptr)
    {
      e->display_off = (uint32_t) (name_len + 1);
      memcpy (e->data + e->display_off, display, shown_len + 1);
    }
  e->key_off = (uint32_t) key_off;
  e->key_len = (uint32_t) key_len;
  memcpy (e->data + key_off, key, key_len + 1);

  e->kind = info->kind;
  e->mtime_ns = info->mtime_ns;
  e->size = info->size;
  e->flags = 0;
  if (name[0] == '.' || name[name_len - 1] == '~')
    e->flags |= GBK_ENTRY_HIDDEN;
  if (info->symlink)
    e->flags |= GBK_ENTRY_SYMLINK;
  if (info->stat_failed)
    e->flags |= GBK_ENTRY_STAT_FAILED;
  bool exec = info->kind == GBK_KIND_REGULAR && (info->mode & 0111) != 0;
  if (exec)
    e->flags |= GBK_ENTRY_EXECUTABLE;
  e->type = gbk_type_table_lookup (types, raw, name_len, info->kind, exec);
  return e;
}

void
gbk_entry_free (GbkEntry *e)
{
  g_free (e);
}

static GbkFileKind
kind_from_mode (uint32_t mode)
{
  switch (mode & S_IFMT)
    {
    case S_IFDIR: return GBK_KIND_DIRECTORY;
    case S_IFCHR: return GBK_KIND_CHARDEV;
    case S_IFBLK: return GBK_KIND_BLOCKDEV;
    case S_IFIFO: return GBK_KIND_FIFO;
    case S_IFSOCK: return GBK_KIND_SOCKET;
    default: return GBK_KIND_REGULAR;
    }
}

static GbkFileKind
kind_from_dtype (unsigned char d_type)
{
  switch (d_type)
    {
    case DT_DIR: return GBK_KIND_DIRECTORY;
    case DT_CHR: return GBK_KIND_CHARDEV;
    case DT_BLK: return GBK_KIND_BLOCKDEV;
    case DT_FIFO: return GBK_KIND_FIFO;
    case DT_SOCK: return GBK_KIND_SOCKET;
    case DT_LNK: return GBK_KIND_BROKEN_LINK;
    default: return GBK_KIND_REGULAR;
    }
}

static int64_t
mtime_ns (const struct statx_timestamp *t)
{
  int64_t ns;
  if (ckd_mul (&ns, (int64_t) t->tv_sec, 1000000000) || ckd_add (&ns, ns, (int64_t) t->tv_nsec))
    return t->tv_sec < 0 ? INT64_MIN : INT64_MAX;   /* absurd timestamps: clamp */
  return ns;
}

/* Never trigger automounts, and don't force a round-trip on network filesystems. */
#define STATX_FLAGS (AT_NO_AUTOMOUNT | AT_STATX_DONT_SYNC)
#define STATX_WANT (STATX_TYPE | STATX_MODE | STATX_MTIME | STATX_SIZE)

GbkEntry *
gbk_entry_new_at (int dirfd, const char *name, unsigned char d_type, const GbkTypeTable *types)
{
  GbkEntryInfo info = {};
  struct statx st;
  if (statx (dirfd, name, STATX_FLAGS | AT_SYMLINK_NOFOLLOW, STATX_WANT, &st) != 0)
    {
      if (errno == ENOENT || errno == ENOTDIR)
        return nullptr;
      info.kind = kind_from_dtype (d_type);
      info.stat_failed = true;
      return gbk_entry_new (name, strlen (name), &info, types);
    }

  if (S_ISLNK (st.stx_mode))
    {
      info.symlink = true;
      struct statx target;
      if (statx (dirfd, name, STATX_FLAGS, STATX_WANT, &target) == 0)
        st = target;
      else
        {
          info.kind = GBK_KIND_BROKEN_LINK;
          info.mtime_ns = mtime_ns (&st.stx_mtime);
          return gbk_entry_new (name, strlen (name), &info, types);
        }
    }

  info.kind = kind_from_mode (st.stx_mode);
  info.mode = st.stx_mode & 07777;
  info.size = st.stx_size;
  info.mtime_ns = mtime_ns (&st.stx_mtime);
  return gbk_entry_new (name, strlen (name), &info, types);
}
