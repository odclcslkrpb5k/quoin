/* One tab: a toolbar row (path bar), a directory shown in a compact view, and its own
 * history. The path bar belongs to the tab: it shows this tab's folder. */
#pragma once

#include "context.h"
#include "pathbar.h"
#include "view.h"

#define GBK_TYPE_BROWSER (gbk_browser_get_type ())
G_DECLARE_FINAL_TYPE (GbkBrowser, gbk_browser, GBK, BROWSER, GtkBox)

/* An info bar under the path bar says when the folder's machine can't be reached, with
 * Retry and, when ssh needs a person, "Log in in a terminal".
 *
 * Signals:
 *   "path-changed"    the directory changed (navigation, or walking up after it vanished)
 *   "status-changed"  the status text (gbk_browser_status) may have changed */

GbkBrowser *gbk_browser_new (GbkContext *ctx, bool show_hidden, GbkSortMode sort);

void gbk_browser_open (GbkBrowser *b, GbkLoc *loc);    /* records history */
/* What a person typed (GbkAddress): on the machine it names, or the one being looked at.
 * Another machine's home folder is known once it has answered: until then the status says
 * "Connecting…", and the tab goes there when it does (or says why it couldn't). */
void gbk_browser_open_address (GbkBrowser *b, const GbkAddress *a);
/* The home folder of the machine being looked at. */
void gbk_browser_home (GbkBrowser *b);
/* A folder on another machine that can't be reached: try again now. */
void gbk_browser_reconnect (GbkBrowser *b);
void gbk_browser_back (GbkBrowser *b);
void gbk_browser_forward (GbkBrowser *b);
void gbk_browser_up (GbkBrowser *b);

/* The tab's back and forward history (GbkLoc*, most recent last): new arrays, to give to
 * another tab with gbk_browser_set_history (reopening a closed one). */
void gbk_browser_history (const GbkBrowser *b, GPtrArray **back, GPtrArray **forward);
void gbk_browser_set_history (GbkBrowser *b, const GPtrArray *back, const GPtrArray *forward);

GbkLoc *gbk_browser_loc (const GbkBrowser *b);
GbkView *gbk_browser_view (const GbkBrowser *b);
GbkPathbar *gbk_browser_pathbar (const GbkBrowser *b);
/* The row above the view that holds the path bar; the window adds its menu button here. */
GtkBox *gbk_browser_toolbar (const GbkBrowser *b);
GbkDirModel *gbk_browser_model (const GbkBrowser *b);

void gbk_browser_set_show_hidden (GbkBrowser *b, bool show);
void gbk_browser_set_sort (GbkBrowser *b, GbkSortMode sort);

/* Select these names as they appear (e.g. just pasted). Gives up after a short while. */
void gbk_browser_select_names (GbkBrowser *b, GPtrArray *names);

/* A transient message shown instead of the usual status, until the selection changes. */
void gbk_browser_set_message (GbkBrowser *b, const char *message);
/* Status bar text. g_free. */
char *gbk_browser_status (GbkBrowser *b);

/* Git (gitwatch.h): the work tree's branch and changes ("main ↑1 · 2 modified"), or why
 * git couldn't say (then gbk_browser_git_failed); nullptr outside a work tree. */
const char *gbk_browser_git_summary (const GbkBrowser *b);
bool gbk_browser_git_failed (const GbkBrowser *b);
const char *gbk_browser_git_upstream (const GbkBrowser *b);   /* or nullptr */
/* Look at the git status again soon (the window is back in use). */
void gbk_browser_refresh_git (const GbkBrowser *b);
