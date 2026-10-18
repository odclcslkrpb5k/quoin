#include "open.h"
#include "launch.h"

void
gbk_item_free (GbkItem *it)
{
  if (it == nullptr)
    return;
  g_clear_pointer (&it->loc, gbk_loc_unref);
  g_free (it->display);
  g_free (it->mime);
  g_free (it);
}

GbkItem *
gbk_item_new (const GbkConfig *config, const GbkTypeTable *types, const GbkLoc *dir,
              const GbkEntry *e)
{
  GbkItem *it = g_new0 (GbkItem, 1);
  it->loc = gbk_loc_child (dir, gbk_entry_name (e));
  it->display = g_strdup (gbk_entry_display (e));
  it->mime = g_strdup (gbk_type_table_mime (types, e->type));
  it->is_dir = gbk_entry_is_dir (e);
  it->regular = e->kind == GBK_KIND_REGULAR;
  it->executable = (e->flags & GBK_ENTRY_EXECUTABLE) != 0;
  it->script = !it->is_dir && gbk_config_interpreter (config, gbk_entry_name (e)) != nullptr;
  return it;
}

GbkItem *
gbk_item_new_dir (GbkLoc *dir)
{
  GbkItem *it = g_new0 (GbkItem, 1);
  it->loc = gbk_loc_ref (dir);
  it->display = gbk_loc_display (dir);
  it->mime = g_strdup ("inode/directory");
  it->is_dir = true;
  return it;
}

/* The local path of `loc`, or an error naming `display`: launching, running and actions
 * need the file on this machine. */
[[nodiscard]] static bool
local_path (const GbkLoc *loc, const char *display, const char **out, GError **error)
{
  if (gbk_loc_as_local (loc, out))
    return true;
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "\"%s\" is on another machine: not possible there yet", display);
  return false;
}

GbkOpenKind
gbk_open_plan (const GbkItem *it)
{
  if (it->is_dir)
    return GBK_OPEN_ENTER;
  if (it->executable)
    return it->script ? GBK_OPEN_RUN_TERMINAL : GBK_OPEN_RUN;
  return GBK_OPEN_DEFAULT;
}

bool
gbk_open_can_execute (const GbkItem *it)
{
  return !it->is_dir && (it->executable || it->script);
}

char **
gbk_open_exec_argv (const GbkConfig *config, const GbkItem *it)
{
  const char *path;
  if (!gbk_open_can_execute (it) || !gbk_loc_as_local (it->loc, &path))
    return nullptr;
  GPtrArray *argv = g_ptr_array_new ();
  if (!it->executable)
    {
      g_autofree char *base = gbk_loc_name (it->loc);
      const char *const *interp = gbk_config_interpreter (config, base);
      if (interp == nullptr)
        gbk_fatal ("script without interpreter");   /* it->script says there is one */
      for (size_t i = 0; interp[i] != nullptr; i++)
        g_ptr_array_add (argv, g_strdup (interp[i]));
    }
  g_ptr_array_add (argv, g_strdup (path));
  g_ptr_array_add (argv, nullptr);
  return (char **) g_ptr_array_free (argv, false);
}

bool
gbk_open_execute (const GbkConfig *config, const GbkItem *it, bool terminal, const GbkLoc *cwd_loc,
                  GError **error)
{
  const char *path, *cwd;
  if (!local_path (it->loc, it->display, &path, error)
      || !local_path (cwd_loc, it->display, &cwd, error))
    return false;
  g_auto (GStrv) argv = gbk_open_exec_argv (config, it);
  if (argv == nullptr)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s can't be executed",
                   it->display);
      return false;
    }
  if (!terminal)
    return gbk_launch_argv (argv, cwd, error);
  GbkCommandArgs args = {
    .dir = cwd,
    .run = (const char *const *) argv,
    .n_run = g_strv_length (argv),
  };
  return gbk_launch_command (config->terminal_run, &args, error);
}

/* Local files only: never a GFile for anything else (gvfs would mount it). */
[[nodiscard]] static bool
files_of (GPtrArray *items, GList **files, GError **error)
{
  *files = nullptr;
  for (guint i = items->len; i > 0; i--)
    {
      const GbkItem *it = g_ptr_array_index (items, i - 1);
      const char *path;
      if (!local_path (it->loc, it->display, &path, error))
        {
          g_list_free_full (g_steal_pointer (files), g_object_unref);
          return false;
        }
      *files = g_list_prepend (*files, g_file_new_for_path (path));
    }
  return true;
}

bool
gbk_open_with (GAppInfo *app, GPtrArray *items, GAppLaunchContext *launch, GError **error)
{
  GList *files;
  if (!files_of (items, &files, error))
    return false;
  bool ok = g_app_info_launch (app, files, launch, error);
  g_list_free_full (files, g_object_unref);
  return ok;
}

bool
gbk_open_default (GPtrArray *items, GAppLaunchContext *launch, GError **error)
{
  /* Group by default application so e.g. 5 images open in one viewer, not 5 viewers. */
  g_autoptr (GHashTable) by_app
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  g_autoptr (GHashTable) apps
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  for (guint i = 0; i < items->len; i++)
    {
      GbkItem *it = g_ptr_array_index (items, i);
      GAppInfo *app = g_app_info_get_default_for_type (it->mime, false);
      if (app == nullptr)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "No application is set to open \"%s\" (%s)", it->display, it->mime);
          return false;
        }
      const char *id = g_app_info_get_id (app);
      g_autofree char *key = g_strdup (id != nullptr ? id : g_app_info_get_executable (app));
      GPtrArray *group = g_hash_table_lookup (by_app, key);
      if (group == nullptr)
        {
          group = g_ptr_array_new ();
          g_hash_table_insert (by_app, g_strdup (key), group);
          g_hash_table_insert (apps, g_strdup (key), app);
        }
      else
        g_object_unref (app);
      g_ptr_array_add (group, it);
    }

  GHashTableIter iter;
  void *key, *group;
  g_hash_table_iter_init (&iter, by_app);
  while (g_hash_table_iter_next (&iter, &key, &group))
    if (!gbk_open_with (g_hash_table_lookup (apps, key), group, launch, error))
      return false;
  return true;
}

bool
gbk_open_action (const GbkAction *a, GPtrArray *items, const GbkLoc *cwd_loc, GError **error)
{
  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < items->len; i++)
    {
      const GbkItem *it = g_ptr_array_index (items, i);
      const char *path;
      if (!local_path (it->loc, it->display, &path, error))
        return false;
      g_ptr_array_add (paths, g_strdup (path));
    }
  g_autofree char *shown = gbk_loc_display (cwd_loc);
  const char *cwd;
  if (!local_path (cwd_loc, shown, &cwd, error))
    return false;
  GbkCommandArgs args = {
    .dir = cwd,
    .files = (const char *const *) paths->pdata,
    .n_files = paths->len,
  };
  return gbk_launch_command (a->command, &args, error);
}

bool
gbk_open_terminal (const GbkConfig *config, const GbkLoc *dir_loc, GError **error)
{
  g_autofree char *shown = gbk_loc_display (dir_loc);
  const char *dir;
  if (!local_path (dir_loc, shown, &dir, error))
    return false;
  GbkCommandArgs args = { .dir = dir };
  return gbk_launch_command (config->terminal, &args, error);
}
