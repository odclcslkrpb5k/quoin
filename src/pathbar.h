/* Location bar: one button per folder from home (or the filesystem root) to the current
 * one, which is shown pressed. Going up keeps the deeper buttons, so you can go back down
 * with one click (as Thunar does). The pencil button (or Ctrl+L) swaps the buttons for a
 * text entry; Enter goes there, Escape goes back to buttons. */
#pragma once
#include "loc.h"

#define GBK_TYPE_PATHBAR (gbk_pathbar_get_type ())
G_DECLARE_FINAL_TYPE (GbkPathbar, gbk_pathbar, GBK, PATHBAR, GtkBox)

/* Signals (each GbkLoc is borrowed for the emission; ref it to keep it):
 *   "navigate"     (GbkLoc *loc)   a button was clicked
 *   "navigate-address" (const GbkAddress *a)   a location was typed (see loc.h); the
 *                  browser resolves it (another machine's home may need a connection)
 *   "context-menu" (GbkLoc *loc, GdkEvent *trigger)   a button was right-clicked
 *   "drop"         (GbkLoc *loc, GPtrArray *locs, GdkEvent *trigger)   local files were
 *                  dropped on the button for `loc`; nothing has been done with them yet */

GtkWidget *gbk_pathbar_new (void);
void gbk_pathbar_set_loc (GbkPathbar *bar, GbkLoc *loc);
/* Rebuild the buttons (another machine's home became known). */
void gbk_pathbar_refresh (GbkPathbar *bar);
/* Switch to the text entry, focused, with the current path selected. */
void gbk_pathbar_edit (GbkPathbar *bar);
bool gbk_pathbar_editing (GbkPathbar *bar);

/* For tests: the button labels, "|"-separated, with the pressed one in [brackets]. */
char *gbk_pathbar_describe (GbkPathbar *bar);
/* For tests: `path` names a folder on the bar's machine. Click its button. */
void gbk_pathbar_click (GbkPathbar *bar, const char *path);
/* For tests: the button for `path` (on the bar's machine). */
GtkWidget *gbk_pathbar_button (GbkPathbar *bar, const char *path);
GtkEntry *gbk_pathbar_entry (const GbkPathbar *bar);
/* For tests: as if `uri_list` were dropped on the button for `path`. */
void gbk_pathbar_drop_for_test (GbkPathbar *bar, const char *path, const char *uri_list,
                                GdkEvent *trigger);
