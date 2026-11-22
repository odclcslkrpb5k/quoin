/* Reading AppImages: squashfs images made by mksquashfs (each compression, data blocks and
 * fragments, links), wrapped in an ELF marked as an AppImage; the quoin-appimage helper
 * run for real; and what must be refused: links out of the image or round in a loop,
 * images cut short, files that aren't AppImages, and bad output from the helper. */
#include "appimage-out.h"
#include "appimage.h"

static char *tmp;

static bool
mem_read (void *ctx, uint64_t offset, void *buf, size_t len)
{
  size_t size;
  const uint8_t *d = g_bytes_get_data (ctx, &size);
  if (offset > size || len > size - offset)
    return false;
  memcpy (buf, d + offset, len);
  return true;
}

static void
run (const char *const *args)
{
  int status;
  g_autoptr (GError) error = nullptr;
  g_autofree char *err = nullptr;
  g_autoptr (GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
  for (size_t i = 0; args[i] != nullptr; i++)
    g_ptr_array_add (argv, g_strdup (args[i]));
  g_ptr_array_add (argv, nullptr);
  if (!g_spawn_sync (nullptr, (char **) argv->pdata, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                     nullptr, &err, &status, &error)
      || !g_spawn_check_wait_status (status, nullptr))
    gbk_fatal ("%s failed: %s", args[0], err != nullptr ? err : error->message);
}

static void
put (const char *dir, const char *rel, const void *data, size_t len)
{
  g_autofree char *p = g_build_filename (dir, rel, nullptr);
  g_autofree char *parent = g_path_get_dirname (p);
  g_assert_cmpint (g_mkdir_with_parents (parent, 0755), ==, 0);
  g_assert_true (g_file_set_contents (p, data, (gssize) len, nullptr));
}

static void
link_at (const char *dir, const char *rel, const char *target)
{
  g_autofree char *p = g_build_filename (dir, rel, nullptr);
  g_assert_cmpint (symlink (target, p), ==, 0);
}

/* A 64 x 64 PNG: opaque red, with a transparent corner. */
static GBytes *
make_png (void)
{
  cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 64, 64);
  cairo_t *cr = cairo_create (s);
  cairo_set_source_rgb (cr, 1, 0, 0);
  cairo_rectangle (cr, 8, 8, 56, 56);
  cairo_fill (cr);
  cairo_destroy (cr);
  g_autofree char *path = g_build_filename (tmp, "icon.png", nullptr);
  g_assert_cmpint (cairo_surface_write_to_png (s, path), ==, CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy (s);
  char *data;
  size_t len;
  g_assert_true (g_file_get_contents (path, &data, &len, nullptr));
  return g_bytes_new_take (data, len);
}

static GBytes *big;    /* three blocks and a bit: data blocks and a fragment */
static GBytes *png;

/* A folder as an AppImage's contents, the image of it, and an AppImage around that. */
static char *
make_tree (const char *name, bool hostile)
{
  g_autofree char *dir = g_build_filename (tmp, name, nullptr);
  const char desktop[] = "[Desktop Entry]\nType=Application\nName=Test App\nName[de]=Testprogramm\n"
                         "X-AppImage-Version=1.2.3\nIcon=app\nExec=AppRun\n";
  put (dir, "app.desktop", desktop, strlen (desktop));
  put (dir, "usr/share/icons/app.png", g_bytes_get_data (png, nullptr), g_bytes_get_size (png));
  put (dir, "usr/lib/big.bin", g_bytes_get_data (big, nullptr), g_bytes_get_size (big));
  put (dir, "AppRun", "#!/bin/sh\n", 10);
  link_at (dir, ".DirIcon", "usr/share/icons/app.png");
  link_at (dir, "up", "usr/share/../lib/big.bin");
  link_at (dir, "rooted", "/usr/lib/big.bin");     /* the image's top, not this machine's */
  if (hostile)
    {
      link_at (dir, "out", "../../../etc/passwd");
      link_at (dir, "loop1", "loop2");
      link_at (dir, "loop2", "loop1");
    }
  return g_steal_pointer (&dir);
}

static GBytes *
make_image (const char *dir, const char *comp)
{
  g_autofree char *img = g_strdup_printf ("%s.%s.sqfs", dir, comp);
  const char *const argv[] = {
    "mksquashfs", dir,      img,         "-comp",        comp,          "-b", "4096",
    "-noappend",  "-quiet", "-all-root", "-no-progress", "-processors", "1",  nullptr
  };   /* not every core: tests run beside it */
  run (argv);
  char *data;
  size_t len;
  g_assert_true (g_file_get_contents (img, &data, &len, nullptr));
  return g_bytes_new_take (data, len);
}

/* An ELF program (a copy of /bin/true, marked as an AppImage of type 2) with `image`
 * after it. *elf: where the image starts. */
static GBytes *
make_appimage (const uint8_t *img, size_t len_img, size_t *elf)
{
  char *prog;
  size_t len;
  g_assert_true (g_file_get_contents ("/bin/true", &prog, &len, nullptr));
  prog[8] = 'A';
  prog[9] = 'I';
  prog[10] = 2;
  GByteArray *a = g_byte_array_new_take ((uint8_t *) prog, len);
  g_byte_array_append (a, img, (guint) len_img);
  *elf = len;
  return g_byte_array_free_to_bytes (a);
}

static void
test_compressions (void)
{
  g_autofree char *dir = make_tree ("tree", false);
  const char *const comps[] = { "gzip", "zstd", "xz" };
  for (size_t i = 0; i < G_N_ELEMENTS (comps); i++)
    {
      g_autoptr (GBytes) img = make_image (dir, comps[i]);
      g_autoptr (GError) error = nullptr;
      g_autoptr (GbkSquash) s = gbk_squash_open (mem_read, img, 0, g_bytes_get_size (img), &error);
      g_assert_no_error (error);
      if (s == nullptr)
        gbk_fatal ("no image");
      g_autoptr (GPtrArray) names = gbk_squash_root_names (s, &error);
      g_assert_no_error (error);
      g_assert_nonnull (names);
      g_assert_cmpuint (names->len, ==, 6); /* .DirIcon AppRun app.desktop rooted up usr */
      g_autoptr (GBytes) got = gbk_squash_read (s, "usr/lib/big.bin", 1 << 20, &error);
      g_assert_no_error (error);
      g_assert_true (g_bytes_equal (got, big));
      g_autoptr (GBytes) up = gbk_squash_read (s, "up", 1 << 20, &error);
      g_assert_no_error (error);
      g_assert_true (g_bytes_equal (up, big));
      g_autoptr (GBytes) rooted = gbk_squash_read (s, "rooted", 1 << 20, &error);
      g_assert_no_error (error);
      g_assert_true (g_bytes_equal (rooted, big));
      g_autoptr (GBytes) icon = gbk_squash_read (s, ".DirIcon", 1 << 20, &error);
      g_assert_no_error (error);
      g_assert_true (g_bytes_equal (icon, png));
      g_autoptr (GBytes) small = gbk_squash_read (s, "usr/lib/big.bin", 100, &error);
      g_assert_null (small);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
      g_clear_error (&error);
      g_autoptr (GBytes) missing = gbk_squash_read (s, "usr/nothing", 100, &error);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    }
}

static void
test_hostile_links (void)
{
  g_autofree char *dir = make_tree ("hostile", true);
  g_autoptr (GBytes) img = make_image (dir, "gzip");
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkSquash) s = gbk_squash_open (mem_read, img, 0, g_bytes_get_size (img), &error);
  g_assert_no_error (error);
  if (s == nullptr)
    gbk_fatal ("no image");
  const char *const refused[] = { "out", "loop1", "usr", "usr/lib/big.bin/x", "../x" };
  for (size_t i = 0; i < G_N_ELEMENTS (refused); i++)
    {
      g_autoptr (GBytes) b = gbk_squash_read (s, refused[i], 1 << 20, &error);
      if (b != nullptr)
        gbk_fatal ("read %s", refused[i]);
      g_assert_nonnull (error);
      g_clear_error (&error);
    }
}

