#include "viewstate.h"

#define GROUP "Views"

static GKeyFile *state;   /* read on first use */

static char *
state_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "quoin", "views.ini", nullptr);
}

static GKeyFile *
loaded (void)
{
  if (state != nullptr)
    return state;
  state = g_key_file_new ();
  g_autofree char *path = state_path ();
  g_autoptr (GError) error = nullptr;
  if (!g_key_file_load_from_file (state, path, G_KEY_FILE_NONE, &error)
      && !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    g_message ("folder views not read, so each folder shows its default: %s", error->message);
  return state;
}

/* Any location as a key: escaped, so '=', '[' or a newline in a name can't break the file. */
static char *
key_of (const char *where)
{
  return g_uri_escape_string (where, nullptr, false);
}

GbkViewChoice
gbk_view_state_get (const char *where)
{
  g_autofree char *key = key_of (where);
  g_autofree char *v = g_key_file_get_string (loaded (), GROUP, key, nullptr);
  if (v == nullptr)
    return GBK_VIEW_CHOICE_NONE;
  if (g_str_equal (v, "icons"))
    return GBK_VIEW_CHOICE_ICONS;
  if (g_str_equal (v, "compact"))
    return GBK_VIEW_CHOICE_COMPACT;
  return GBK_VIEW_CHOICE_NONE;          /* something newer, or garbled: the default */
}

void
gbk_view_state_set (const char *where, GbkViewChoice choice)
{
  GKeyFile *kf = loaded ();
  g_autofree char *key = key_of (where);
  switch (choice)
    {
    case GBK_VIEW_CHOICE_NONE:
      {
        bool was_there = g_key_file_remove_key (kf, GROUP, key, nullptr);
        (void) was_there;
        break;
      }
    case GBK_VIEW_CHOICE_COMPACT: g_key_file_set_string (kf, GROUP, key, "compact"); break;
    case GBK_VIEW_CHOICE_ICONS: g_key_file_set_string (kf, GROUP, key, "icons"); break;
    default: gbk_fatal ("bad view choice %d", (int) choice);
    }
  g_autofree char *path = state_path ();
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr (GError) error = nullptr;
  /* Written whole to a temporary file and renamed over the old one. */
  if (g_mkdir_with_parents (dir, 0700) != 0)
    g_message ("folder views not saved: %s", g_strerror (errno));
  else if (!g_key_file_save_to_file (kf, path, &error))
    g_message ("folder views not saved: %s", error->message);
}

void
gbk_view_state_forget_for_test (void)
{
  g_clear_pointer (&state, g_key_file_unref);
}
