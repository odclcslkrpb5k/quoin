/* Input: clipboard bytes from an arbitrary application. */
#include "cliptext.h"
#include "fuzz.h"

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  for (int gnome = 0; gnome < 2; gnome++)
    {
      bool cut;
      g_autoptr (GPtrArray) paths = gbk_clip_parse ((const char *) data, size, gnome, &cut);
      for (guint i = 0; i < paths->len; i++)
        g_assert (g_path_is_absolute (g_ptr_array_index (paths, i)));
      /* Round trip: what we parse, we can write back and parse again identically. */
      g_autofree char *again = gbk_clip_format_gnome (paths, cut);
      bool cut2;
      g_autoptr (GPtrArray) back = gbk_clip_parse (again, strlen (again), true, &cut2);
      g_assert (cut2 == cut);
      g_assert (back->len == paths->len);
      for (guint i = 0; i < back->len; i++)
        g_assert (g_str_equal (g_ptr_array_index (back, i), g_ptr_array_index (paths, i)));
    }
  /* Locations between Quoin windows: whatever parses, writes back and parses the same. */
  bool cut;
  g_autoptr (GPtrArray) locs = gbk_clip_parse_locs ((const char *) data, size, &cut);
  g_autofree char *text = gbk_clip_format_locs (locs, cut);
  bool cut2;
  g_autoptr (GPtrArray) back = gbk_clip_parse_locs (text, strlen (text), &cut2);
  g_assert (cut2 == cut && back->len == locs->len);
  for (guint i = 0; i < locs->len; i++)
    {
      const GbkLoc *a = g_ptr_array_index (locs, i);
      g_assert (gbk_loc_equal (a, g_ptr_array_index (back, i)));
      g_assert (gbk_loc_host (a) == nullptr || gbk_host_valid (gbk_loc_host (a)));
    }
  return 0;
}
