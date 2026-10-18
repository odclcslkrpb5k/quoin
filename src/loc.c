#include "loc.h"

struct GbkLoc
{
  gatomicrefcount rc;
  char *host;          /* nullptr: this machine */
  char *path;          /* absolute, canonical */
};

static GbkLoc *
loc_new (const char *host, char *canonical)
{
  GbkLoc *loc = g_new0 (GbkLoc, 1);
  g_atomic_ref_count_init (&loc->rc);
  loc->host = g_strdup (host);
  loc->path = canonical;
  return loc;
}

GbkLoc *
gbk_loc_new_local (const char *path)
{
  return loc_new (nullptr, g_canonicalize_filename (path, nullptr));
}

GbkLoc *
gbk_loc_new_like (const GbkLoc *like, const char *path)
{
  return loc_new (like->host, g_canonicalize_filename (path, like->path));
}

bool
gbk_host_valid (const char *host)
{
  size_t len = strlen (host);
  if (len == 0 || len > 255 || host[0] == '-')
    return false;
  for (size_t i = 0; i < len; i++)
    if (!g_ascii_isalnum (host[i]) && strchr ("._@:%+-[]", host[i]) == nullptr)
      return false;
  return true;
}

GbkLoc *
gbk_loc_new_remote (const char *host, const char *path)
{
  if (!gbk_host_valid (host) || path[0] != '/')
    return nullptr;
  return loc_new (host, g_canonicalize_filename (path, "/"));
}

const char *
gbk_loc_host (const GbkLoc *loc)
{
  return loc->host;
}

void
gbk_address_clear (GbkAddress *a)
{
  g_free (a->host);
  g_free (a->path);
  a->host = a->path = nullptr;
}

bool
gbk_address_parse (const char *text, GbkAddress *out)
{
  *out = (GbkAddress) { 0 };
  if (text[0] == '\0')
    return false;
  const char *path = text;
  /* scp's rule: a colon before any '/' names a machine ("[::1]:" in brackets). A local
   * name with a colon is written "./name:x". */
  if (text[0] != '/' && text[0] != '.' && text[0] != '~')
    {
      const char *colon = nullptr;
      if (text[0] == '[')
        {
          const char *close = strchr (text, ']');
          colon = close != nullptr && close[1] == ':' ? close + 1 : nullptr;
        }
      else
        {
          colon = strchr (text, ':');
          const char *slash = strchr (text, '/');
          if (colon != nullptr && slash != nullptr && slash < colon)
            colon = nullptr;
        }
      if (colon != nullptr)
        {
          g_autofree char *host = g_strndup (text, (size_t) (colon - text));
          if (!gbk_host_valid (host))
            return false;
          out->host = g_steal_pointer (&host);
          path = colon + 1;
        }
    }
  if (path[0] == '/')
    out->base = GBK_ADDRESS_ABSOLUTE;
  else if (path[0] == '~' && (path[1] == '\0' || path[1] == '/'))
    {
      out->base = GBK_ADDRESS_HOME;
      path += path[1] == '/' ? 2 : 1;
    }
  else
    /* Relative: to the current folder here; on another machine, to its home (as scp). */
    out->base = out->host != nullptr ? GBK_ADDRESS_HOME : GBK_ADDRESS_CURRENT;
  out->path = g_strdup (path);
  return true;
}

GbkLoc *
gbk_loc_ref (GbkLoc *loc)
{
  g_atomic_ref_count_inc (&loc->rc);
  return loc;
}

void
gbk_loc_unref (GbkLoc *loc)
{
  if (loc == nullptr || !g_atomic_ref_count_dec (&loc->rc))
    return;
  g_free (loc->host);
  g_free (loc->path);
  g_free (loc);
}

bool
gbk_loc_is_local (const GbkLoc *loc)
{
  return loc->host == nullptr;
}

bool
gbk_loc_as_local (const GbkLoc *loc, const char **out)
{
  *out = loc->host == nullptr ? loc->path : nullptr;
  return *out != nullptr;
}

const char *
gbk_loc_path (const GbkLoc *loc)
{
  return loc->path;
}

bool
gbk_loc_is_root (const GbkLoc *loc)
{
  return g_str_equal (loc->path, "/");
}

GbkLoc *
gbk_loc_parent (const GbkLoc *loc)
{
  return loc_new (loc->host, g_path_get_dirname (loc->path));
}

GbkLoc *
gbk_loc_child (const GbkLoc *dir, const char *name)
{
  if (name[0] == '\0' || strchr (name, '/') != nullptr || g_str_equal (name, ".")
      || g_str_equal (name, ".."))
    gbk_fatal ("a child location needs a single filename component");
  return loc_new (dir->host, g_build_filename (dir->path, name, nullptr));
}

char *
gbk_loc_name (const GbkLoc *loc)
{
  return g_path_get_basename (loc->path);
}

char *
gbk_loc_display_name (const GbkLoc *loc)
{
  return g_filename_display_basename (loc->path);
}

char *
gbk_loc_display (const GbkLoc *loc)
{
  g_autofree char *shown = g_filename_display_name (loc->path);
  if (loc->host == nullptr)
    return g_steal_pointer (&shown);
  return g_strconcat (loc->host, ":", shown, nullptr);
}

static bool
same_machine (const GbkLoc *a, const GbkLoc *b)
{
  return g_strcmp0 (a->host, b->host) == 0;
}

bool
gbk_loc_equal (const GbkLoc *a, const GbkLoc *b)
{
  return same_machine (a, b) && g_str_equal (a->path, b->path);
}

bool
gbk_loc_is_under (const GbkLoc *loc, const GbkLoc *root)
{
  if (!same_machine (loc, root))
    return false;
  /* Canonical paths: no trailing slash except the root itself. */
  size_t n = gbk_loc_is_root (root) ? 0 : strlen (root->path);
  return strncmp (loc->path, root->path, n) == 0 && (loc->path[n] == '\0' || loc->path[n] == '/');
}

bool
gbk_loc_is_child_of (const GbkLoc *loc, const GbkLoc *dir)
{
  if (!same_machine (loc, dir) || gbk_loc_is_root (loc))
    return false;
  g_autofree char *parent = g_path_get_dirname (loc->path);
  return g_str_equal (parent, dir->path);
}

GPtrArray *
gbk_loc_array_new (void)
{
  return g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_loc_unref);
}

GPtrArray *
gbk_loc_array_from_local (const GPtrArray *paths)
{
  GPtrArray *locs = gbk_loc_array_new ();
  for (guint i = 0; i < paths->len; i++)
    g_ptr_array_add (locs, gbk_loc_new_local (g_ptr_array_index (paths, i)));
  return locs;
}

GPtrArray *
gbk_loc_array_as_local (const GPtrArray *locs)
{
  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < locs->len; i++)
    {
      const char *p;
      if (!gbk_loc_as_local (g_ptr_array_index (locs, i), &p))
        return nullptr;
      g_ptr_array_add (paths, g_strdup (p));
    }
  return g_steal_pointer (&paths);
}
