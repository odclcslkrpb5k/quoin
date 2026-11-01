/* Drag and drop, shared by the file view and the path bar.
 *
 * Out: Quoin windows (this process or another) get the locations themselves, on any
 * machine (x-special/quoin-locations, as the clipboard does). Other apps get files as
 * text/uri-list and plain text paths. Files on another machine go to other apps as copies
 * downloaded here (Konqueror's way, never sftp://): the download starts with the drag, so
 * it's usually done by the drop; a drop before then gets nothing and the status bar says
 * to drop again. Only files, up to GBK_DND_FETCH_MAX in all: folders and bigger drags give
 * other apps just host:/path text. A download no other app took is removed when the drag
 * ends; ones taken stay in the cache (edits.h) until its 7-day clean-up.
 * The other app decides what to do with them. If it asks us to delete the originals after
 * a "move" (drag-data-delete), we don't: we never delete files on another program's say.
 *
 * In: Quoin's locations or text/uri-list with local files, and never acted on by
 * ourselves: the window asks Move Here / Copy Here / Cancel at the pointer. The drag is
 * finished as "not moved" (del = false) whatever the choice, since we do the work
 * ourselves. */
#pragma once
#include "loc.h"

enum
{
  GBK_DND_LOCS,
  GBK_DND_URIS,
  GBK_DND_TEXT
};

#define GBK_DND_FETCH_MAX (UINT64_C (64) << 20)

/* What we accept: Quoin's locations first, then text/uri-list. */
[[nodiscard]] GtkTargetList *gbk_dnd_dest_targets (void);
#define GBK_DND_ACTIONS (GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK | GDK_ACTION_ASK)

/* The action to report while hovering a drop target: we'll ask, so ASK if the source
 * offers it, else COPY, else MOVE; 0 if none (no drop). */
GdkDragAction gbk_dnd_action (GdkDragContext *ctx);

/* The locations in dropped data, as GbkLoc* (from Quoin: on any machine; from other apps:
 * local files only, others skipped). Never nullptr. */
[[nodiscard]] GPtrArray *gbk_dnd_locs (GtkSelectionData *sd);

/* A drag out, from its start to its end. */
typedef struct GbkDragOut GbkDragOut;

/* Drag `locs` (GbkLoc*, referenced; all in one folder). `fetchable`: they are files whose
 * sizes add up to at most GBK_DND_FETCH_MAX, so if they are on another machine their
 * download for other apps starts now. */
[[nodiscard]] GbkDragOut *gbk_drag_out_new (const GPtrArray *locs, bool fetchable);
/* What it offers. */
[[nodiscard]] GtkTargetList *gbk_drag_out_targets (const GbkDragOut *d);
/* The drag data in format `info`; nullptr if it can't be had (yet). */
[[nodiscard]] char *gbk_drag_out_data (GbkDragOut *d, guint info);
/* Fill drag data in format `info`; nothing set if it can't be had (yet). */
void gbk_drag_out_get (GbkDragOut *d, GtkSelectionData *sd, guint info);
/* The drag ended: a download nobody took is stopped and removed. */
void gbk_drag_out_free (GbkDragOut *d);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkDragOut, gbk_drag_out_free)

/* Tests: whatever other apps get is ready (a download has finished with every file). */
[[nodiscard]] bool gbk_drag_out_ready (const GbkDragOut *d);
