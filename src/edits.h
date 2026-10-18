/* Saving edits back to other machines (docs/remote-plan.md, section 7, user decision 4).
 * A file opened from another machine is a copy here (fetch.h) with a record beside it
 * (edit-store.h). While the app that opened it runs, the copy is watched:
 *
 * - Every second it is looked at; once it has changed and then stayed the same for a second
 *   (the app finished saving, however it saves), it is uploaded. The upload replaces the
 *   file there only if that is still as it was when downloaded or last uploaded; otherwise
 *   the person is asked (Replace, Keep Both, Don't Upload). It never goes to a machine
 *   whose identity (machine-id, user) differs from the one it came from.
 * - Offline, it waits and tries again.
 * - "While the app runs": a process with the app's executable name exists, judged after a
 *   grace period from the launch (apps take a while to start; some hand the file to an
 *   instance already running). If no such process is ever seen, while a Quoin window is
 *   open, and for a while after the last one closes.
 * - Quoin keeps running without windows until every watch has ended (each holds the
 *   application), and when its last window closes it always says so, naming each file
 *   and its app.
 *
 * Nothing is lost if Quoin ends first: a copy changed and not uploaded is offered at the
 * next start (Upload, Show, or Discard). Main thread only. */
#pragma once
#include "context.h"
#include "edit-store.h"

/* Start: `app` is kept running while anything is watched; connections come from `ctx`. Also
 * looks for copies an earlier Quoin left unsent, and removes old finished ones. */
void gbk_edits_init (GtkApplication *app, GbkContext *ctx);
/* Initialized (otherwise remote files open read-only, unwatched). */
bool gbk_edits_ready (void);
/* Stop watching (at exit; copies not uploaded stay, offered next time). */
void gbk_edits_shutdown (void);

/* Watch a copy (its record already saved, its lock held: `lock_fd`, taken over). `asked`:
 * the path there as it was opened (before links were resolved), to find it again.
 * `executable`: the app's executable name to look for (nullptr: unknown). Returns an id for
 * gbk_edits_launched. */
uint64_t gbk_edits_watch (GbkEditRecord *rec, int lock_fd, const char *asked,
                          const char *executable);
/* The app for watch `id` started as process `pid`. */
void gbk_edits_launched (uint64_t id, GPid pid);
/* The copy of `host`:`path` being watched (nullptr: none). g_free. */
[[nodiscard]] char *gbk_edits_find (const char *host, const char *path);
/* How many copies are watched. */
guint gbk_edits_count (void);

/* News about files being edited, for every window's status bar (gbk_window_set_activity);
 * the latest stays until there is newer. "" when there is none. */
void gbk_edits_say (const char *text);
const char *gbk_edits_activity (void);

/* The cache folder for copies (created if needed). g_free. */
[[nodiscard]] char *gbk_edits_cache (void);

/* Look (on a thread) for copies an earlier Quoin left unsent, to offer them, and remove old
 * finished ones. gbk_edits_init does it once. */
void gbk_edits_scan (void);

/* Tests: how often copies are looked at, how long an app has to show up, and how long an
 * unknown app's copy is watched after the last window closes (0: the defaults); and a
 * stand-in for the question about unsent copies (it gets their count and returns the
 * answer, GBK_EDITS_UPLOAD / SHOW / DISCARD). */
void gbk_edits_set_timing_for_test (int poll_ms, int grace_ms, int after_windows_ms);
typedef enum
{
  GBK_EDITS_DISCARD,
  GBK_EDITS_SHOW,
  GBK_EDITS_UPLOAD,
} GbkEditsAnswer;
void gbk_edits_set_unsent_answer_for_test (GbkEditsAnswer (*answer) (guint n));
/* Tests: whether a Quoin window counts as the one in use (1), not (0), or as it really is
 * (-1, the default): notifications go out only when it isn't. */
void gbk_edits_set_in_front_for_test (int in_front);

#define GBK_EDITS_POLL_MS 1000
#define GBK_EDITS_APP_CHECK_MS 5000
#define GBK_EDITS_GRACE_MS 60000
#define GBK_EDITS_AFTER_WINDOWS_MS ((int) 10 * 60 * 1000) /* app unknown: after the last window */
#define GBK_EDITS_KEEP_DAYS 7                       /* finished copies are kept this long */
