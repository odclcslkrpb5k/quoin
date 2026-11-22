#include "appimage.h"

#define ELF_HEADER 64u

void
gbk_appimage_clear (GbkAppImage *a)
{
  g_free (a->name);
  g_free (a->version);
  if (a->icon != nullptr)
    g_bytes_unref (a->icon);
  *a = (GbkAppImage) { 0 };
}

static uint64_t
le (const uint8_t *p, unsigned bytes)
{
  uint64_t v = 0;
  for (unsigned i = 0; i < bytes; i++)
    v |= (uint64_t) p[i] << (8 * i);
  return v;
}

bool
gbk_appimage_offset (GbkReadAt read, void *ctx, uint64_t size, uint64_t *offset)
{
  uint8_t h[ELF_HEADER];
  if (size < sizeof h || !read (ctx, 0, h, sizeof h))
    return false;
  /* ELF, little-endian, and the AppImage type 2 mark in the padding of e_ident. */
  if (memcmp (h,
              "\x7f"
              "ELF",
              4)
          != 0
      || h[5] != 1 || h[8] != 'A' || h[9] != 'I' || h[10] != 2)
    return false;
  uint64_t shoff, shentsize, shnum;
  if (h[4] == 2)                      /* 64-bit */
    {
      shoff = le (h + 0x28, 8);
      shentsize = le (h + 0x3A, 2);
      shnum = le (h + 0x3C, 2);
    }
  else if (h[4] == 1)                 /* 32-bit */
    {
      shoff = le (h + 0x20, 4);
      shentsize = le (h + 0x2E, 2);
      shnum = le (h + 0x30, 2);
    }
  else
    return false;
  /* The image follows the section headers, which end the program. */
  if (shoff > size || shentsize * shnum > size - shoff)
    return false;
  *offset = shoff + shentsize * shnum;
  return *offset < size;
}

/* Text for showing: valid UTF-8, one line, not too long; nullptr if empty. */
static char *
clean (const char *raw)
{
  if (raw == nullptr)
    return nullptr;
  g_autofree char *valid = g_utf8_make_valid (raw, -1);
  for (char *c = valid; *c != '\0'; c++)
    if ((unsigned char) *c < 0x20)
      *c = ' ';
  g_strstrip (valid);
  if (valid[0] == '\0')
    return nullptr;
  if (g_utf8_strlen (valid, -1) > GBK_APPIMAGE_TEXT_MAX)
    *g_utf8_offset_to_pointer (valid, GBK_APPIMAGE_TEXT_MAX) = '\0';
  return g_steal_pointer (&valid);
}

/* The newest release's version in AppStream metadata: the first <release version="...">
 * in <component><releases> (the specification has them newest first). */
typedef struct
{
  int depth;
  bool in_releases;  /* inside <component><releases> */
  char *version;
} Releases;

static void
release_start (GMarkupParseContext *, const char *element, const char **names, const char **values,
               void *data, GError **)
{
  Releases *r = data;
  r->depth++;
  if (r->depth == 2 && strcmp (element, "releases") == 0)
    r->in_releases = true;
  else if (r->depth == 3 && r->in_releases && r->version == nullptr
           && strcmp (element, "release") == 0)
    for (size_t i = 0; names[i] != nullptr; i++)
      if (strcmp (names[i], "version") == 0)
        r->version = g_strdup (values[i]);
}

static void
release_end (GMarkupParseContext *, const char *, void *data, GError **)
{
  Releases *r = data;
  if (r->depth == 2)
    r->in_releases = false;
  r->depth--;
}

