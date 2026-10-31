#include "config.h"

static GbkConfig *
load (const char *text, GPtrArray *errors)
{
  return gbk_config_load_data (text, strlen (text), errors);
}

static void
assert_errors (const char *text, const char *expected_fragment)
{
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GbkConfig) c = load (text, errors);
  g_assert_cmpuint (errors->len, >, 0);
  bool found = false;
  for (guint i = 0; i < errors->len; i++)
    found |= strstr (g_ptr_array_index (errors, i), expected_fragment) != nullptr;
  if (!found)
    {
      for (guint i = 0; i < errors->len; i++)
        g_test_message ("error: %s", (char *) g_ptr_array_index (errors, i));
      g_assert_true (found);
    }
  /* rejected: defaults in force */
  g_assert_cmpstr (gbk_command_template (c->terminal), ==, "xfce4-terminal --working-directory=%d");
  g_assert_cmpuint (c->actions->len, ==, 0);
}

static void
test_defaults (void)
{
  g_autoptr (GbkConfig) c = gbk_config_new_default ();
  g_assert_false (c->show_hidden);
  g_assert_true (c->full_path_title);
  g_assert_cmpint (c->sort, ==, GBK_SORT_NAME);
  g_assert_true (gbk_command_uses (c->terminal_run, 'c'));
  g_assert_cmpstr (gbk_config_interpreter (c, "x.py")[0], ==, "python3");
  g_assert_cmpstr (gbk_config_interpreter (c, "X.SH")[0], ==, "sh");
  g_assert_null (gbk_config_interpreter (c, "x.txt"));
  g_assert_null (gbk_config_interpreter (c, ".sh"));     /* dotfile, no extension */
  g_assert_null (gbk_config_interpreter (c, "sh"));
}

static void
test_example_conf (void)
{
  /* docs/example.conf must always load cleanly: the documentation can't rot. */
  g_autofree char *path
      = g_build_filename (g_getenv ("GBK_SOURCE_ROOT"), "docs", "example.conf", nullptr);
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GbkConfig) c = gbk_config_load_file (path, errors);
  for (guint i = 0; i < errors->len; i++)
    g_test_message ("error: %s", (char *) g_ptr_array_index (errors, i));
  g_assert_cmpuint (errors->len, ==, 0);
  g_assert_cmpuint (c->actions->len, ==, 3);
  const GbkAction *a = g_ptr_array_index (c->actions, 0);
  g_assert_cmpstr (a->id, ==, "extract-here");
  g_assert_true (gbk_action_matches (a, "application/zip", false));
  g_assert_false (gbk_action_matches (a, "text/plain", false));
  g_assert_false (gbk_action_matches (a, "inode/directory", true));
  const GbkAction *compress = g_ptr_array_index (c->actions, 2);
  g_assert_true (gbk_action_matches (compress, "text/plain", false));
  g_assert_true (gbk_action_matches (compress, "inode/directory", true));
}

static void
test_full (void)
{
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GbkConfig) c = load ("[General]\n"
                                  "Terminal=st -d %d\n"
                                  "TerminalRun=st -d %d -e %c\n"
                                  "ShowHidden=true\n"
                                  "FullPathInTitle=false\n"
                                  "SortBy=modified\n"
                                  "[Interpreters]\n"
                                  "py=python3 -u\n"
                                  "[Types]\n"
                                  "foo=application/x-foo\n"
                                  "[Action view]\n"
                                  "Name=View\n"
                                  "Types=image/*\n"
                                  "Command=viewer %f\n",
                                  errors);
  g_assert_cmpuint (errors->len, ==, 0);
  g_assert_true (c->show_hidden);
  g_assert_false (c->full_path_title);
  g_assert_cmpint (c->sort, ==, GBK_SORT_MTIME);
  g_assert_cmpstr (gbk_command_template (c->terminal), ==, "st -d %d");
  const char *const *py = gbk_config_interpreter (c, "a.py");
  g_assert_cmpstr (py[0], ==, "python3");
  g_assert_cmpstr (py[1], ==, "-u");
  g_assert_null (gbk_config_interpreter (c, "a.sh"));   /* section replaced defaults */
  const GbkAction *view = g_ptr_array_index (c->actions, 0);
  g_assert_true (gbk_action_matches (view, "image/png", false));
  g_assert_false (gbk_action_matches (view, "imagex/png", false));
  g_assert_false (gbk_action_matches (view, "video/mp4", false));

  GbkTypeBuilder *b = gbk_type_builder_new ();
  gbk_config_apply_types (c, b);
  g_autoptr (GbkTypeTable) t = gbk_type_builder_finish (b);
  g_assert_cmpstr (
      gbk_type_table_mime (t, gbk_type_table_lookup (t, "x.foo", 5, GBK_KIND_REGULAR, false)), ==,
      "application/x-foo");
}

