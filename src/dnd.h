/* Drag and drop, shared by the file view and the path bar.
 *
 * Out: files go as text/uri-list (any app) and as plain text paths (terminals, editors).
 * The other app decides what to do with them. If it asks us to delete the originals after
 * a "move" (drag-data-delete), we don't: we never delete files on another program's say.
 *
 * In: we only take text/uri-list with local files, and never act on a drop by ourselves:
 * the window asks Move Here / Copy Here / Cancel at the pointer. The drag is finished as
 * "not moved" (del = false) whatever the choice, since we do the work ourselves. */
#pragma once
#include "loc.h"

enum
{
  GBK_DND_URIS,
  GBK_DND_TEXT
};

/* What we offer when dragging out. */
[[nodiscard]] GtkTargetList *gbk_dnd_source_targets (void);
/* What we accept: text/uri-list only. */
[[nodiscard]] GtkTargetList *gbk_dnd_dest_targets (void);
#define GBK_DND_ACTIONS (GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK)

/* The action to report while hovering a drop target: we'll ask, so ASK if the source
 * offers it, else COPY, else MOVE; 0 if none (no drop). */
GdkDragAction gbk_dnd_action (GdkDragContext *ctx);

/* The local files in dropped data, as GbkLoc* (non-local URIs skipped). Never nullptr. */
[[nodiscard]] GPtrArray *gbk_dnd_locs (GtkSelectionData *sd);

/* Fill drag data for `locs` (GbkLoc*) in format `info`. Other apps get file:// URIs and
 * paths, so only files on this machine can go: returns false, setting nothing, if any
 * isn't. */
bool gbk_dnd_set_data (GtkSelectionData *sd, guint info, const GPtrArray *locs);
