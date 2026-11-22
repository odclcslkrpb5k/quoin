/* quoin-appimage: reads an AppImage's name, version and icon for Quoin's Icons view, in a
 * process of its own, so that a hostile file can at worst end this process (Quoin then
 * shows the generic icon). Never runs the AppImage. It limits itself before reading
 * anything: memory, CPU time, no core dump, no new privileges. Icons are decoded by
 * gdk-pixbuf, PNG and SVG only; on systems with glycin that happens in sandboxed decoder
 * processes of glycin's own.
 *
 *   quoin-appimage PATH SIZE    describe PATH on stdout, its icon at most SIZE px square
 *   quoin-appimage --version
 *
 * Output (to Quoin on the same machine): GBK_APPIMAGE_OUT_MAGIC, then u32 little-endian
 * lengths each followed by their bytes: the name, the version (UTF-8, either may be
 * empty), then u32 width and height (0 for no icon) and width * height ARGB32 pixels,
 * premultiplied, in this machine's byte order (cairo's format). Exit status 0: it is an
 * AppImage; 1: it isn't, or can't be read; 2: bad arguments. */
#include "appimage-out.h"
#include "appimage.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sys/prctl.h>
#include <sys/resource.h>

#define MEMORY_MAX (UINT64_C (512) << 20)

/* AddressSanitizer reserves terabytes of address space up front, so the memory limit
 * can't apply to the sanitizer builds (tests); release builds have it. */
#if defined(__SANITIZE_ADDRESS__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SANITIZED 1
#endif
#endif
#define CPU_SECONDS 10

static void
limit (__rlimit_resource_t resource, rlim_t value)
{
  struct rlimit r = { value, value };
  if (setrlimit (resource, &r) != 0)
    {
      fprintf (stderr, "quoin-appimage: can't limit itself: %s\n", g_strerror (errno));
      exit (1);
    }
}

static bool
read_at (void *ctx, uint64_t offset, void *buf, size_t len)
{
  int fd = *(const int *) ctx;
  size_t done = 0;
  while (done < len)
    {
      ssize_t n = pread (fd, (uint8_t *) buf + done, len - done, (off_t) (offset + done));
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      done += (size_t) n;
    }
  return true;
}

/* PNG or SVG, by their first bytes: the only kinds decoded. */
static bool
known_image (GBytes *b)
{
  size_t len = g_bytes_get_size (b);
  const char *d = g_bytes_get_data (b, nullptr);
  static const char png[] = "\x89PNG\r\n\x1a\n";
  if (len >= 8 && memcmp (d, png, 8) == 0)
    return true;
  /* SVG: its root element early on (after an XML declaration, comments, a doctype). */
  return g_strstr_len (d, (gssize) MIN (len, 1024), "<svg") != nullptr;
}

/* The icon as premultiplied ARGB32, at most `size` px square, or nullptr. */
static GdkPixbuf *
decode (GBytes *icon, int size)
{
  if (icon == nullptr || !known_image (icon))
    return nullptr;
  g_autoptr (GInputStream) in = g_memory_input_stream_new_from_bytes (icon);
  g_autoptr (GError) error = nullptr;
  GdkPixbuf *p = gdk_pixbuf_new_from_stream_at_scale (in, size, size, true, nullptr, &error);
  if (p == nullptr)
    fprintf (stderr, "quoin-appimage: the icon can't be read: %s\n", error->message);
  return p;
}

static void
put_u32 (GByteArray *out, uint32_t v)
{
  const uint8_t b[4]
      = { (uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24) };
  g_byte_array_append (out, b, 4);
}

static void
put_text (GByteArray *out, const char *s)
{
  size_t len = s != nullptr ? strlen (s) : 0;
  put_u32 (out, (uint32_t) len);
  g_byte_array_append (out, (const uint8_t *) (s != nullptr ? s : ""), (guint) len);
}