static char *
metainfo_version (GbkSquash *s, const char *id)
{
  const char *const where[][2] = { { "usr/share/metainfo/", ".metainfo.xml" },
                                   { "usr/share/metainfo/", ".appdata.xml" },
                                   { "usr/share/appdata/", ".appdata.xml" } };
  for (size_t i = 0; i < G_N_ELEMENTS (where); i++)
    {
      g_autofree char *path = g_strconcat (where[i][0], id, where[i][1], nullptr);
      g_autoptr (GError) e = nullptr;
      g_autoptr (GBytes) xml = gbk_squash_read (s, path, GBK_APPIMAGE_METAINFO_MAX, &e);
      if (xml == nullptr)
        continue;
      static const GMarkupParser parser
          = { .start_element = release_start, .end_element = release_end };
      Releases r = { 0 };
      size_t len;
      const char *data = g_bytes_get_data (xml, &len);
      GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &r, nullptr);
      /* Whatever was found before any error in it still counts. */
      bool parsed = len > 0 && g_markup_parse_context_parse (ctx, data, (gssize) len, nullptr);
      g_markup_parse_context_free (ctx);
      g_autofree char *version = g_steal_pointer (&r.version);
      if (parsed || version != nullptr)
        return clean (version);
    }
  return nullptr;
}

/* An icon file at the top named for the .desktop file's Icon key ("app" -> app.png). */
static GBytes *
icon_named (GbkSquash *s, const char *icon)
{
  if (icon == nullptr || icon[0] == '\0' || strchr (icon, '/') != nullptr)
    return nullptr;
  const char *const exts[] = { ".png", ".svg" };
  for (size_t i = 0; i < G_N_ELEMENTS (exts); i++)
    {
      g_autofree char *name = g_strconcat (icon, exts[i], nullptr);
      g_autoptr (GError) error = nullptr;
      GBytes *b = gbk_squash_read (s, name, GBK_APPIMAGE_ICON_MAX, &error);
      if (b != nullptr)
        return b;
    }
  return nullptr;
}

bool
gbk_appimage_read (GbkReadAt read, void *ctx, uint64_t size, GbkAppImage *out, GError **error)
{
  memset (out, 0, sizeof *out);
  uint64_t offset;
  if (!gbk_appimage_offset (read, ctx, size, &offset))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Not an AppImage");
      return false;
    }
  g_autoptr (GbkSquash) s = gbk_squash_open (read, ctx, offset, size, error);
  if (s == nullptr)
    return false;
  g_autoptr (GPtrArray) names = gbk_squash_root_names (s, error);
  if (names == nullptr)
    return false;

  /* The first .desktop file at the top (an AppImage has one). */
  g_autofree char *icon_key = nullptr;
  g_autofree char *id = nullptr;
  for (guint i = 0; i < names->len; i++)
    {
      const char *n = g_ptr_array_index (names, i);
      if (!g_str_has_suffix (n, ".desktop"))
        continue;
      id = g_strndup (n, strlen (n) - strlen (".desktop"));
      g_autoptr (GError) e = nullptr;
      g_autoptr (GBytes) text = gbk_squash_read (s, n, GBK_APPIMAGE_DESKTOP_MAX, &e);
      if (text == nullptr)
        break;
      g_autoptr (GKeyFile) kf = g_key_file_new ();
      size_t len;
      const char *data = g_bytes_get_data (text, &len);
      if (len > 0 && g_key_file_load_from_data (kf, data, len, G_KEY_FILE_NONE, nullptr))
        {
          g_autofree char *name = g_key_file_get_string (kf, "Desktop Entry", "Name", nullptr);
          g_autofree char *version
              = g_key_file_get_string (kf, "Desktop Entry", "X-AppImage-Version", nullptr);
          out->name = clean (name);
          out->version = clean (version);
          icon_key = g_key_file_get_string (kf, "Desktop Entry", "Icon", nullptr);
        }
      break;
    }
  /* No version in the .desktop file: perhaps in its AppStream metadata. */
  if (out->version == nullptr && id != nullptr && id[0] != '\0' && strchr (id, '/') == nullptr)
    out->version = metainfo_version (s, id);

  g_autoptr (GError) e = nullptr;
  out->icon = gbk_squash_read (s, ".DirIcon", GBK_APPIMAGE_ICON_MAX, &e);
  if (out->icon == nullptr)
    out->icon = icon_named (s, icon_key);
  return true;
}