static void
test_errors (void)
{
  assert_errors ("[Genral]\n", "unknown section [Genral]");
  assert_errors ("[General]\nTerminl=x\n", "unknown key \"Terminl\"");
  assert_errors ("[General]\nShowHidden=yes\n", "expected true or false");
  assert_errors ("[General]\nSortBy=bigness\n", "expected name, modified or size");
  assert_errors ("[General]\nTerminal=st %f\n", "not allowed here");
  assert_errors ("[General]\nTerminalRun=st -d %d\n", "must contain %c");
  assert_errors ("[General]\nTerminal=st 'oops\n", "can't parse command");
  assert_errors ("[Interpreters]\n.py=python3\n", "not a valid extension");
  assert_errors ("[Interpreters]\npy=python3 %f\n", "no placeholders");
  assert_errors ("[Types]\nfoo=notamime\n", "not a valid extension and MIME type");
  assert_errors ("[Action ]\nName=x\nCommand=x\nTypes=*\n", "needs an id");
  assert_errors ("[Action a]\nName=x\nTypes=*\n", "Command");
  assert_errors ("[Action a]\nCommand=x\nTypes=*\n", "Name");
  assert_errors ("[Action a]\nName=x\nCommand=x\n", "matches nothing");
  assert_errors ("[Action a]\nName=x\nCommand=x\nTypes=image/png*\n", "type/* pattern");
  assert_errors ("[Action a]\nName=x\nCommand=x %c\nTypes=*\n", "not allowed here");
  assert_errors ("not a key file", "");
  /* A bad entry anywhere rejects everything, including valid parts. */
  assert_errors ("[General]\nShowHidden=true\n[Bogus]\n", "unknown section");
}

static void
test_missing_file (void)
{
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GbkConfig) c = gbk_config_load_file ("/nonexistent/gbk.conf", errors);
  g_assert_cmpuint (errors->len, ==, 0);
  g_assert_nonnull (c->terminal);
}

/* The config from before the rename (gbk-file/gbk-file.conf) moves to quoin/quoin.conf once;
 * an existing new one is never touched. */
static void
test_old_location_moves (void)
{
  g_autofree char *home = g_dir_make_tmp ("gbk-conf-XXXXXX", nullptr);
  g_autofree char *new_path = g_build_filename (home, "quoin", "quoin.conf", nullptr);
  g_autofree char *old_dir = g_build_filename (home, "gbk-file", nullptr);
  g_autofree char *old = g_build_filename (old_dir, "gbk-file.conf", nullptr);

  g_autofree char *p0 = gbk_config_path_in (home);        /* nothing anywhere */
  g_assert_cmpstr (p0, ==, new_path);
  g_assert_false (g_file_test (new_path, G_FILE_TEST_EXISTS));

  g_assert_cmpint (g_mkdir (old_dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (old, "[General]\nShowHidden=true\n", -1, nullptr));
  g_autofree char *p1 = gbk_config_path_in (home);
  g_assert_cmpstr (p1, ==, new_path);
  g_autofree char *moved = nullptr;
  g_assert_true (g_file_get_contents (new_path, &moved, nullptr, nullptr));
  g_assert_cmpstr (moved, ==, "[General]\nShowHidden=true\n");
  g_assert_false (g_file_test (old_dir, G_FILE_TEST_EXISTS));   /* emptied, removed */

  /* Both exist: the new one wins, the old one stays put. */
  g_assert_cmpint (g_mkdir (old_dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (old, "old", -1, nullptr));
  g_autofree char *p2 = gbk_config_path_in (home);
  g_assert_cmpstr (p2, ==, new_path);
  g_assert_true (g_file_test (old, G_FILE_TEST_EXISTS));
  g_autofree char *kept = nullptr;
  g_assert_true (g_file_get_contents (new_path, &kept, nullptr, nullptr));
  g_assert_cmpstr (kept, ==, moved);

  unlink (old);
  rmdir (old_dir);
  unlink (new_path);
  g_autofree char *new_dir = g_path_get_dirname (new_path);
  rmdir (new_dir);
  rmdir (home);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/config/defaults", test_defaults);
  g_test_add_func ("/config/example-conf", test_example_conf);
  g_test_add_func ("/config/full", test_full);
  g_test_add_func ("/config/errors", test_errors);
  g_test_add_func ("/config/missing-file", test_missing_file);
  g_test_add_func ("/config/old-location-moves", test_old_location_moves);
  return g_test_run ();
}
