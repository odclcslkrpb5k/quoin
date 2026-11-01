#include "config.h"

#define CONFIG_MAX_BYTES ((size_t) 1024 * 1024)
#define OVERRIDE_WEIGHT 1000      /* config [Types] beat every system glob */

static const char default_terminal[] = "xfce4-terminal --working-directory=%d";
static const char default_terminal_run[] = "xfce4-terminal --hold --working-directory=%d -x %c";
static const char *const default_interpreters[][2] = {
  { "sh", "sh" }, { "bash", "bash" }, { "py", "python3" }, { "pl", "perl" }, { "rb", "ruby" },
};

static void
action_free (GbkAction *a)
{
  g_free (a->id);
  g_free (a->name);
  g_strfreev (a->types);
  gbk_command_free (a->command);
  gbk_command_free (a->remote_command);
  g_free (a);
}

void
gbk_config_free (GbkConfig *c)
{
  if (c == nullptr)
    return;
  gbk_command_free (c->terminal);
  gbk_command_free (c->terminal_run);
  g_hash_table_unref (c->interpreters);
  g_ptr_array_unref (c->type_overrides);
  g_ptr_array_unref (c->actions);
  g_free (c);
}

static GbkConfig *
config_new_empty (void)
{
  GbkConfig *c = g_new0 (GbkConfig, 1);
  c->interpreters
      = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_strfreev);
  c->type_overrides = g_ptr_array_new_with_free_func (g_free);
  c->actions = g_ptr_array_new_with_free_func ((GDestroyNotify) action_free);
  c->sort = GBK_SORT_NAME;
  return c;
}

static GbkCommand *
must_parse (const char *tmpl, const char *allowed)
{
  g_autoptr (GError) error = nullptr;
  GbkCommand *cmd = gbk_command_parse (tmpl, allowed, &error);
  if (cmd == nullptr)
    gbk_fatal ("built-in command \"%s\" invalid: %s", tmpl, error->message);
  return cmd;
}

GbkConfig *
gbk_config_new_default (void)
{
  GbkConfig *c = config_new_empty ();
  c->full_path_title = true;
  c->terminal = must_parse (default_terminal, "d");
  c->terminal_run = must_parse (default_terminal_run, "dc");
  for (size_t i = 0; i < G_N_ELEMENTS (default_interpreters); i++)
    {
      char **argv = g_new0 (char *, 2);
      argv[0] = g_strdup (default_interpreters[i][1]);
      g_hash_table_insert (c->interpreters, g_strdup (default_interpreters[i][0]), argv);
    }
  return c;
}

/* ---- loading ---- */

typedef struct
{
  GKeyFile *kf;
  GPtrArray *errors;
  GbkConfig *c;
} Loader;

G_GNUC_PRINTF (2, 3) static void fail (Loader *l, const char *format, ...)
{
  va_list args;
  va_start (args, format);
  g_ptr_array_add (l->errors, g_strdup_vprintf (format, args));
  va_end (args);
}

static bool
check_keys (Loader *l, const char *group, const char *const *allowed)
{
  g_auto (GStrv) keys = g_key_file_get_keys (l->kf, group, nullptr, nullptr);
  bool ok = true;
  for (size_t i = 0; keys != nullptr && keys[i] != nullptr; i++)
    if (!g_strv_contains (allowed, keys[i]))
      {
        fail (l, "[%s] unknown key \"%s\"", group, keys[i]);
        ok = false;
      }
  return ok;
}

static char *
get_string (Loader *l, const char *group, const char *key)
{
  g_autoptr (GError) error = nullptr;
  char *v = g_key_file_get_string (l->kf, group, key, &error);
  if (v == nullptr)
    fail (l, "[%s] %s: %s", group, key, error->message);
  return v;
}

static GbkCommand *
get_command (Loader *l, const char *group, const char *key, const char *allowed)
{
  g_autofree char *tmpl = get_string (l, group, key);
  if (tmpl == nullptr)
    return nullptr;
  g_autoptr (GError) error = nullptr;
  GbkCommand *cmd = gbk_command_parse (tmpl, allowed, &error);
  if (cmd == nullptr)
    fail (l, "[%s] %s: %s", group, key, error->message);
  return cmd;
}

