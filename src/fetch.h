/* Opening files that are on another machine (docs/remote-plan.md, section 7): each is
 * downloaded into a private folder of its own in the cache (edits.h:
 * ~/.cache/quoin/remote/<id>/, 0700), followed through links there, and opened from there
 * like a local file. Saves are uploaded back while the app runs (edits.h). The download is
 * a job, with the usual progress window and errors; the files open when it finishes, if
 * the window that asked is still open. A file already open this way opens its same copy
 * again. */
#pragma once
#include "open.h"

/* Fetch `items` (GbkItem*, files on another machine) and open them with `app`, or with each
 * one's default application when `app` is nullptr. */
void gbk_fetch_open (GtkWindow *parent, GPtrArray *items, GAppInfo *app);
