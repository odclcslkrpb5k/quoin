/* Compact view: icon + name items flowing top-to-bottom in columns, columns left-to-right,
 * scrolling horizontally. (Thunar's "Compact view" at the smallest zoom; the only view.)
 *
 * Holds model handles only, never entry pointers, across main-loop iterations. Selection
 * and cursor survive re-sorts and live updates; handles that stop resolving drop out. */
#pragma once

#include "dirmodel.h"

#define GBK_TYPE_VIEW (gbk_view_get_type ())
G_DECLARE_FINAL_TYPE (GbkView, gbk_view, GBK, VIEW, GtkWidget)

/* Signals:
 *   "activate"          (no args) user double-clicked or pressed Enter: act on selection
 *   "selection-changed" (no args)
 *   "context-menu"      (GdkEvent *trigger, may be nullptr for keyboard) right-click or
 *                       Menu key. Selection already reflects the target: empty selection
 *                       means the current directory itself.
 *   "drop"              (GPtrArray *locs, guint64 folder, GdkEvent *trigger) local files
 *                       were dropped; `folder` is the GbkHandle of the folder item they
 *                       were dropped on, or GBK_HANDLE_NONE for the view's own folder.
 *                       Nothing has been done with them: the handler decides (asks). */

GtkWidget *gbk_view_new (GbkTypeTable *types);

/* Takes a reference. nullptr clears. Resets cursor and selection. */
void gbk_view_set_model (GbkView *v, GbkDirModel *model);
GbkDirModel *gbk_view_get_model (const GbkView *v);

GbkHandle gbk_view_cursor (const GbkView *v);
/* Moves the cursor to `h` (if visible), selects only it, scrolls it into view. */
void gbk_view_select_only (GbkView *v, GbkHandle h);
void gbk_view_select_all (GbkView *v);
/* Select exactly the visible ones of `handles`; the cursor goes to the first in view order. */
void gbk_view_set_selection (GbkView *v, const GbkHandle *handles, size_t n);
void gbk_view_unselect_all (GbkView *v);
size_t gbk_view_n_selected (GbkView *v);
/* Selected handles in visible order. g_free the result. */
GbkHandle *gbk_view_selected (GbkView *v, size_t *n);
bool gbk_view_is_selected (GbkView *v, GbkHandle h);
/* Names in this folder that are open for editing (from another machine, edits.h): drawn
 * with an emblem. A set of char* (referenced); nullptr: none. */
void gbk_view_set_marked (GbkView *v, GHashTable *names);
bool gbk_view_is_marked (const GbkView *v, GbkHandle h);

/* Keyboard-style cursor movement, used by the key handler and by tests. */
typedef enum
{
  GBK_MOVE_ITEM,       /* count = +-1: down/up within the flow */
  GBK_MOVE_COLUMN,     /* count = +-1: right/left one column */
  GBK_MOVE_PAGE,       /* count = +-1: one screenful of columns */
  GBK_MOVE_ENDS,       /* count < 0: first item; > 0: last item */
} GbkMoveStep;

typedef enum
{
  GBK_SELECT_REPLACE,  /* plain: select just the new cursor item */
  GBK_SELECT_EXTEND,   /* Shift: select the range from the anchor */
  GBK_SELECT_NONE,     /* Ctrl: move the cursor only */
} GbkSelectMode;

void gbk_view_move_cursor (GbkView *v, GbkMoveStep step, int count, GbkSelectMode mode);
void gbk_view_toggle_cursor (GbkView *v);            /* Space */

/* Type-ahead: append text to the search buffer and jump to the first visible item whose
 * name starts with the buffer (case-insensitive). Returns false if nothing matches. */
bool gbk_view_type_ahead (GbkView *v, const char *utf8);
const char *gbk_view_type_ahead_text (GbkView *v);   /* "" when idle */

/* Layout introspection for tests. */
size_t gbk_view_rows_per_column (const GbkView *v);
GbkHandle gbk_view_item_at (GbkView *v, double x, double y);   /* widget coordinates */

/* Drag and drop (see dnd.h). What a drag of the current selection carries (uri-list). */
[[nodiscard]] char *gbk_view_drag_uris (GbkView *v);
/* Would a drop at (x, y) be accepted, and where (*folder: an item, or NONE for the view's
 * folder)? `from_self`: the drag started in this view. */
bool gbk_view_drop_target_at (GbkView *v, double x, double y, bool from_self, GbkHandle *folder);
/* Tests: as if `uri_list` were dropped at (x, y) from another app. */
bool gbk_view_drop_for_test (GbkView *v, const char *uri_list, double x, double y,
                             GdkEvent *trigger);
