/* Opening files that are on another machine (docs/remote-plan.md, section 7): they are
 * copied into a private cache folder, ~/.cache/quoin/remote/<random>/ (0700), made
 * read-only there, and opened from there like local files. The copy is a job, with the
 * usual progress window and errors; the files open when it finishes, if the window that
 * asked is still open. Editing them and saving back to that machine comes later.
 *
 * Cache folders older than GBK_FETCH_KEEP_DAYS are removed in the background at startup. */
#pragma once
#include "open.h"

/* Fetch `items` (GbkItem*, files on another machine) and open them with `app`, or with each
 * one's default application when `app` is nullptr. */
void gbk_fetch_open (GtkWindow *parent, GPtrArray *items, GAppInfo *app);

/* The cache folder (created if needed). g_free. */
[[nodiscard]] char *gbk_fetch_cache_dir (void);
/* Remove cache folders older than `days`, on a thread of its own. */
void gbk_fetch_cleanup_start (int days);

#define GBK_FETCH_KEEP_DAYS 7
