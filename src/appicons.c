#include "appicons.h"
#include "appimage-out.h"
#include "capture.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <poll.h>
#include <sys/eventfd.h>

#define POP_MS 200            /* how often a waiting worker looks whether it should stop */

typedef struct
{
  char *dir, *name;
  int64_t mtime_ns;
  uint64_t size;
  int px;
} Want;

struct GbkAppIcons
{
  gatomicrefcount rc;      /* one for the view, one for the worker */
  GAsyncQueue *wants;      /* Want* */
  GAsyncQueue *answers;    /* GbkAppIcon* */
  int wake_fd;             /* eventfd: worker -> main */
  int cancel_fd;           /* eventfd: main -> worker (and to the helper's run) */
  bool started;
};

static int helper_runs;
static int live_workers;

int
gbk_app_icons_helper_runs (void)
{
  return g_atomic_int_get (&helper_runs);
}

int
gbk_app_icons_live_workers (void)
{
  return g_atomic_int_get (&live_workers);
}

bool
gbk_app_icons_named (const char *name)
{
  size_t len = strlen (name);
  return len > 9 && g_ascii_strcasecmp (name + len - 9, ".AppImage") == 0;
}

void
gbk_app_icon_free (GbkAppIcon *a)
{
  if (a == nullptr)
    return;
  g_free (a->name);
  g_free (a->label);
  if (a->icon != nullptr)
    cairo_surface_destroy (a->icon);
  g_free (a);
}

static void
want_free (Want *w)
{
  g_free (w->dir);
  g_free (w->name);
  g_free (w);
}

static void
icons_unref (GbkAppIcons *a)
{
  if (!g_atomic_ref_count_dec (&a->rc))
    return;
  g_async_queue_unref (a->wants);
  g_async_queue_unref (a->answers);
  close (a->wake_fd);
  close (a->cancel_fd);
  g_free (a);
}

static void
signal_fd (int fd)
{
  uint64_t one = 1;
  while (write (fd, &one, sizeof one) < 0 && errno == EINTR)
    ;
}

static bool
cancelled (const GbkAppIcons *a)
{
  struct pollfd p = { .fd = a->cancel_fd, .events = POLLIN };
  return poll (&p, 1, 0) > 0;
}

/* ---- the caches ---- */

static char *
cache_key (const char *path)
{
  g_autofree char *uri = g_filename_to_uri (path, nullptr, nullptr);
  return uri != nullptr ? g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1) : nullptr;
}

static char *
thumb_path (const char *key)
{
  g_autofree char *file = g_strconcat (key, ".png", nullptr);
  return g_build_filename (g_get_user_cache_dir (), "thumbnails", "large", file, nullptr);
}

static char *
ini_path (const char *key)
{
  g_autofree char *file = g_strconcat (key, ".ini", nullptr);
  return g_build_filename (g_get_user_cache_dir (), "quoin", "appimage", file, nullptr);
}

/* Write `data` to `path` whole, or not at all (a temporary file, renamed over it). */
static void
save_atomically (const char *path, const char *data, size_t len)
{
  g_autofree char *dir = g_path_get_dirname (path);
  g_autoptr (GError) error = nullptr;
  if (g_mkdir_with_parents (dir, 0700) != 0)
    g_debug ("AppImage cache not written: %s", g_strerror (errno));
  else if (!g_file_set_contents_full (path, data, (gssize) len, G_FILE_SET_CONTENTS_CONSISTENT,
                                      0600, &error))
    g_debug ("AppImage cache not written: %s", error->message);
}

/* "Name Version", the name alone, or nullptr for neither. */
static char *
label_of (const char *name, const char *version)
{
  if (name == nullptr)
    return nullptr;
  if (version == nullptr || strstr (name, version) != nullptr)
    return g_strdup (name);
  return g_strdup_printf ("%s %s", name, version);
}

/* `src` (ARGB32 premultiplied), fitted into a `px` square, centred. */
static cairo_surface_t *
fit (cairo_surface_t *src, int px)
{
  int w = cairo_image_surface_get_width (src), h = cairo_image_surface_get_height (src);
  if (w <= 0 || h <= 0)
    return nullptr;
  cairo_surface_t *dst = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, px, px);
  cairo_t *cr = cairo_create (dst);
  double scale = MIN ((double) px / w, (double) px / h);
  cairo_translate (cr, (px - w * scale) / 2, (px - h * scale) / 2);
  cairo_scale (cr, scale, scale);
  cairo_set_source_surface (cr, src, 0, 0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
  cairo_paint (cr);
  cairo_destroy (cr);
  return dst;
}