/* Cut short anywhere, or one byte wrong anywhere in the first part: an error, never more. */
static void
test_damaged (void)
{
  g_autofree char *dir = make_tree ("damaged", false);
  g_autoptr (GBytes) img = make_image (dir, "zstd");
  size_t len;
  const uint8_t *d = g_bytes_get_data (img, &len);
  for (size_t cut = 0; cut < len; cut += 97)
    {
      g_autoptr (GBytes) part = g_bytes_new (d, cut);
      g_autoptr (GError) error = nullptr;
      g_autoptr (GbkSquash) s = gbk_squash_open (mem_read, part, 0, cut, &error);
      if (s == nullptr)
        continue;
      g_autoptr (GPtrArray) names = gbk_squash_root_names (s, &error);
      g_clear_error (&error);
      g_autoptr (GBytes) b = gbk_squash_read (s, ".DirIcon", 1 << 20, &error);
    }
  for (size_t at = 0; at < MIN (len, 4096); at++)
    {
      g_autofree uint8_t *copy = g_memdup2 (d, len);
      copy[at] ^= 0x5a;
      g_autoptr (GBytes) bad = g_bytes_new_take (g_steal_pointer (&copy), len);
      g_autoptr (GError) error = nullptr;
      g_autoptr (GbkSquash) s = gbk_squash_open (mem_read, bad, 0, len, &error);
      if (s == nullptr)
        continue;
      g_autoptr (GBytes) b = gbk_squash_read (s, "usr/lib/big.bin", 1 << 20, &error);
      g_assert_true ((b == nullptr) == (error != nullptr));
    }
}

