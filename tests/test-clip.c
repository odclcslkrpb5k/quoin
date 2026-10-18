#include "cliptext.h"

static GPtrArray *
list (const char *first, ...)
{
  GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
  va_list ap;
  va_start (ap, first);
  for (const char *p = first; p != nullptr; p = va_arg (ap, const char *))
    g_ptr_array_add (a, g_strdup (p));
  va_end (ap);
  return a;
}

static void
test_round_trip (void)
{
  g_autoptr (GPtrArray) paths
      = list ("/home/u/a b.txt", "/home/u/\xff\xfe raw", "/x/%25", (const char *) nullptr);
  for (int cut = 0; cut < 2; cut++)
    {
      g_autofree char *g = gbk_clip_format_gnome (paths, cut);
      g_assert_true (g_str_has_prefix (g, cut ? "cut\nfile:///" : "copy\nfile:///"));
      bool was_cut;
      g_autoptr (GPtrArray) back = gbk_clip_parse (g, strlen (g), true, &was_cut);
      g_assert_cmpint (was_cut, ==, cut);
      g_assert_cmpuint (back->len, ==, 3);
      for (guint i = 0; i < 3; i++)
        g_assert_cmpstr (g_ptr_array_index (back, i), ==, g_ptr_array_index (paths, i));
    }
  g_autofree char *u = gbk_clip_format_uri_list (paths);
  g_assert_true (g_str_has_suffix (u, "\r\n"));
  bool cut;
  g_autoptr (GPtrArray) back = gbk_clip_parse (u, strlen (u), false, &cut);
  g_assert_false (cut);
  g_assert_cmpuint (back->len, ==, 3);
}

static void
test_foreign (void)
{
  const char data[] = "# from a browser\r\nhttp://example.com/\r\nfile:///ok\r\n"
                      "not a uri\r\nfile://remotehost/x\r\nfile:///also%20ok\r\n";
  bool cut;
  g_autoptr (GPtrArray) p = gbk_clip_parse (data, strlen (data), false, &cut);
  g_assert_cmpuint (p->len, ==, 2);
  g_assert_cmpstr (g_ptr_array_index (p, 0), ==, "/ok");
  g_assert_cmpstr (g_ptr_array_index (p, 1), ==, "/also ok");
}

/* Between Quoin windows: locations on any machine, names with any bytes, and nothing a
 * hostile clipboard owner could slip in (a host that's an ssh option, a NUL, a relative
 * path) gets through. */
static void
test_locations (void)
{
  g_autoptr (GPtrArray) locs = gbk_loc_array_new ();
  g_ptr_array_add (locs, gbk_loc_new_local ("/home/u/a b\tc\nd%"));
  g_ptr_array_add (locs, gbk_loc_new_remote ("ws1", "/srv/x"));
  g_ptr_array_add (locs, gbk_loc_new_remote ("[::1]", "/caf\xe9"));
  g_autofree char *text = gbk_clip_format_locs (locs, true);
  bool cut = false;
  g_autoptr (GPtrArray) back = gbk_clip_parse_locs (text, strlen (text), &cut);
  g_assert_true (cut);
  g_assert_cmpuint (back->len, ==, 3);
  for (guint i = 0; i < 3; i++)
    g_assert_true (gbk_loc_equal (g_ptr_array_index (locs, i), g_ptr_array_index (back, i)));

  const char hostile[] = "copy\n-oProxyCommand=x\t/a\nws1\t/b%00c\nws1\trelative\n\t/ok\nnotab\n";
  g_autoptr (GPtrArray) got = gbk_clip_parse_locs (hostile, strlen (hostile), &cut);
  g_assert_false (cut);
  g_assert_cmpuint (got->len, ==, 1);
  g_assert_cmpstr (gbk_loc_path (g_ptr_array_index (got, 0)), ==, "/ok");
  g_assert_true (gbk_loc_is_local (g_ptr_array_index (got, 0)));
  g_autoptr (GPtrArray) none = gbk_clip_parse_locs ("paste\n\t/a", 9, &cut);
  g_assert_cmpuint (none->len, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/clip/round-trip", test_round_trip);
  g_test_add_func ("/clip/foreign", test_foreign);
  g_test_add_func ("/clip/locations", test_locations);
  return g_test_run ();
}