/* A pixbuf (RGB or RGBA, 8 bits) as an ARGB32 surface. */
static cairo_surface_t *
surface_of_pixbuf (const GdkPixbuf *p)
{
  int w = gdk_pixbuf_get_width (p), h = gdk_pixbuf_get_height (p);
  int n = gdk_pixbuf_get_n_channels (p), in_stride = gdk_pixbuf_get_rowstride (p);
  if (gdk_pixbuf_get_bits_per_sample (p) != 8 || n < 3 || w <= 0 || h <= 0
      || w > GBK_APP_ICONS_THUMB_PX * 4 || h > GBK_APP_ICONS_THUMB_PX * 4)
    return nullptr;
  bool alpha = gdk_pixbuf_get_has_alpha (p);
  const uint8_t *in = gdk_pixbuf_read_pixels (p);
  cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  cairo_surface_flush (s);
  uint8_t *out = cairo_image_surface_get_data (s);
  int out_stride = cairo_image_surface_get_stride (s);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        const uint8_t *px = in + (size_t) y * (size_t) in_stride + (size_t) x * (size_t) n;
        uint32_t a = alpha ? px[3] : 255;
        uint32_t word
            = (a << 24) | (px[0] * a / 255) << 16 | (px[1] * a / 255) << 8 | (px[2] * a / 255);
        memcpy (out + (size_t) y * (size_t) out_stride + (size_t) x * 4, &word, 4);
      }
  cairo_surface_mark_dirty (s);
  return s;
}

/* The helper's pixels as a surface. */
static cairo_surface_t *
surface_of_info (const GbkAppImageInfo *info)
{
  cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, info->width, info->height);
  cairo_surface_flush (s);
  uint8_t *out = cairo_image_surface_get_data (s);
  int stride = cairo_image_surface_get_stride (s);
  for (int y = 0; y < info->height; y++)
    memcpy (out + (size_t) y * (size_t) stride, info->pixels + (size_t) y * (size_t) info->width,
            (size_t) info->width * 4);
  cairo_surface_mark_dirty (s);
  return s;
}

/* The thumbnail as the specification has it: PNG, straight alpha, with where it is from. */
static void
save_thumbnail (const char *key, const char *path, int64_t mtime_s, uint64_t size,
                const GbkAppImageInfo *info)
{
  size_t n = (size_t) info->width * (size_t) info->height;
  g_autofree uint8_t *rgba = g_malloc (n * 4);
  for (size_t i = 0; i < n; i++)
    {
      uint32_t px = info->pixels[i], a = px >> 24;
      uint8_t *o = rgba + i * 4;
      o[0] = (uint8_t) (a > 0 ? ((px >> 16) & 0xff) * 255 / a : 0);
      o[1] = (uint8_t) (a > 0 ? ((px >> 8) & 0xff) * 255 / a : 0);
      o[2] = (uint8_t) (a > 0 ? (px & 0xff) * 255 / a : 0);
      o[3] = (uint8_t) a;
    }
  g_autoptr (GdkPixbuf) p
      = gdk_pixbuf_new_from_data (rgba, GDK_COLORSPACE_RGB, true, 8, info->width, info->height,
                                  info->width * 4, nullptr, nullptr);
  g_autofree char *uri = g_filename_to_uri (path, nullptr, nullptr);
  g_autofree char *mtime = g_strdup_printf ("%" G_GINT64_FORMAT, mtime_s);
  g_autofree char *bytes = g_strdup_printf ("%" G_GUINT64_FORMAT, size);
  g_autofree char *png = nullptr;
  size_t len;
  g_autoptr (GError) error = nullptr;
  if (uri == nullptr
      || !gdk_pixbuf_save_to_buffer (p, &png, &len, "png", &error, "tEXt::Thumb::URI", uri,
                                     "tEXt::Thumb::MTime", mtime, "tEXt::Thumb::Size", bytes,
                                     "tEXt::Software", "Quoin", (const char *) nullptr))
    return;
  g_autofree char *target = thumb_path (key);
  save_atomically (target, png, len);
}

