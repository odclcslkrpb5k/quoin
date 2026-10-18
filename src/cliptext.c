#include "cliptext.h"

#define MAX_CLIP_BYTES (16u << 20)

static char *
join_uris (GPtrArray *paths, const char *sep, const char *prefix, bool trailing)
{
  GString *s = g_string_new (prefix);
  for (guint i = 0; i < paths->len; i++)
    {
      g_autofree char *uri = g_filename_to_uri (g_ptr_array_index (paths, i), nullptr, nullptr);
      if (uri == nullptr)
        continue;                     /* not absolute: can't express it */
      if (i > 0 || prefix[0] != '\0')
        if (s->len > 0)
          g_string_append (s, sep);
      g_string_append (s, uri);
    }
  if (trailing && s->len > 0)
    g_string_append (s, sep);
  return g_string_free (s, false);
}

char *
gbk_clip_format_gnome (GPtrArray *paths, bool cut)
{
  return join_uris (paths, "\n", cut ? "cut" : "copy", false);
}

char *
gbk_clip_format_uri_list (GPtrArray *paths)
{
  return join_uris (paths, "\r\n", "", true);
}

char *
gbk_clip_format_text (GPtrArray *paths)
{
  GString *s = g_string_new ("");
  for (guint i = 0; i < paths->len; i++)
    {
      g_autofree char *shown = g_filename_display_name (g_ptr_array_index (paths, i));
      if (i > 0)
        g_string_append_c (s, '\n');
      g_string_append (s, shown);
    }
  return g_string_free (s, false);
}

GPtrArray *
gbk_clip_parse (const char *data, size_t len, bool gnome, bool *cut)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  *cut = false;
  if (data == nullptr || len > MAX_CLIP_BYTES)
    return out;
  g_autofree char *text = g_strndup (data, len);   /* stops at an embedded NUL */
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);

  for (size_t i = 0; lines[i] != nullptr; i++)
    {
      char *line = g_strstrip (lines[i]);           /* also drops the \r of CRLF */
      if (line[0] == '\0' || line[0] == '#')
        continue;
      if (gnome && i == 0)
        {
          *cut = g_str_equal (line, "cut");
          if (g_str_equal (line, "cut") || g_str_equal (line, "copy"))
            continue;
        }
      /* Ask for the hostname: otherwise GLib silently drops it, and file://otherhost/x
       * would become our local /x. Only local URIs are files we can act on. */
      g_autofree char *host = nullptr;
      g_autofree char *path = g_filename_from_uri (line, &host, nullptr);
      if (path == nullptr || !g_path_is_absolute (path))
        continue;
      if (host != nullptr && host[0] != '\0' && !g_str_equal (host, "localhost"))
        continue;
      g_ptr_array_add (out, g_steal_pointer (&path));
    }
  return out;
}

/* ---- locations, between Quoin windows ---- */

static char *
escape (const char *s)
{
  return g_uri_escape_string (s, "/", false);
}

char *
gbk_clip_format_locs (const GPtrArray *locs, bool cut)
{
  GString *s = g_string_new (cut ? "cut" : "copy");
  for (guint i = 0; i < locs->len; i++)
    {
      const GbkLoc *loc = g_ptr_array_index (locs, i);
      const char *host = gbk_loc_host (loc);
      g_autofree char *h = escape (host != nullptr ? host : "");
      g_autofree char *p = escape (gbk_loc_path (loc));
      g_string_append_printf (s, "\n%s\t%s", h, p);
    }
  return g_string_free (s, false);
}

GPtrArray *
gbk_clip_parse_locs (const char *data, size_t len, bool *cut)
{
  GPtrArray *out = gbk_loc_array_new ();
  *cut = false;
  if (data == nullptr || len > MAX_CLIP_BYTES)
    return out;
  g_autofree char *text = g_strndup (data, len);   /* stops at an embedded NUL */
  g_auto (GStrv) lines = g_strsplit (text, "\n", -1);
  if (lines[0] == nullptr || (!g_str_equal (lines[0], "copy") && !g_str_equal (lines[0], "cut")))
    return out;
  *cut = g_str_equal (lines[0], "cut");
  for (size_t i = 1; lines[i] != nullptr; i++)
    {
      const char *tab = strchr (lines[i], '\t');
      if (tab == nullptr)
        continue;
      g_autofree char *h_esc = g_strndup (lines[i], (size_t) (tab - lines[i]));
      /* Unescaping refuses %00: no NUL can hide in a host or a path. */
      g_autofree char *host = g_uri_unescape_string (h_esc, nullptr);
      g_autofree char *path = g_uri_unescape_string (tab + 1, nullptr);
      if (host == nullptr || path == nullptr || path[0] != '/')
        continue;
      GbkLoc *loc = host[0] == '\0' ? gbk_loc_new_local (path) : gbk_loc_new_remote (host, path);
      if (loc != nullptr)
        g_ptr_array_add (out, loc);
    }
  return out;
}
