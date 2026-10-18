#include "edit-store.h"

#define GROUP "Edit"

void
gbk_edit_record_free (GbkEditRecord *r)
{
  if (r == nullptr)
    return;
  g_free (r->id);
  g_free (r->name);
  g_free (r->host);
  g_free (r->machine_id);
  g_free (r->remote);
  g_free (r->app);
  g_free (r);
}

char *
gbk_edit_record_dir (const char *cache, const GbkEditRecord *r)
{
  return g_build_filename (cache, r->id, nullptr);
}

char *
gbk_edit_record_copy (const char *cache, const GbkEditRecord *r)
{
  return g_build_filename (cache, r->id, r->name, nullptr);
}

static char *
side_file (const char *cache, const char *id, const char *ext)
{
  g_autofree char *name = g_strconcat (id, ext, nullptr);
  return g_build_filename (cache, name, nullptr);
}

/* Bytes (a name or a path: not necessarily UTF-8) as hex, for the key file. */
static char *
hex (const char *bytes)
{
  GString *s = g_string_sized_new (strlen (bytes) * 2);
  for (const char *c = bytes; *c != '\0'; c++)
    g_string_append_printf (s, "%02x", (unsigned) (unsigned char) *c);
  return g_string_free (s, false);
}

static int
hex_digit (char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return -1;
}

/* nullptr unless it is lowercase hex of bytes without NUL. */
static char *
unhex (const char *s)
{
  size_t len = s != nullptr ? strlen (s) : 1;
  if (len % 2 != 0)
    return nullptr;
  g_autofree char *out = g_malloc (len / 2 + 1);
  for (size_t i = 0; i < len / 2; i++)
    {
      int hi = hex_digit (s[2 * i]), lo = hex_digit (s[2 * i + 1]);
      if (hi < 0 || lo < 0 || (hi == 0 && lo == 0))
        return nullptr;
      out[i] = (char) (hi * 16 + lo);
    }
  out[len / 2] = '\0';
  return g_steal_pointer (&out);
}

static void
put_token (GKeyFile *kf, const char *prefix, const GbkFileToken *t)
{
  const struct
  {
    const char *key;
    uint64_t v;
  } u[] = { { "Dev", t->dev }, { "Ino", t->ino }, { "Size", t->size }, { "Nlink", t->nlink } };
  for (size_t i = 0; i < G_N_ELEMENTS (u); i++)
    {
      g_autofree char *k = g_strconcat (prefix, u[i].key, nullptr);
      g_key_file_set_uint64 (kf, GROUP, k, u[i].v);
    }
  g_autofree char *m = g_strconcat (prefix, "MtimeNs", nullptr);
  g_autofree char *c = g_strconcat (prefix, "CtimeNs", nullptr);
  g_key_file_set_int64 (kf, GROUP, m, t->mtime_ns);
  g_key_file_set_int64 (kf, GROUP, c, t->ctime_ns);
}

[[nodiscard]] static bool
get_token (GKeyFile *kf, const char *prefix, GbkFileToken *t, GError **error)
{
  uint64_t *u[] = { &t->dev, &t->ino, &t->size, &t->nlink };
  const char *keys[] = { "Dev", "Ino", "Size", "Nlink" };
  for (size_t i = 0; i < G_N_ELEMENTS (u); i++)
    {
      g_autofree char *k = g_strconcat (prefix, keys[i], nullptr);
      g_autoptr (GError) e = nullptr;
      *u[i] = g_key_file_get_uint64 (kf, GROUP, k, &e);
      if (e != nullptr)
        {
          g_propagate_error (error, g_steal_pointer (&e));
          return false;
        }
    }
  g_autofree char *m = g_strconcat (prefix, "MtimeNs", nullptr);
  g_autofree char *c = g_strconcat (prefix, "CtimeNs", nullptr);
  g_autoptr (GError) e = nullptr;
  t->mtime_ns = g_key_file_get_int64 (kf, GROUP, m, &e);
  if (e == nullptr)
    t->ctime_ns = g_key_file_get_int64 (kf, GROUP, c, &e);
  if (e != nullptr)
    {
      g_propagate_error (error, g_steal_pointer (&e));
      return false;
    }
  return true;
}