/* A cached thumbnail for this file as it is now, as a surface; nullptr if there's none. */
static cairo_surface_t *
load_thumbnail (const char *key, const char *path, int64_t mtime_s)
{
  g_autofree char *file = thumb_path (key);
  g_autoptr (GdkPixbuf) p = gdk_pixbuf_new_from_file (file, nullptr);
  if (p == nullptr)
    return nullptr;
  const char *mtime = gdk_pixbuf_get_option (p, "tEXt::Thumb::MTime");
  const char *uri = gdk_pixbuf_get_option (p, "tEXt::Thumb::URI");
  g_autofree char *want_uri = g_filename_to_uri (path, nullptr, nullptr);
  int64_t t;
  if (mtime == nullptr || uri == nullptr || want_uri == nullptr || !g_str_equal (uri, want_uri)
      || !g_ascii_string_to_signed (mtime, 10, G_MININT64, G_MAXINT64, &t, nullptr) || t != mtime_s)
    return nullptr;
  return surface_of_pixbuf (p);
}

/* ---- finding one ---- */

static char *
helper_path (void)
{
  const char *env = g_getenv ("QUOIN_APPIMAGE");
  if (env != nullptr)
    return g_strdup (env);
  g_autofree char *self = g_file_read_link ("/proc/self/exe", nullptr);
  if (self == nullptr)
    return nullptr;
  g_autofree char *dir = g_path_get_dirname (self);
  return g_build_filename (dir, "quoin-appimage", nullptr);
}

/* Ask the helper; true with *info when it answered. */
static bool
ask_helper (const GbkAppIcons *a, const char *path, GbkAppImageInfo *info)
{
  g_autofree char *helper = helper_path ();
  if (helper == nullptr)
    return false;
  g_autofree char *px = g_strdup_printf ("%d", GBK_APP_ICONS_THUMB_PX);
  const char *const argv[] = { helper, path, px, nullptr };
  g_atomic_int_inc (&helper_runs);
  GbkCaptured c;
  g_autoptr (GError) error = nullptr;
  if (!gbk_capture (argv, nullptr, nullptr, a->cancel_fd, GBK_APP_ICONS_TIMEOUT_MS,
                    GBK_APPIMAGE_OUT_MAX, &c, &error))
    {
      g_debug ("no AppImage details: %s", error->message);
      return false;
    }
  bool ok = WIFEXITED (c.status) && WEXITSTATUS (c.status) == 0
            && gbk_appimage_out_parse (c.out->data, c.out->len, info);
  gbk_captured_clear (&c);
  return ok;
}

static GbkAppIcon *
find (const GbkAppIcons *a, const Want *w)
{
  GbkAppIcon *r = g_new0 (GbkAppIcon, 1);
  r->name = g_strdup (w->name);
  r->mtime_ns = w->mtime_ns;
  g_autofree char *path = g_build_filename (w->dir, w->name, nullptr);
  g_autofree char *key = cache_key (path);
  if (key == nullptr)
    return r;
  int64_t mtime_s = w->mtime_ns / 1000000000;
  g_autofree char *ini = ini_path (key);
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  if (g_key_file_load_from_file (kf, ini, G_KEY_FILE_NONE, nullptr)
      && g_key_file_get_int64 (kf, "AppImage", "MTime", nullptr) == w->mtime_ns
      && g_key_file_get_uint64 (kf, "AppImage", "Size", nullptr) == w->size)
    {
      g_autofree char *reader = g_key_file_get_string (kf, "AppImage", "Reader", nullptr);
      /* Not again until it changes, or another Quoin (which may read it) comes. */
      if (g_key_file_get_boolean (kf, "AppImage", "Failed", nullptr)
          && g_strcmp0 (reader, GBK_VERSION) == 0)
        return r;
      g_autofree char *name = g_key_file_get_string (kf, "AppImage", "Name", nullptr);
      g_autofree char *version = g_key_file_get_string (kf, "AppImage", "Version", nullptr);
      bool has_icon = g_key_file_get_boolean (kf, "AppImage", "Icon", nullptr);
      cairo_surface_t *thumb = has_icon ? load_thumbnail (key, path, mtime_s) : nullptr;
      if (!has_icon || thumb != nullptr)
        {
          r->label = label_of (name, version);
          if (thumb != nullptr)
            {
              r->icon = fit (thumb, w->px);
              cairo_surface_destroy (thumb);
            }
          return r;
        }
      /* The icon's thumbnail has gone: ask again. */
    }

  GbkAppImageInfo info;
  g_autoptr (GKeyFile) out = g_key_file_new ();
  g_key_file_set_int64 (out, "AppImage", "MTime", w->mtime_ns);
  g_key_file_set_uint64 (out, "AppImage", "Size", w->size);
  g_key_file_set_string (out, "AppImage", "Reader", GBK_VERSION);
  if (!ask_helper (a, path, &info))
    {
      if (!cancelled (a))
        {
          g_key_file_set_boolean (out, "AppImage", "Failed", true);
          size_t len;
          g_autofree char *text = g_key_file_to_data (out, &len, nullptr);
          save_atomically (ini, text, len);
        }
      return r;
    }
  if (info.name != nullptr)
    g_key_file_set_string (out, "AppImage", "Name", info.name);
  if (info.version != nullptr)
    g_key_file_set_string (out, "AppImage", "Version", info.version);
  g_key_file_set_boolean (out, "AppImage", "Icon", info.width > 0);
  if (info.width > 0)
    {
      save_thumbnail (key, path, mtime_s, w->size, &info);
      cairo_surface_t *full = surface_of_info (&info);
      r->icon = fit (full, w->px);
      cairo_surface_destroy (full);
    }
  size_t len;
  g_autofree char *text = g_key_file_to_data (out, &len, nullptr);
  save_atomically (ini, text, len);
  r->label = label_of (info.name, info.version);
  gbk_appimage_info_clear (&info);
  return r;
}

