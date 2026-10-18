/* Locations (pure): canonical form, navigation, comparison across machines, and the one way
 * to a local path. */
#include "loc.h"

static void
test_canonical (void)
{
  g_autoptr (GbkLoc) a = gbk_loc_new_local ("/a//b/./c/../d/");
  g_assert_cmpstr (gbk_loc_path (a), ==, "/a/b/d");
  g_autoptr (GbkLoc) root = gbk_loc_new_local ("/");
  g_assert_true (gbk_loc_is_root (root));
  g_assert_false (gbk_loc_is_root (a));

  /* Relative: to the current folder, or to another location. */
  g_autofree char *cwd = g_get_current_dir ();
  g_autoptr (GbkLoc) rel = gbk_loc_new_local ("x");
  g_autofree char *expected = g_build_filename (cwd, "x", nullptr);
  g_assert_cmpstr (gbk_loc_path (rel), ==, expected);
  g_autoptr (GbkLoc) like = gbk_loc_new_like (a, "../e");
  g_assert_cmpstr (gbk_loc_path (like), ==, "/a/b/e");
}

static void
test_navigation (void)
{
  g_autoptr (GbkLoc) d = gbk_loc_new_local ("/a/b");
  g_autoptr (GbkLoc) c = gbk_loc_child (d, "c d");
  g_assert_cmpstr (gbk_loc_path (c), ==, "/a/b/c d");
  g_autoptr (GbkLoc) p = gbk_loc_parent (c);
  g_assert_true (gbk_loc_equal (p, d));
  g_autoptr (GbkLoc) root = gbk_loc_new_local ("/");
  g_autoptr (GbkLoc) up = gbk_loc_parent (root);
  g_assert_true (gbk_loc_is_root (up));
  g_autofree char *name = gbk_loc_name (c);
  g_assert_cmpstr (name, ==, "c d");
  g_autofree char *root_name = gbk_loc_name (root);
  g_assert_cmpstr (root_name, ==, "/");
  g_autoptr (GbkLoc) top = gbk_loc_child (root, "etc");
  g_assert_cmpstr (gbk_loc_path (top), ==, "/etc");

  g_assert_true (gbk_loc_is_child_of (c, d));
  g_assert_false (gbk_loc_is_child_of (d, d));
  g_assert_false (gbk_loc_is_child_of (c, root));
  g_assert_false (gbk_loc_is_child_of (root, root));
  g_assert_true (gbk_loc_is_child_of (top, root));
}

static void
test_under (void)
{
  g_autoptr (GbkLoc) ab = gbk_loc_new_local ("/media/a/b");
  g_autoptr (GbkLoc) a = gbk_loc_new_local ("/media/a");
  g_autoptr (GbkLoc) aa = gbk_loc_new_local ("/media/ab");
  g_autoptr (GbkLoc) root = gbk_loc_new_local ("/");
  g_assert_true (gbk_loc_is_under (ab, a));
  g_assert_true (gbk_loc_is_under (a, a));
  g_assert_false (gbk_loc_is_under (aa, a));     /* "/media/ab" is not inside "/media/a" */
  g_assert_false (gbk_loc_is_under (a, ab));
  g_assert_true (gbk_loc_is_under (ab, root));
  g_assert_true (gbk_loc_is_under (root, root));
}

/* The same path on two machines is two different places. */
static void
test_machines (void)
{
  g_autoptr (GbkLoc) here = gbk_loc_new_local ("/home/u/x");
  g_autoptr (GbkLoc) there = gbk_loc_new_remote_for_test ("ws", "/home/u/x");
  g_autoptr (GbkLoc) there2 = gbk_loc_new_remote_for_test ("ws", "/home/u/x");
  g_autoptr (GbkLoc) other = gbk_loc_new_remote_for_test ("ws2", "/home/u/x");
  g_autoptr (GbkLoc) home_there = gbk_loc_new_remote_for_test ("ws", "/home/u");
  g_autoptr (GbkLoc) home_here = gbk_loc_new_local ("/home/u");
  g_assert_false (gbk_loc_equal (here, there));
  g_assert_true (gbk_loc_equal (there, there2));
  g_assert_false (gbk_loc_equal (there, other));
  g_assert_true (gbk_loc_is_under (there, home_there));
  g_assert_false (gbk_loc_is_under (there, home_here));
  g_assert_false (gbk_loc_is_under (here, home_there));
  g_assert_true (gbk_loc_is_child_of (there, home_there));
  g_assert_false (gbk_loc_is_child_of (there, home_here));

  const char *p = "not touched";
  g_assert_true (gbk_loc_is_local (here));
  g_assert_true (gbk_loc_as_local (here, &p));
  g_assert_cmpstr (p, ==, "/home/u/x");
  g_assert_false (gbk_loc_is_local (there));
  g_assert_false (gbk_loc_as_local (there, &p));
  g_assert_null (p);

  /* Navigation stays on the machine. */
  g_autoptr (GbkLoc) parent = gbk_loc_parent (there);
  g_autoptr (GbkLoc) child = gbk_loc_child (there, "y");
  g_autoptr (GbkLoc) like = gbk_loc_new_like (there, "/etc");
  g_assert_false (gbk_loc_is_local (parent));
  g_assert_false (gbk_loc_is_local (child));
  g_assert_false (gbk_loc_is_local (like));

  /* Shown with the host, as scp and rsync take it. */
  g_autofree char *shown_here = gbk_loc_display (here);
  g_autofree char *shown_there = gbk_loc_display (there);
  g_assert_cmpstr (shown_here, ==, "/home/u/x");
  g_assert_cmpstr (shown_there, ==, "ws:/home/u/x");
}

static void
test_arrays (void)
{
  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (paths, g_strdup ("/a"));
  g_ptr_array_add (paths, g_strdup ("/b/c"));
  g_autoptr (GPtrArray) locs = gbk_loc_array_from_local (paths);
  g_autoptr (GPtrArray) back = gbk_loc_array_as_local (locs);
  g_assert_nonnull (back);
  g_assert_cmpuint (back->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (back, 1), ==, "/b/c");

  /* One location elsewhere: no paths at all, never a partial list. */
  g_ptr_array_add (locs, gbk_loc_new_remote_for_test ("ws", "/d"));
  g_autoptr (GPtrArray) none = gbk_loc_array_as_local (locs);
  g_assert_null (none);
}

/* Bytes that aren't UTF-8 stay exact in the path and are escaped for showing. */
static void
test_display (void)
{
  g_autoptr (GbkLoc) odd = gbk_loc_new_local ("/tmp/caf\xe9");
  g_assert_cmpstr (gbk_loc_path (odd), ==, "/tmp/caf\xe9");
  g_autofree char *shown = gbk_loc_display (odd);
  g_autofree char *name = gbk_loc_display_name (odd);
  g_assert_true (g_utf8_validate (shown, -1, nullptr));
  g_assert_true (g_utf8_validate (name, -1, nullptr));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/loc/canonical", test_canonical);
  g_test_add_func ("/loc/navigation", test_navigation);
  g_test_add_func ("/loc/under", test_under);
  g_test_add_func ("/loc/machines", test_machines);
  g_test_add_func ("/loc/arrays", test_arrays);
  g_test_add_func ("/loc/display", test_display);
  return g_test_run ();
}
