/* What an AppImage says about itself, read without running it: its name, version and icon,
 * from the .desktop file and .DirIcon at the top of the squashfs image inside it (an
 * AppImage of type 2 is an ELF program with the image appended). Untrusted input:
 * squashfs.h's limits apply, and the icon comes back as undecoded bytes (decoding it is
 * the quoin-appimage helper's job, in a process of its own). Fuzzed. */
#pragma once
#include "squashfs.h"

typedef struct
{
  char *name;           /* the .desktop file's Name (valid UTF-8), or nullptr */
  char *version;        /* its X-AppImage-Version, else the newest release in its
                  * AppStream metadata (usr/share/metainfo), or nullptr */
  GBytes *icon;         /* .DirIcon's bytes (PNG or SVG, as shipped), or nullptr */
} GbkAppImage;

void gbk_appimage_clear (GbkAppImage *a);

/* Where the image starts in an AppImage of `size` bytes: after the ELF program. False if
 * it isn't an AppImage of type 2. */
[[nodiscard]] bool gbk_appimage_offset (GbkReadAt read, void *ctx, uint64_t size, uint64_t *offset);

/* Read what the AppImage says. True when it is one, even if some parts are missing. */
[[nodiscard]] bool gbk_appimage_read (GbkReadAt read, void *ctx, uint64_t size, GbkAppImage *out,
                                      GError **error);

#define GBK_APPIMAGE_DESKTOP_MAX ((size_t) 64 * 1024)
#define GBK_APPIMAGE_ICON_MAX ((size_t) 4 * 1024 * 1024)
#define GBK_APPIMAGE_METAINFO_MAX ((size_t) 256 * 1024)
#define GBK_APPIMAGE_TEXT_MAX 200                  /* name and version, in characters */
