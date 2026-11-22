/* AppImages' own icons and names for the Icons view, found in the background (a worker
 * thread per view) and cached:
 *
 *   ~/.cache/thumbnails/large/<md5 of its URI>.png   the icon, 256 px at most, as the
 *       freedesktop.org thumbnail specification has it (Thumb::URI, Thumb::MTime), so
 *       other programs can use it and we can use theirs
 *   ~/.cache/quoin/appimage/<md5 of its URI>.ini     its name and version, or that it
 *       couldn't be read; both for the file as it was (mtime and size)
 *
 * What isn't cached comes from the quoin-appimage helper (appimage-main.c), a process of
 * its own that never runs the AppImage; its output is checked (appimage-out.h). Until an
 * answer comes, and whenever anything fails, the view shows the generic icon and the
 * file's name: failing safe. A file that couldn't be read isn't tried again until it
 * changes, or another Quoin comes (the cache holds only for the Quoin that wrote it).
 * The label's version: the one in the file's name if it has one (upstream's names are
 * the more exact), else the one the AppImage declares. */
#pragma once
#include <cairo.h>

/* Is it named like an AppImage (".AppImage", any case)? */
bool gbk_app_icons_named (const char *name);

typedef struct
{
  char *name;              /* the file's raw name, as asked */
  int64_t mtime_ns;        /* as asked */
  char *label;             /* "FreeCAD 1.1.3"; nullptr: show the file name */
  cairo_surface_t *icon;   /* `px` square; nullptr: the generic icon */
} GbkAppIcon;

void gbk_app_icon_free (GbkAppIcon *a);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkAppIcon, gbk_app_icon_free)

typedef struct GbkAppIcons GbkAppIcons;

[[nodiscard]] GbkAppIcons *gbk_app_icons_new (void);
/* Readable while answers are waiting. */
int gbk_app_icons_wake_fd (const GbkAppIcons *a);
/* Find `name` in folder `dir` (on this machine) as it is now (mtime, size); its icon `px`
 * square. Answered once, in order. */
void gbk_app_icons_want (GbkAppIcons *a, const char *dir, const char *name, int64_t mtime_ns,
                         uint64_t size, int px);
/* The next answer, or nullptr (then the wake fd is reset). */
[[nodiscard]] GbkAppIcon *gbk_app_icons_take (GbkAppIcons *a);
/* Stop (the helper, if running, is killed) and release the caller's reference. */
void gbk_app_icons_stop (GbkAppIcons *a);

/* Tests: helper runs started, ever; workers not yet exited. */
int gbk_app_icons_helper_runs (void);
int gbk_app_icons_live_workers (void);

#define GBK_APP_ICONS_THUMB_PX 256      /* the thumbnail specification's "large" */
#define GBK_APP_ICONS_TIMEOUT_MS 15000  /* one helper run */
