/* Opening files that are on another machine (docs/remote-plan.md, section 7): each is
 * downloaded into a private folder of its own in the cache (edits.h:
 * ~/.cache/quoin/remote/<id>/, 0700), followed through links there, and opened from there
 * like a local file. Saves are uploaded back while the app runs (edits.h). The download is
 * a job, with the usual progress window and errors; the files open when it finishes, if
 * the window that asked is still open. A file already open this way opens its same copy
 * again. */
#pragma once
#include "job.h"
#include "open.h"

/* Fetch `items` (GbkItem*, files on another machine) and open them with `app`, or with each
 * one's default application when `app` is nullptr. */
void gbk_fetch_open (GtkWindow *parent, GPtrArray *items, GAppInfo *app);

/* Record a downloaded copy of `orig` (`got`, in its own folder `dir` in the cache) and
 * watch it for saves (edits.h). `app` names the app for people ("" unknown), `executable`
 * is the program to look for (nullptr unknown); `dropped`: dropped onto some app, which
 * isn't known (gbk_edits_watch). False if it can't be watched (edits not set up, or the
 * machine's identity unknown): see gbk_fetch_read_only. */
[[nodiscard]] bool gbk_fetch_watch (const GbkLoc *orig, const GbkFetched *got, const char *dir,
                                    const char *app, const char *executable, bool dropped,
                                    uint64_t *id);
/* The copy of `loc` (another machine's file) being edited here, or nullptr. g_free. */
[[nodiscard]] char *gbk_fetch_open_copy (const GbkLoc *loc);
/* An unwatched copy is only for looking at: nobody's saves would reach the other machine,
 * so it is made read-only. */
void gbk_fetch_read_only (const char *path);