bool
gbk_edit_record_save (const char *cache, const GbkEditRecord *r, GError **error)
{
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_autofree char *name = hex (r->name);
  g_autofree char *remote = hex (r->remote);
  g_key_file_set_string (kf, GROUP, "Name", name);
  g_key_file_set_string (kf, GROUP, "Host", r->host);
  g_key_file_set_string (kf, GROUP, "MachineId", r->machine_id);
  g_key_file_set_uint64 (kf, GROUP, "Uid", r->uid);
  g_key_file_set_string (kf, GROUP, "Remote", remote);
  g_key_file_set_string (kf, GROUP, "App", r->app);
  put_token (kf, "Base", &r->base);
  put_token (kf, "Synced", &r->synced);
  gsize len;
  g_autofree char *text = g_key_file_to_data (kf, &len, nullptr);
  g_autofree char *path = side_file (cache, r->id, ".ini");
  return g_file_set_contents_full (path, text, (gssize) len, G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                   error);
}

GbkEditRecord *
gbk_edit_record_load (const char *cache, const char *id, GError **error)
{
  g_autofree char *path = side_file (cache, id, ".ini");
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, error))
    return nullptr;
  g_autoptr (GbkEditRecord) r = g_new0 (GbkEditRecord, 1);
  r->id = g_strdup (id);
  g_autofree char *name = g_key_file_get_string (kf, GROUP, "Name", nullptr);
  g_autofree char *remote = g_key_file_get_string (kf, GROUP, "Remote", nullptr);
  r->name = unhex (name);
  r->remote = unhex (remote);
  r->host = g_key_file_get_string (kf, GROUP, "Host", nullptr);
  r->machine_id = g_key_file_get_string (kf, GROUP, "MachineId", nullptr);
  r->app = g_key_file_get_string (kf, GROUP, "App", nullptr);
  g_autoptr (GError) uid_error = nullptr;
  uint64_t uid = g_key_file_get_uint64 (kf, GROUP, "Uid", &uid_error);
  bool names_ok = r->name != nullptr && strchr (r->name, '/') == nullptr
                  && !g_str_equal (r->name, ".") && !g_str_equal (r->name, "..")
                  && r->remote != nullptr && r->remote[0] == '/';
  if (!names_ok || r->host == nullptr || r->machine_id == nullptr || r->app == nullptr
      || uid_error != nullptr || uid > UINT32_MAX)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "A damaged record: %s", id);
      return nullptr;
    }
  r->uid = (uint32_t) uid;
  if (!get_token (kf, "Base", &r->base, error) || !get_token (kf, "Synced", &r->synced, error))
    return nullptr;
  return g_steal_pointer (&r);
}

bool
gbk_edit_record_dirty (const char *cache, const GbkEditRecord *r, GbkFileToken *now)
{
  g_autofree char *copy = gbk_edit_record_copy (cache, r);
  struct stat st;
  *now = (GbkFileToken) { 0 };
  if (lstat (copy, &st) != 0 || !S_ISREG (st.st_mode))
    return false;
  *now = gbk_file_token_of (&st);
  return !gbk_file_token_same (now, &r->synced);
}

int
gbk_edit_lock (const char *cache, const char *id)
{
  g_autofree char *path = side_file (cache, id, ".lock");
  int fd = open (path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return -1;
  if (flock (fd, LOCK_EX | LOCK_NB) != 0)
    {
      close (fd);
      return -1;
    }
  return fd;
}

static GbkChoice
skip_errors (const char *path G_GNUC_UNUSED, const GError *error G_GNUC_UNUSED,
             void *data G_GNUC_UNUSED)
{
  return GBK_CHOICE_SKIP;   /* what can't go now goes next time */
}

void
gbk_edit_record_remove (const char *cache, const char *id)
{
  g_autoptr (GPtrArray) dir = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (dir, g_build_filename (cache, id, nullptr));
  GbkOpsHooks hooks = { .error = skip_errors };
  g_autoptr (GError) error = nullptr;
  if (!gbk_ops_run (GBK_OP_DELETE, dir, nullptr, &hooks, nullptr, &error))
    g_warning ("removing an edited copy: %s", error->message);
  g_autofree char *ini = side_file (cache, id, ".ini");
  g_autofree char *lock = side_file (cache, id, ".lock");
  unlink (ini);
  unlink (lock);
}
