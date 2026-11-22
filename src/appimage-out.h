/* What quoin-appimage (appimage-main.c) writes, and Quoin's strict reading of it: the
 * helper read an untrusted file, so its output is checked like anything else from outside
 * (lengths, UTF-8, the icon's size, every pixel). Fuzzed. */
#pragma once

#define GBK_APPIMAGE_OUT_MAGIC "QUOIN-APPIMAGE 1\n"
#define GBK_APPIMAGE_OUT_ICON_MAX 256            /* px, either side */
#define GBK_APPIMAGE_OUT_TEXT_MAX 1024           /* bytes, the name and the version */
#define GBK_APPIMAGE_OUT_MAX \
  (sizeof GBK_APPIMAGE_OUT_MAGIC + 4u * (2u + GBK_APPIMAGE_OUT_TEXT_MAX) + 8u \
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