static bool
get_bool (Loader *l, const char *group, const char *key, bool *out)
{
  g_autofree char *v = get_string (l, group, key);
  if (v == nullptr)
    return false;
  if (g_str_equal (v, "true"))
    *out = true;
  else if (g_str_equal (v, "false"))
    *out = false;
  else
    {
      fail (l, "[%s] %s: expected true or false, got \"%s\"", group, key, v);
      return false;
    }
  return true;
}

static void
load_general (Loader *l)
{
  static const char *const keys[]
      = { "Terminal", "TerminalRun", "ShowHidden", "FullPathInTitle", "SortBy", nullptr };
  const char *g = "General";
  check_keys (l, g, keys);

  if (g_key_file_has_key (l->kf, g, "Terminal", nullptr))
    {
      GbkCommand *cmd = get_command (l, g, "Terminal", "d");
      if (cmd != nullptr)
        {
          gbk_command_free (l->c->terminal);
          l->c->terminal = cmd;
        }
    }
  if (g_key_file_has_key (l->kf, g, "TerminalRun", nullptr))
    {
      GbkCommand *cmd = get_command (l, g, "TerminalRun", "dc");
      if (cmd != nullptr && !gbk_command_uses (cmd, 'c'))
        {
          fail (l, "[%s] TerminalRun: must contain %%c (the command to run)", g);
          g_clear_pointer (&cmd, gbk_command_free);
        }
      if (cmd != nullptr)
        {
          gbk_command_free (l->c->terminal_run);
          l->c->terminal_run = cmd;
        }
    }
  if (g_key_file_has_key (l->kf, g, "ShowHidden", nullptr))
    get_bool (l, g, "ShowHidden", &l->c->show_hidden);
  if (g_key_file_has_key (l->kf, g, "FullPathInTitle", nullptr))
    get_bool (l, g, "FullPathInTitle", &l->c->full_path_title);
  if (g_key_file_has_key (l->kf, g, "SortBy", nullptr))
    {
      g_autofree char *v = get_string (l, g, "SortBy");
      if (v == nullptr)
        ;
      else if (g_str_equal (v, "name"))
        l->c->sort = GBK_SORT_NAME;
      else if (g_str_equal (v, "modified"))
        l->c->sort = GBK_SORT_MTIME;
      else if (g_str_equal (v, "size"))
        l->c->sort = GBK_SORT_SIZE;
      else
        fail (l, "[%s] SortBy: expected name, modified or size, got \"%s\"", g, v);
    }
}

/* An extension as used in [Interpreters] and [Types]: no dots at the ends, no '/'. */
static bool
valid_ext_key (const char *ext)
{
  size_t len = strlen (ext);
  return len > 0 && len <= GBK_TYPE_MAX_EXT && ext[0] != '.' && ext[len - 1] != '.'
         && strchr (ext, '/') == nullptr;
}

static void
load_interpreters (Loader *l)
{
  const char *g = "Interpreters";
  /* The section replaces the defaults entirely: what you see is what you get. */
  g_hash_table_remove_all (l->c->interpreters);
  g_auto (GStrv) keys = g_key_file_get_keys (l->kf, g, nullptr, nullptr);
  for (size_t i = 0; keys != nullptr && keys[i] != nullptr; i++)
    {
      if (!valid_ext_key (keys[i]))
        {
          fail (l, "[%s] \"%s\" is not a valid extension", g, keys[i]);
          continue;
        }
      g_autofree char *v = get_string (l, g, keys[i]);
      if (v == nullptr)
        continue;
      int argc;
      char **argv = nullptr;
      g_autoptr (GError) error = nullptr;
      if (!g_shell_parse_argv (v, &argc, &argv, &error))
        {
          fail (l, "[%s] %s: %s", g, keys[i], error->message);
          continue;
        }
      if (argv[0][0] == '\0' || strchr (v, '%') != nullptr)
        {
          fail (l, "[%s] %s: expected a program (and options), no placeholders", g, keys[i]);
          g_strfreev (argv);
          continue;
        }
      g_hash_table_replace (l->c->interpreters, g_ascii_strdown (keys[i], -1), argv);
    }
}

