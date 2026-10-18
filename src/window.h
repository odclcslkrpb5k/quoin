/* A browser window: tabs (one GbkBrowser each, with its path bar), status bar. Actions apply to the
 * active tab; show-hidden and sort apply window-wide. */
#pragma once

#include "browser.h"
#include "devices.h"
#include "pathbar.h"

#define GBK_TYPE_WINDOW (gbk_window_get_type ())
G_DECLARE_FINAL_TYPE (GbkWindow, gbk_window, GBK, WINDOW, GtkApplicationWindow)

GbkWindow *gbk_window_new (const GtkApplication *app, GbkContext *ctx, GbkLoc *loc);
/* Keyboard shortcuts for every window of `app`. Call once. */
void gbk_window_install_accels (GtkApplication *app);

/* Navigate the active tab to `loc`, recording history. */
void gbk_window_open (GbkWindow *w, GbkLoc *loc);
void gbk_window_new_tab (GbkWindow *w, GbkLoc *loc);
/* What a person typed (see gbk_browser_open_address). */
void gbk_window_open_address (GbkWindow *w, const GbkAddress *a);
GbkLoc *gbk_window_loc (GbkWindow *w);
GbkView *gbk_window_view (GbkWindow *w);
const char *gbk_window_status (GbkWindow *w);
/* The status bar's other line: news about files being edited on other machines ("Saved
 * notes.txt to ws1 at 14:02"); "" hides it. edits.c keeps every window's the same. */
void gbk_window_set_activity (GbkWindow *w, const char *text);
const char *gbk_window_activity (const GbkWindow *w);
/* Right side of the status bar: the sort button (label "↑Name" etc.; click cycles name ->
 * modified -> size) and the ".*" hidden-files toggle. */
GtkButton *gbk_window_sort_button (const GbkWindow *w);
/* The whole status bar (right-click: the folder's menu, as on empty space). */
GtkWidget *gbk_window_status_bar (const GbkWindow *w);
GtkToggleButton *gbk_window_hidden_button (const GbkWindow *w);
int gbk_window_n_tabs (GbkWindow *w);
GbkLoc *gbk_window_tab_loc (GbkWindow *w, int i); /* tab `i`'s folder */
GbkPathbar *gbk_window_pathbar (GbkWindow *w);
/* The ☰ menu's Devices list. It starts following the system this long after the window
 * opens (the first folder comes first). */
GbkDevices *gbk_window_devices (const GbkWindow *w);
#define GBK_WINDOW_DEVICES_DELAY_MS 300
