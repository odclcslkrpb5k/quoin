/* Tests: a real AppImage to look at: a copy of /bin/true marked as an AppImage of type 2,
 * with a squashfs image (made by mksquashfs) holding a .desktop file and a red icon. */
#pragma once

static void
testappimage_make (const char *path, const char *name, const char *version)
{
  g_autofree char *work = g_strdup_printf ("%s.tree", path);
  g_autofree char *icons = g_build_filename (work, "usr", "share", "icons", nullptr);
  g_assert_cmpint (g_mkdir_with_parents (icons, 0755), ==, 0);
  cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 64, 64);
  cairo_t *cr = cairo_create (s);
  cairo_set_source_rgb (cr, 1, 0, 0);
  cairo_paint (cr);
  cairo_destroy (cr);
  g_autofree char *png = g_build_filename (icons, "app.png", nullptr);
  g_assert_cmpint (cairo_surface_write_to_png (s, png), ==, CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy (s);
  g_autofree char *desktop = g_strdup_printf ("[Desktop Entry]\nType=Application\nName=%s\n"
                                              "X-AppImage-Version=%s\nIcon=app\nExec=AppRun\n",
                                              name, version);
  g_autofree char *desktop_path = g_build_filename (work, "app.desktop", nullptr);
  g_assert_true (g_file_set_contents (desktop_path, desktop, -1, nullptr));
  g_autofree char *dir_icon = g_build_filename (work, ".DirIcon", nullptr);
  g_assert_cmpint (symlink ("usr/share/icons/app.png", dir_icon), ==, 0);
  g_autofree char *img = g_strdup_printf ("%s.sqfs", path);
  const char *const argv[] = { "mksquashfs", work, img, "-comp", "gzip", "-noappend", "-quiet",
                               "-all-root", "-no-progress", "-processors", "1", nullptr };
  g_autoptr (GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
  for (size_t i = 0; argv[i] != nullptr; i++)
    g_ptr_array_add (args, g_strdup (argv[i]));
  g_ptr_array_add (args, nullptr);
  int status;
  g_autoptr (GError) error = nullptr;
  g_assert_true (g_spawn_sync (nullptr, (char **) args->pdata, nullptr,
                               G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, nullptr, nullptr,
                               nullptr, nullptr, &status, &error));
  g_assert_true (g_spawn_check_wait_status (status, nullptr));
  char *prog = nullptr, *image = nullptr;
  size_t prog_len = 0, image_len = 0;
  if (!g_file_get_contents ("/bin/true", &prog, &prog_len, nullptr) || prog_len < 64
      || !g_file_get_contents (img, &image, &image_len, nullptr))
    gbk_fatal ("can't make an AppImage");
  prog[8] = 'A';
  prog[9] = 'I';
  prog[10] = 2;
  GByteArray *all = g_byte_array_new_take ((uint8_t *) prog, prog_len);
  g_byte_array_append (all, (const uint8_t *) image, (guint) image_len);
  g_free (image);
  g_assert_true (g_file_set_contents (path, (const char *) all->data, (gssize) all->len, nullptr));
  g_byte_array_unref (all);
  g_assert_cmpint (chmod (path, 0755), ==, 0);
  if (img != nullptr)
    unlink (img);
}