static char *
write_appimage (const char *name, const uint8_t *data, size_t len)
{
  char *path = g_build_filename (tmp, name, nullptr);
  g_assert_true (g_file_set_contents (path, (const char *) data, (gssize) len, nullptr));
  return path;
}

/* The helper's exit status and all of its output. */
static GBytes *
run_helper (const char *path, const char *size, bool *ok)
{
  const char *helper = g_getenv ("QUOIN_APPIMAGE");
  g_assert_nonnull (helper);
  g_autoptr (GError) error = nullptr;
  g_autoptr (GSubprocess) p
      = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                          &error, helper, path, size, (const char *) nullptr);
  g_assert_no_error (error);
  GBytes *out = nullptr;
  g_assert_true (g_subprocess_communicate (p, nullptr, nullptr, &out, nullptr, &error));
  g_assert_no_error (error);
  *ok = g_subprocess_get_if_exited (p) && g_subprocess_get_exit_status (p) == 0;
  return out;
}

static void
test_appimage (void)
{
  g_autofree char *dir = make_tree ("app", false);
  g_autoptr (GBytes) img = make_image (dir, "zstd");
  size_t elf;
  size_t img_len;
  const uint8_t *img_data = g_bytes_get_data (img, &img_len);
  g_autoptr (GBytes) ai = make_appimage (img_data, img_len, &elf);
  uint64_t offset;
  g_assert_true (gbk_appimage_offset (mem_read, ai, g_bytes_get_size (ai), &offset));
  g_assert_cmpuint (offset, ==, elf);
  GbkAppImage a;
  g_autoptr (GError) error = nullptr;
  g_assert_true (gbk_appimage_read (mem_read, ai, g_bytes_get_size (ai), &a, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (a.name, ==, "Test App");
  g_assert_cmpstr (a.version, ==, "1.2.3");
  g_assert_true (a.icon != nullptr && g_bytes_equal (a.icon, png));
  gbk_appimage_clear (&a);

  /* Not marked as an AppImage: refused, though the image is there. */
  g_autofree uint8_t *plain = g_memdup2 (g_bytes_get_data (ai, nullptr), g_bytes_get_size (ai));
  plain[8] = 0;
  g_autoptr (GBytes) unmarked = g_bytes_new (plain, g_bytes_get_size (ai));
  g_assert_false (gbk_appimage_offset (mem_read, unmarked, g_bytes_get_size (unmarked), &offset));

  /* The helper, for real: the name, the version, and the icon scaled to 48 px. */
  size_t ai_len;
  const uint8_t *ai_data = g_bytes_get_data (ai, &ai_len);
  g_autofree char *path = write_appimage ("Test.AppImage", ai_data, ai_len);
  bool ok;
  g_autoptr (GBytes) out = run_helper (path, "48", &ok);
  g_assert_true (ok);
  size_t out_len;
  const uint8_t *out_data = g_bytes_get_data (out, &out_len);
  GbkAppImageInfo info;
  g_assert_true (gbk_appimage_out_parse (out_data, out_len, &info));
  g_assert_cmpstr (info.name, ==, "Test App");
  g_assert_cmpstr (info.version, ==, "1.2.3");
  g_assert_cmpint (info.width, ==, 48);
  g_assert_cmpint (info.height, ==, 48);
  g_assert_cmphex (info.pixels[47 * 48 + 47], ==, 0xffff0000); /* opaque red */
  g_assert_cmphex (info.pixels[0] >> 24, ==, 0); /* the clear corner */
  gbk_appimage_info_clear (&info);

  /* Not an AppImage: the helper says so (exit 1), and writes nothing. */
  g_autofree char *text = g_build_filename (tmp, "plain.txt", nullptr);
  g_assert_true (g_file_set_contents (text, "hello", -1, nullptr));
  g_autoptr (GBytes) out2 = run_helper (text, "48", &ok);
  g_assert_false (ok);
  g_assert_cmpuint (g_bytes_get_size (out2), ==, 0);
}

static void
test_output_refused (void)
{
  GByteArray *ok = g_byte_array_new ();
  g_byte_array_append (ok, (const uint8_t *) GBK_APPIMAGE_OUT_MAGIC,
                       (guint) strlen (GBK_APPIMAGE_OUT_MAGIC));
  const uint8_t tail[]
      = { 1, 0, 0, 0, 'x', 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0x10, 0x10, 0x10, 0x80 };
  g_byte_array_append (ok, tail, sizeof tail);
  GbkAppImageInfo info;
  g_assert_true (gbk_appimage_out_parse (ok->data, ok->len, &info));
  g_assert_cmpstr (info.name, ==, "x");
  g_assert_null (info.version);
  g_assert_cmpint (info.width, ==, 1);
  gbk_appimage_info_clear (&info);
  struct
  {
    size_t at; /* byte to change, or SIZE_MAX: drop the last one */
    uint8_t to;
  } bad[] = {
    { 0, 'X' }, /* not the magic */
    { 17, 9 }, /* the name's length past the end */
    { 21, 0x01 }, /* a control character in the name */
    { 25, 2 }, /* the version's length: too big */
    { 26, 2 }, /* width 2: too few pixels */
    { 33, 1 }, /* height 2^24 + 1: too big */
    { 34, 0x90 }, /* brighter than its opacity */
    { SIZE_MAX, 0 }, /* cut short */
  };
  for (size_t i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_autofree uint8_t *copy = g_memdup2 (ok->data, ok->len);
      size_t len = ok->len;
      if (bad[i].at == SIZE_MAX)
        len--;
      else
        copy[bad[i].at] = bad[i].to;
      if (gbk_appimage_out_parse (copy, len, &info))
        gbk_fatal ("bad output %zu accepted", i);
    }
  g_byte_array_unref (ok);
}

/* The version in upstream's file names: the user's own nine, and what isn't a version. */
static void
test_version_in_name (void)
{
  const char *const cases[][2] = {
    { "FreeCAD_1.1.3-Linux-x86_64-py311.AppImage", "1.1.3" },
    { "FreeCAD_26.3rc2-Linux-x86_64.AppImage", "26.3rc2" },
    { "OrcaSlicer_Linux_AppImage_Ubuntu2404_V2.5.1.AppImage", "2.5.1" },
    { "Snapmaker_Orca_Linux_AppImage_Ubuntu2404_V2.4.1-alpha.AppImage", "2.4.1-alpha" },
    { "Snapmaker_Orca_Linux_AppImage_Ubuntu2404_V2.4.1.AppImage", "2.4.1" },
    { "arduino-ide_2.3.6_Linux_64bit.AppImage", "2.3.6" },
    { "solidcraft-0.17.0-linux-x86_64.AppImage", "0.17.0" },
    { "solidcraft-0.40.0-rc.2-linux-x86_64.AppImage", "0.40.0-rc.2" },
    { "App-v3.0.BETA1.appimage", "3.0.BETA1" },
    { "App 1.2 nightly.AppImage", "1.2" },
    { "1.0.AppImage", "1.0" },
    { "App-2.0-x86_64", "2.0" },
    { "App-x86_64.AppImage", nullptr },
    { "App-py311-64bit.AppImage", nullptr },
    { "App_Ubuntu24.04.AppImage", nullptr },   /* not at the start of a part */
    { "App-2.3.6x.AppImage", nullptr },
    { "App-1..2.AppImage", nullptr },
    { "App-1.AppImage", nullptr },
    { "App-v.AppImage", nullptr },
    { ".AppImage", nullptr },
    { "", nullptr },
  };
  for (size_t i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *v = gbk_appimage_version_in_name (cases[i][0]);
      if (g_strcmp0 (v, cases[i][1]) != 0)
        gbk_fatal ("%s: %s, not %s", cases[i][0], v, cases[i][1]);
    }
}

/* Read an AppImage made from `dir` (gzip). */
static void
read_tree (const char *dir, GbkAppImage *a)
{
  g_autoptr (GBytes) img = make_image (dir, "gzip");
  size_t img_len, elf;
  const uint8_t *img_data = g_bytes_get_data (img, &img_len);
  g_autoptr (GBytes) ai = make_appimage (img_data, img_len, &elf);
  g_autoptr (GError) error = nullptr;
  if (!gbk_appimage_read (mem_read, ai, g_bytes_get_size (ai), a, &error))
    gbk_fatal ("not read: %s", error->message);
}

/* No X-AppImage-Version: the newest release in its AppStream metadata, if it's sound. */
static void
test_metainfo (void)
{
  const char desktop[] = "[Desktop Entry]\nType=Application\nName=Meta App\nIcon=app\n";
  struct
  {
    const char *file, *xml, *want;
  } cases[] = {
    { "usr/share/metainfo/org.ex.App.metainfo.xml",
      "<?xml version=\"1.0\"?><component><id>org.ex.App</id><releases>"
      "<release version=\"26.3.0\" date=\"2026-10-18\"><description><p>x</p></description>"
      "</release><release version=\"1.0\"/></releases></component>",
      "26.3.0" },
    { "usr/share/metainfo/org.ex.App.appdata.xml",
      "<component><releases><release date=\"1\" version=\"2.4.1\"/></releases></component>",
      "2.4.1" },
    { "usr/share/appdata/org.ex.App.appdata.xml",
      "<component><releases><release version=\"0.9\"/></releases></component>", "0.9" },
    /* Only <component><releases><release>. */
    { "usr/share/metainfo/org.ex.App.metainfo.xml",
      "<component><x><releases><release version=\"6\"/></releases></x>"
      "<release version=\"7\"/></component>",
      nullptr },
    { "usr/share/metainfo/org.ex.App.metainfo.xml",
      "<component><releases></releases><release version=\"7\"/></component>", nullptr },
    /* Broken after the release: still counts; before it: nothing. */
    { "usr/share/metainfo/org.ex.App.metainfo.xml",
      "<component><releases><release version=\"3.1\"/></releases><oops></component>", "3.1" },
    { "usr/share/metainfo/org.ex.App.metainfo.xml", "<component><releases><release version=\"3.1\"",
      nullptr },
    { "usr/share/metainfo/org.ex.App.metainfo.xml",
      "<component><releases><release version=\"a\nb\tc\"/></releases></component>", "a b c" },
    { "usr/share/metainfo/org.ex.App.metainfo.xml", "", nullptr },
    { "usr/share/metainfo/other.metainfo.xml",
      "<component><releases><release version=\"5\"/></releases></component>", nullptr },
  };
  for (size_t i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *leaf = g_strdup_printf ("meta%zu", i);
      g_autofree char *dir = g_build_filename (tmp, leaf, nullptr);
      put (dir, "org.ex.App.desktop", desktop, strlen (desktop));
      put (dir, cases[i].file, cases[i].xml, strlen (cases[i].xml));
      GbkAppImage a;
      read_tree (dir, &a);
      g_assert_cmpstr (a.name, ==, "Meta App");
      if (g_strcmp0 (a.version, cases[i].want) != 0)
        gbk_fatal ("case %zu: %s, not %s", i, a.version, cases[i].want);
      gbk_appimage_clear (&a);
    }

  /* X-AppImage-Version, when there is one, comes first. */
  g_autofree char *dir = g_build_filename (tmp, "meta-both", nullptr);
  const char both[] = "[Desktop Entry]\nName=Meta App\nX-AppImage-Version=1.2.3\n";
  const char xml[] = "<component><releases><release version=\"9\"/></releases></component>";
  put (dir, "org.ex.App.desktop", both, strlen (both));
  put (dir, "usr/share/metainfo/org.ex.App.metainfo.xml", xml, strlen (xml));
  GbkAppImage a;
  read_tree (dir, &a);
  g_assert_cmpstr (a.version, ==, "1.2.3");
  gbk_appimage_clear (&a);
}

static void
rm_rf (const char *path)
{
  g_autoptr (GDir) d = g_dir_open (path, 0, nullptr);
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    {
      g_autofree char *p = g_build_filename (path, name, nullptr);
      struct stat st;
      if (lstat (p, &st) == 0 && S_ISDIR (st.st_mode))
        rm_rf (p);
      else
        unlink (p);
    }
  rmdir (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_autoptr (GError) error = nullptr;
  tmp = g_dir_make_tmp ("gbk-appimage-XXXXXX", &error);
  g_assert_no_error (error);
  GByteArray *b = g_byte_array_new ();
  for (guint i = 0; i < 3 * 4096 + 1234; i++)
    {
      uint8_t c = (uint8_t) (g_test_rand_int () & 0x0f); /* compressible, but not trivially */
      g_byte_array_append (b, &c, 1);
    }
  big = g_byte_array_free_to_bytes (b);
  png = make_png ();
  g_test_add_func ("/appimage/compressions", test_compressions);
  g_test_add_func ("/appimage/hostile-links", test_hostile_links);
  g_test_add_func ("/appimage/damaged", test_damaged);
  g_test_add_func ("/appimage/appimage", test_appimage);
  g_test_add_func ("/appimage/output-refused", test_output_refused);
  g_test_add_func ("/appimage/version-in-name", test_version_in_name);
  g_test_add_func ("/appimage/metainfo", test_metainfo);
  int status = g_test_run ();
  g_bytes_unref (big);
  g_bytes_unref (png);
  rm_rf (tmp);
  g_free (tmp);
  return status;
}