static void
load_types (Loader *l)
{
  const char *g = "Types";
  g_auto (GStrv) keys = g_key_file_get_keys (l->kf, g, nullptr, nullptr);
  /* Validate against a scratch builder: exactly the rules the real table applies. */
  g_autoptr (GbkTypeBuilder) probe = gbk_type_builder_new ();
  for (size_t i = 0; keys != nullptr && keys[i] != nullptr; i++)
    {
      g_autofree char *mime = get_string (l, g, keys[i]);
      if (mime == nullptr)
        continue;
      if (!valid_ext_key (keys[i])
          || !gbk_type_builder_add (probe, keys[i], mime, OVERRIDE_WEIGHT, false))
        {
          fail (l, "[%s] %s=%s: not a valid extension and MIME type", g, keys[i], mime);
          continue;
        }
      g_ptr_array_add (l->c->type_overrides, g_strdup (keys[i]));
      g_ptr_array_add (l->c->type_overrides, g_steal_pointer (&mime));
    }
}

static bool
valid_type_pattern (const char *p)
{
  if (g_str_equal (p, "*"))
    return true;
  const char *slash = strchr (p, '/');
  if (slash == nullptr || slash == p || slash[1] == '\0' || strchr (slash + 1, '/') != nullptr)
    return false;
  const char *star = strchr (p, '*');
  return star == nullptr
         || (star == slash + 1 && star[1] == '\0');   /* only a trailing star after the slash */
}

static void
load_action (Loader *l, const char *group)
{
  static const char *const keys[]
      = { "Name", "Types", "Directories", "Command", "RemoteCommand", nullptr };
  check_keys (l, group, keys);
  const char *id = group + strlen ("Action ");
  if (id[0] == '\0')
    {
      fail (l, "[%s] action needs an id: [Action <id>]", group);
      return;
    }

  g_autoptr (GbkCommand) cmd = get_command (l, group, "Command", "dfF");
  /* Optional: what runs instead for files on another machine, there (no display there). */
  g_autoptr (GbkCommand) remote = g_key_file_has_key (l->kf, group, "RemoteCommand", nullptr)
                                      ? get_command (l, group, "RemoteCommand", "dfF")
                                      : nullptr;
  g_autofree char *name = get_string (l, group, "Name");
  bool dirs = false;
  if (g_key_file_has_key (l->kf, group, "Directories", nullptr))
    get_bool (l, group, "Directories", &dirs);
  g_auto (GStrv) types = nullptr;
  if (g_key_file_has_key (l->kf, group, "Types", nullptr))
    {
      types = g_key_file_get_string_list (l->kf, group, "Types", nullptr, nullptr);
      for (size_t i = 0; types != nullptr && types[i] != nullptr; i++)
        if (!valid_type_pattern (types[i]))
          fail (l, "[%s] Types: \"%s\" is not a MIME type or type/* pattern", group, types[i]);
    }
  if (types == nullptr)
    types = g_new0 (char *, 1);
  if (types[0] == nullptr && !dirs)
    fail (l, "[%s] matches nothing: give Types and/or Directories=true", group);
  if (cmd == nullptr || name == nullptr)
    return;
  if (name[0] == '\0')
    {
      fail (l, "[%s] Name is empty", group);
      return;
    }

  GbkAction *a = g_new0 (GbkAction, 1);
  a->id = g_strdup (id);
  a->name = g_steal_pointer (&name);
  a->types = g_steal_pointer (&types);
  a->directories = dirs;
  a->command = g_steal_pointer (&cmd);
  a->remote_command = g_steal_pointer (&remote);
  g_ptr_array_add (l->c->actions, a);
}

