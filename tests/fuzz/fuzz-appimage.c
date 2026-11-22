/* Input: arbitrary bytes as an AppImage, as a bare squashfs image, and as quoin-appimage's
 * output, and as a file name. Each reader must refuse what it can't use, within its limits. */
#include "appimage-out.h"
#include "appimage.h"
#include "fuzz.h"

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

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  g_autoptr (GBytes) b = g_bytes_new_static (data, size);

  GbkAppImage a;
  g_autoptr (GError) error = nullptr;
  if (gbk_appimage_read (mem_read, b, size, &a, &error))
    {
      g_assert (a.name == nullptr || g_utf8_validate (a.name, -1, nullptr));
      g_assert (a.version == nullptr || g_utf8_validate (a.version, -1, nullptr));
      g_assert (a.icon == nullptr || g_bytes_get_size (a.icon) <= GBK_APPIMAGE_ICON_MAX);
    }
  else
    g_assert (error != nullptr);
  gbk_appimage_clear (&a);
  g_clear_error (&error);

  g_autoptr (GbkSquash) s = gbk_squash_open (mem_read, b, 0, size, &error);
  if (s != nullptr)
    {
      g_autoptr (GPtrArray) names = gbk_squash_root_names (s, &error);
      g_clear_error (&error);
      for (guint i = 0; names != nullptr && i < names->len && i < 16; i++)
        {
          g_autoptr (GBytes) f = gbk_squash_read (s, g_ptr_array_index (names, i), 1 << 20, &error);
          g_assert ((f == nullptr) == (error != nullptr));
          g_assert (f == nullptr || g_bytes_get_size (f) <= (1 << 20));
          g_clear_error (&error);
        }
    }

  /* As a file name. */
  g_autofree char *name = g_strndup ((const char *) data, MIN (size, 4096));
  g_autofree char *version = gbk_appimage_version_in_name (name);
  g_assert (version == nullptr || (version[0] != '\0' && strstr (name, version) != nullptr));

  GbkAppImageInfo info;
  if (gbk_appimage_out_parse (data, size, &info))
    {
      g_assert (info.width >= 0 && info.width <= GBK_APPIMAGE_OUT_ICON_MAX);
      g_assert ((info.width == 0) == (info.pixels == nullptr));
      gbk_appimage_info_clear (&info);
    }
  return 0;
}