/* ---- the worker ---- */

static void *
worker (void *data)
{
  GbkAppIcons *a = data;
  while (!cancelled (a))
    {
      Want *w = g_async_queue_timeout_pop (a->wants, (guint64) POP_MS * 1000);
      if (w == nullptr)
        continue;
      GbkAppIcon *r = find (a, w);
      want_free (w);
      if (cancelled (a))
        {
          gbk_app_icon_free (r);
          break;
        }
      g_async_queue_push (a->answers, r);
      signal_fd (a->wake_fd);
    }
  g_atomic_int_dec_and_test (&live_workers);
  icons_unref (a);
  return nullptr;
}

GbkAppIcons *
gbk_app_icons_new (void)
{
  GbkAppIcons *a = g_new0 (GbkAppIcons, 1);
  g_atomic_ref_count_init (&a->rc);
  a->wants = g_async_queue_new_full ((GDestroyNotify) want_free);
  a->answers = g_async_queue_new_full ((GDestroyNotify) gbk_app_icon_free);
  a->wake_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  a->cancel_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (a->wake_fd < 0 || a->cancel_fd < 0)
    gbk_fatal ("eventfd: %s", g_strerror (errno)); /* fd exhaustion: nothing sane to do */
  return a;
}

int
gbk_app_icons_wake_fd (const GbkAppIcons *a)
{
  return a->wake_fd;
}

void
gbk_app_icons_want (GbkAppIcons *a, const char *dir, const char *name, int64_t mtime_ns,
                    uint64_t size, int px)
{
  if (!a->started)
    {
      /* The first request starts the worker: views that never show an AppImage cost nothing. */
      a->started = true;
      g_atomic_ref_count_inc (&a->rc);
      g_atomic_int_inc (&live_workers);
      g_autoptr (GError) error = nullptr;
      GThread *t = g_thread_try_new ("gbk-appimage", worker, a, &error);
      if (t == nullptr)
        {
          g_warning ("no thread for AppImage icons: %s", error->message);
          g_atomic_int_dec_and_test (&live_workers);
          icons_unref (a);
          return;
        }
      g_thread_unref (t);
    }
  Want *w = g_new0 (Want, 1);
  w->dir = g_strdup (dir);
  w->name = g_strdup (name);
  w->mtime_ns = mtime_ns;
  w->size = size;
  w->px = CLAMP (px, 1, GBK_APP_ICONS_THUMB_PX * 4);
  g_async_queue_push (a->wants, w);
}

GbkAppIcon *
gbk_app_icons_take (GbkAppIcons *a)
{
  GbkAppIcon *r = g_async_queue_try_pop (a->answers);
  if (r == nullptr)
    {
      uint64_t n;
      while (read (a->wake_fd, &n, sizeof n) < 0 && errno == EINTR)
        ;
      r = g_async_queue_try_pop (a->answers); /* one that came meanwhile */
    }
  return r;
}

void
gbk_app_icons_stop (GbkAppIcons *a)
{
  if (a == nullptr)
    return;
  signal_fd (a->cancel_fd);
  icons_unref (a);
}