GbkConfig *
gbk_config_load_data (const char *data, size_t len, GPtrArray *errors)
{
  g_autoptr (GPtrArray) own_errors = g_ptr_array_new_with_free_func (g_free);
  Loader l = { .errors = own_errors, .c = gbk_config_new_default () };
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  l.kf = kf;

  g_autoptr (GError) error = nullptr;
  if (len > CONFIG_MAX_BYTES)
    fail (&l, "config file is larger than %zu bytes", CONFIG_MAX_BYTES);
  else if (memchr (data, '\0', len) != nullptr)
    fail (&l, "config file contains a NUL byte");
  else if (!g_key_file_load_from_data (kf, data, len, G_KEY_FILE_NONE, &error))
    fail (&l, "%s", error->message);
  else
    {
      g_auto (GStrv) groups = g_key_file_get_groups (kf, nullptr);
      for (size_t i = 0; groups[i] != nullptr; i++)
        {
          const char *g = groups[i];
          if (g_str_equal (g, "General"))
            load_general (&l);
          else if (g_str_equal (g, "Interpreters"))
            load_interpreters (&l);
          else if (g_str_equal (g, "Types"))
            load_types (&l);
          else if (g_str_has_prefix (g, "Action "))
            load_action (&l, g);
          else
            fail (&l, "unknown section [%s]", g);
        }
    }

  if (own_errors->len > 0)
    {
      gbk_config_free (l.c);
      l.c = gbk_config_new_default ();
      if (errors != nullptr)
        for (guint i = 0; i < own_errors->len; i++)
          g_ptr_array_add (errors, g_strdup (g_ptr_array_index (own_errors, i)));
    }
  return l.c;
}

GbkConfig *
gbk_config_load_file (const char *path, GPtrArray *errors)
{
  g_autofree char *data = nullptr;
  gsize len = 0;
  g_autoptr (GError) error = nullptr;
  if (!g_file_get_contents (path, &data, &len, &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT) && errors != nullptr)
        g_ptr_array_add (errors, g_strdup_printf ("%s: %s", path, error->message));
      return gbk_config_new_default ();
    }
  return gbk_config_load_data (data, len, errors);
}

char *
gbk_config_path_in (const char *config_home)
{
  g_autofree char *dir = g_build_filename (config_home, "quoin", nullptr);
  g_autofree char *path = g_build_filename (dir, "quoin.conf", nullptr);
  g_autofree char *old_dir = g_build_filename (config_home, "gbk-file", nullptr);
  g_autofree char *old = g_build_filename (old_dir, "gbk-file.conf", nullptr);
  struct stat st;
  if (lstat (path, &st) == 0 || lstat (old, &st) != 0)
    return g_steal_pointer (&path);
  /* Only the pre-rename file: move it across (never over anything), once. */
  if ((mkdir (dir, 0700) == 0 || errno == EEXIST)
      && renameat2 (AT_FDCWD, old, AT_FDCWD, path, RENAME_NOREPLACE) == 0)
    {
      rmdir (old_dir);                  /* only succeeds if nothing else was in there */
      return g_steal_pointer (&path);
    }
  return g_steal_pointer (&old);        /* couldn't move it: read it where it is */
}

char *
gbk_config_default_path (void)
{
  return gbk_config_path_in (g_get_user_config_dir ());
}

/* ---- queries ---- */

const char *const *
gbk_config_interpreter (const GbkConfig *c, const char *name)
{
  for (size_t i = 1; name[i] != '\0'; i++)      /* i = 1: leading dot isn't an extension */
    {
      if (name[i] != '.' || name[i + 1] == '\0')
        continue;
      g_autofree char *ext = g_ascii_strdown (name + i + 1, -1);
      char **argv = g_hash_table_lookup (c->interpreters, ext);
      if (argv != nullptr)
        return (const char *const *) argv;
    }
  return nullptr;
}

void
gbk_config_apply_types (const GbkConfig *c, GbkTypeBuilder *b)
{
  for (guint i = 0; i + 1 < c->type_overrides->len; i += 2)
    {
      bool ok = gbk_type_builder_add (b, g_ptr_array_index (c->type_overrides, i),
                                      g_ptr_array_index (c->type_overrides, i + 1), OVERRIDE_WEIGHT,
                                      false);
      g_assert (ok);    /* validated at load */
    }
}

bool
gbk_action_matches (const GbkAction *a, const char *mime, bool is_dir)
{
  if (is_dir)
    return a->directories;
  for (size_t i = 0; a->types[i] != nullptr; i++)
    {
      const char *p = a->types[i];
      if (g_str_equal (p, "*") || g_str_equal (p, mime))
        return true;
      size_t n = strlen (p);
      if (n >= 2 && p[n - 1] == '*' && strncmp (p, mime, n - 1) == 0)
        return true;              /* type wildcard, e.g. image/<any> */
    }
  return false;
}
