#include "appimage-out.h"

void
gbk_appimage_info_clear (GbkAppImageInfo *info)
{
  g_clear_pointer (&info->name, g_free);
  g_clear_pointer (&info->version, g_free);
  g_clear_pointer (&info->pixels, g_free);
  info->width = info->height = 0;
}

typedef struct
{
  const uint8_t *p;
  size_t left;
} In;

[[nodiscard]] static bool
take_u32 (In *in, uint32_t *v)
{
  if (in->left < 4)
    return false;
  *v = (uint32_t) in->p[0] | (uint32_t) in->p[1] << 8 | (uint32_t) in->p[2] << 16
       | (uint32_t) in->p[3] << 24;
  in->p += 4;
  in->left -= 4;
  return true;
}

/* Text: valid UTF-8 without control characters; nullptr (and true) when empty. */
[[nodiscard]] static bool
take_text (In *in, char **out)
{
  uint32_t len;
  if (!take_u32 (in, &len) || len > GBK_APPIMAGE_OUT_TEXT_MAX || len > in->left)
    return false;
  const char *s = (const char *) in->p;
  if (!g_utf8_validate_len (s, len, nullptr))
    return false;
  for (uint32_t i = 0; i < len; i++)
    if ((unsigned char) s[i] < 0x20)
      return false;
  *out = len > 0 ? g_strndup (s, len) : nullptr;
  in->p += len;
  in->left -= len;
  return true;
}

bool
gbk_appimage_out_parse (const uint8_t *data, size_t len, GbkAppImageInfo *out)
{
  memset (out, 0, sizeof *out);
  size_t magic = strlen (GBK_APPIMAGE_OUT_MAGIC);
  if (len < magic || memcmp (data, GBK_APPIMAGE_OUT_MAGIC, magic) != 0)
    return false;
  In in = { data + magic, len - magic };
  uint32_t w, h;
  if (!take_text (&in, &out->name) || !take_text (&in, &out->version) || !take_u32 (&in, &w)
      || !take_u32 (&in, &h) || w > GBK_APPIMAGE_OUT_ICON_MAX || h > GBK_APPIMAGE_OUT_ICON_MAX
      || (w == 0) != (h == 0) || in.left != (size_t) w * h * 4)
    {
      gbk_appimage_info_clear (out);
      return false;
    }
  if (w > 0)
    {
      out->pixels = g_new (uint32_t, (size_t) w *h);
      memcpy (out->pixels, in.p, in.left);
      /* Premultiplied: no colour brighter than its pixel's opacity allows. */
      for (size_t i = 0; i < (size_t) w * h; i++)
        {
          uint32_t px = out->pixels[i], a = px >> 24;
          if (((px >> 16) & 0xff) > a || ((px >> 8) & 0xff) > a || (px & 0xff) > a)
            {
              gbk_appimage_info_clear (out);
              return false;
            }
        }
      out->width = (int) w;
      out->height = (int) h;
    }
  return true;
}

static bool
part_end (char c)
{
  return c == '\0' || c == '_' || c == '-' || c == ' ';
}

/* Digits, then one or more ".digits"; the end of them, or nullptr. */
static const char *
dotted (const char *p)
{
  if (!g_ascii_isdigit (*p))
    return nullptr;
  int dots = 0;
  for (;;)
    {
      while (g_ascii_isdigit (*p))
        p++;
      if (p[0] != '.' || !g_ascii_isdigit (p[1]))
        break;
      p++;
      dots++;
    }
  return dots > 0 ? p : nullptr;
}

/* A pre-release mark at `p` ("rc2", "-alpha", "-rc.2", ".beta1"): its end, or `p`. */
static const char *
pre_release (const char *p)
{
  static const char *const marks[] = { "alpha", "beta", "rc", "pre", "preview", "dev", "nightly" };
  const char *q = p;
  if (*q == '-' || *q == '.')
    q++;
  size_t letters = 0;
  while (g_ascii_isalpha (q[letters]))
    letters++;
  bool known = false;
  for (size_t i = 0; i < G_N_ELEMENTS (marks); i++)
    known = known
            || (strlen (marks[i]) == letters && g_ascii_strncasecmp (q, marks[i], letters) == 0);
  if (!known)
    return p;
  q += letters;
  if (*q == '.' && g_ascii_isdigit (q[1]))
    q++;
  while (g_ascii_isdigit (*q))
    q++;
  return q;
}

char *
gbk_appimage_version_in_name (const char *name)
{
  g_autofree char *stem = g_strdup (name);
  size_t len = strlen (stem);
  const char suffix[] = ".appimage";
  if (len >= sizeof suffix - 1
      && g_ascii_strcasecmp (stem + len - (sizeof suffix - 1), suffix) == 0)
    stem[len - (sizeof suffix - 1)] = '\0';
  for (const char *p = stem; *p != '\0'; p++)
    {
      if (p != stem && !part_end (p[-1]))
        continue;
      const char *start = (*p == 'v' || *p == 'V') ? p + 1 : p;
      const char *end = dotted (start);
      if (end == nullptr)
        continue;
      end = pre_release (end);
      if (part_end (*end))
        return g_strndup (start, (size_t) (end - start));
    }
  return nullptr;
}
