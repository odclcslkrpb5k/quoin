/* The contents of one directory, kept live, filtered and sorted. Main thread only.
 *
 * Entries are referred to by GbkHandle (slot + generation). A handle to a removed entry
 * stops resolving (gbk_dir_model_get returns nullptr); it can never reach a different file.
 * Hold handles across main-loop iterations, never GbkEntry pointers: an entry pointer is
 * only valid until the model next changes. */
#pragma once

#include "entry.h"
#include "loc.h"
#include "scanner.h"

typedef uint64_t GbkHandle;
#define GBK_HANDLE_NONE ((GbkHandle) 0)

typedef enum
{
  GBK_SORT_NAME,       /* natural, case-insensitive; folders first */
  GBK_SORT_MTIME,      /* newest first; folders first; ties by name */
  GBK_SORT_SIZE,       /* largest first; folders first, by name (no meaningful size) */
} GbkSortMode;

typedef enum
{
  GBK_DIR_LOADING,     /* first snapshot still arriving (entries may already be visible) */
  GBK_DIR_READY,
  GBK_DIR_ERROR,       /* couldn't be read: see gbk_dir_model_error() */
  GBK_DIR_GONE,        /* deleted, moved or unmounted; entries are the last known state */
  /* On another machine, and the connection is down: entries are the last known state,
   * and it reconnects by itself (after a while, when the network comes back, or on
   * gbk_dir_model_reconnect). Not final, unlike ERROR and GONE: a new listing replaces
   * the old one in place, so handles to entries still there keep resolving. */
  GBK_DIR_DISCONNECTED,
} GbkDirState;

#define GBK_TYPE_DIR_MODEL (gbk_dir_model_get_type ())
G_DECLARE_FINAL_TYPE (GbkDirModel, gbk_dir_model, GBK, DIR_MODEL, GObject)

/* Signals:
 *   "changed"        the visible list or some entry changed
 *   "state-changed"  gbk_dir_model_state() changed */
[[nodiscard]] GbkDirModel *gbk_dir_model_new (GbkLoc *loc, GbkTypeTable *types);

const GbkLoc *gbk_dir_model_loc (const GbkDirModel *m); /* nullptr when detached */
GbkDirState gbk_dir_model_state (const GbkDirModel *m);
int gbk_dir_model_error (const GbkDirModel *m);          /* errno value in GBK_DIR_ERROR */
bool gbk_dir_model_live (const GbkDirModel *m);          /* live updates active */
/* On another machine: reconnect now if DISCONNECTED (Reload, the window coming back). */
void gbk_dir_model_reconnect (GbkDirModel *m);
/* DISCONNECTED: why (one line), and whether logging in once in a terminal may fix it. */
const char *gbk_dir_model_conn_error (const GbkDirModel *m);
bool gbk_dir_model_needs_login (const GbkDirModel *m);
/* Still connecting to the folder's machine. */
bool gbk_dir_model_connecting (const GbkDirModel *m);
/* Free space on the directory's filesystem, if known yet. */
bool gbk_dir_model_free_space (const GbkDirModel *m, uint64_t *bytes);

void gbk_dir_model_set_sort (GbkDirModel *m, GbkSortMode mode);
GbkSortMode gbk_dir_model_sort (const GbkDirModel *m);
void gbk_dir_model_set_show_hidden (GbkDirModel *m, bool show);
bool gbk_dir_model_show_hidden (const GbkDirModel *m);

size_t gbk_dir_model_n_entries (const GbkDirModel *m);   /* including hidden */
size_t gbk_dir_model_n_visible (GbkDirModel *m);
/* `i` >= n_visible is a fatal programming error. */
GbkHandle gbk_dir_model_visible_handle (GbkDirModel *m, size_t i);
/* Position in the visible list, or SIZE_MAX if hidden/stale. O(1). */
size_t gbk_dir_model_visible_index (GbkDirModel *m, GbkHandle h);

/* nullptr if the handle is stale. Valid until the model next changes. */
const GbkEntry *gbk_dir_model_get (GbkDirModel *m, GbkHandle h);
GbkHandle gbk_dir_model_find (GbkDirModel *m, const char *raw_name);

/* Test hooks. A detached model has no scanner; feed it messages with apply_for_test (takes
 * ownership), which applies them exactly as if they had arrived from a worker. */
[[nodiscard]] GbkDirModel *gbk_dir_model_new_detached (GbkTypeTable *types);
void gbk_dir_model_apply_for_test (GbkDirModel *m, GbkScanMsg *msg);
/* Test hook: rebuild the visible order from scratch and check it equals the current
 * (possibly incrementally maintained) order. Fatal on mismatch. */
void gbk_dir_model_check_order (GbkDirModel *m);
