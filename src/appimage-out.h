/* What quoin-appimage (appimage-main.c) writes, and Quoin's strict reading of it: the
 * helper read an untrusted file, so its output is checked like anything else from outside
 * (lengths, UTF-8, the icon's size, every pixel). Fuzzed. */
#pragma once

#define GBK_APPIMAGE_OUT_MAGIC "QUOIN-APPIMAGE 1\n"
#define GBK_APPIMAGE_OUT_ICON_MAX 256            /* px, either side */
#define GBK_APPIMAGE_OUT_TEXT_MAX 1024           /* bytes, the name and the version */
#define GBK_APPIMAGE_OUT_MAX \
  (sizeof GBK_APPIMAGE_OUT_MAGIC + (size_t) 4 * (2 + GBK_APPIMAGE_OUT_TEXT_MAX) + 8u \
   + (size_t) GBK_APPIMAGE_OUT_ICON_MAX * GBK_APPIMAGE_OUT_ICON_MAX * 4u)

typedef struct
{
  char *name;           /* nullptr when the AppImage has none */
  char *version;        /* nullptr when it has none */
  int width, height;    /* 0 x 0: no icon */
  uint32_t *pixels;     /* width * height ARGB32, premultiplied, native byte order */
} GbkAppImageInfo;

void gbk_appimage_info_clear (GbkAppImageInfo *info);

/* Parse the helper's whole output. False if anything about it is wrong. */
[[nodiscard]] bool gbk_appimage_out_parse (const uint8_t *data, size_t len, GbkAppImageInfo *out);

/* The version in an AppImage's file name, as upstream names them, or nullptr:
 * "FreeCAD_26.3rc2-Linux-x86_64.AppImage" -> "26.3rc2",
 * "Snapmaker_Orca_Linux_AppImage_Ubuntu2404_V2.4.1-alpha.AppImage" -> "2.4.1-alpha".
 * The first part (between '_', '-' or ' ') that is digits with at least one '.', after an
 * optional 'v', with a pre-release mark if one follows (alpha, beta, rc, ...). g_free. */
[[nodiscard]] char *gbk_appimage_version_in_name (const char *name);
