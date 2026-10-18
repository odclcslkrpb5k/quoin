/* One tab: a toolbar row (path bar), a directory shown in a compact view, and its own
 * history. The path bar belongs to the tab: it shows this tab's folder. */
#pragma once

#include "context.h"
#include "pathbar.h"
#include "view.h"

#define GBK_TYPE_BROWSER (gbk_browser_get_type ())
G_DECLARE_FINAL_TYPE (GbkBrowser, gbk_browser, GBK, BROWSER, GtkBox)

/* Signals:
 *   "path-changed"    the directory changed (navigation, or walking up after it vanished)
 *   "status-changed"  the status text (gbk_browser_status) may have changed */

GbkBrowser *gbk_browser_new (GbkContext *ctx, bool show_hidden, GbkSortMode sort);

void gbk_browser_open (GbkBrowser *b, GbkLoc *loc);    /* records history */
void gbk_browser_back (GbkBrowser *b);
void gbk_browser_forward (GbkBrowser *b);
void gbk_browser_up (GbkBrowser *b);

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