/* RGBA (straight alpha, any rowstride) to ARGB32 premultiplied, as native 32-bit words. */
static void
put_pixels (GByteArray *out, const GdkPixbuf *p)
{
  int w = gdk_pixbuf_get_width (p), h = gdk_pixbuf_get_height (p);
  int n = gdk_pixbuf_get_n_channels (p), stride = gdk_pixbuf_get_rowstride (p);
  bool alpha = gdk_pixbuf_get_has_alpha (p);
  const uint8_t *px = gdk_pixbuf_read_pixels (p);
  put_u32 (out, (uint32_t) w);
  put_u32 (out, (uint32_t) h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      {
        const uint8_t *s = px + (size_t) y * (size_t) stride + (size_t) x * (size_t) n;
        uint32_t a = alpha ? s[3] : 255;
        uint32_t r = s[0] * a / 255, g = s[1] * a / 255, b = s[2] * a / 255;
        uint32_t word = (a << 24) | (r << 16) | (g << 8) | b;
        g_byte_array_append (out, (const uint8_t *) &word, 4);
      }
}

static bool
write_all (const GByteArray *out)
{
  size_t done = 0;
  while (done < out->len)
    {
      ssize_t n = write (STDOUT_FILENO, out->data + done, out->len - done);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      done += (size_t) n;
    }
  return true;
}

int
main (int argc, char **argv)
{
  if (argc == 2 && g_str_equal (argv[1], "--version"))
    {
      printf ("quoin-appimage %s\n", GBK_VERSION);
      return 0;
    }
  uint64_t size;
  if (argc != 3
      || !g_ascii_string_to_unsigned (argv[2], 10, 1, GBK_APPIMAGE_OUT_ICON_MAX, &size, nullptr))
    {
      fprintf (stderr, "usage: quoin-appimage PATH SIZE (1 to %d)\n", GBK_APPIMAGE_OUT_ICON_MAX);
      return 2;
    }
  /* Before anything is read: what a hostile file could make this process do is bounded. */
  if (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    {
      fprintf (stderr, "quoin-appimage: can't limit itself: %s\n", g_strerror (errno));
      return 1;
    }
  limit (RLIMIT_CORE, 0);
  /* No RLIMIT_FSIZE: gdk-pixbuf decodes through glycin, which hands images to its own
   * sandboxed decoder processes in memory files, and writes to those count. */
  limit (RLIMIT_CPU, CPU_SECONDS);
#ifndef SANITIZED
  limit (RLIMIT_AS, MEMORY_MAX);
#endif

  int fd = open (argv[1], O_RDONLY | O_CLOEXEC | O_NOCTTY);
  struct stat st;
  if (fd < 0 || fstat (fd, &st) != 0 || !S_ISREG (st.st_mode))
    {
      fprintf (stderr, "quoin-appimage: can't read it: %s\n", g_strerror (errno));
      return 1;
    }
  GbkAppImage a;
  g_autoptr (GError) error = nullptr;
  if (!gbk_appimage_read (read_at, &fd, (uint64_t) st.st_size, &a, &error))
    {
      fprintf (stderr, "quoin-appimage: %s\n", error->message);
      gbk_appimage_clear (&a);
      return 1;
    }
  close (fd);
  g_autoptr (GdkPixbuf) icon = decode (a.icon, (int) size);
  g_autoptr (GByteArray) out = g_byte_array_new ();
  g_byte_array_append (out, (const uint8_t *) GBK_APPIMAGE_OUT_MAGIC,
                       (guint) strlen (GBK_APPIMAGE_OUT_MAGIC));
  put_text (out, a.name);
  put_text (out, a.version);
  if (icon != nullptr && gdk_pixbuf_get_n_channels (icon) >= 3
      && gdk_pixbuf_get_bits_per_sample (icon) == 8)
    put_pixels (out, icon);
  else
    {
      put_u32 (out, 0);
      put_u32 (out, 0);
    }
  gbk_appimage_clear (&a);
  return write_all (out) ? 0 : 1;
}
