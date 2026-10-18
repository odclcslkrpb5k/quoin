/* Directory scanner: one detached worker thread per watched directory.
 *
 * The worker owns every filesystem call (open, readdir, statx, inotify), so the main thread
 * never blocks on a slow or hung filesystem. It sends self-contained messages (owned copies)
 * through a queue; the wake fd becomes readable when messages are waiting.
 *
 * Sequence: CHUNK(first) .. CHUNK(last) = one full snapshot; then any number of DELTAs.
 * A rescan (inotify overflow) sends another full CHUNK sequence. ERROR or GONE is final.
 *
 * Stopping never blocks: the worker is told to cancel and cleans up on its own whenever its
 * current syscall returns. */
#pragma once

#include "entry.h"

typedef enum
{
  GBK_SCAN_CHUNK,   /* part of a full snapshot: upserts only */
  GBK_SCAN_DELTA,   /* incremental change: upserts and removes */
  GBK_SCAN_ERROR,   /* directory can't be read; `error` is an errno value */
  GBK_SCAN_GONE,    /* directory deleted, moved, or unmounted */
} GbkScanMsgType;

typedef struct
{
  GbkScanMsgType type;
  bool first;          /* CHUNK: first of a snapshot */
  bool last;           /* CHUNK: last of a snapshot */
  bool live;           /* CHUNK: live updates (inotify) are active */
  int error;           /* ERROR */
  bool has_free;       /* CHUNK(last), DELTA: free_bytes is valid */
  uint64_t free_bytes; /* space available to unprivileged users on this filesystem */
  GPtrArray *upserts;  /* GbkEntry*, owned; never nullptr */
  GPtrArray *removes;  /* char* raw names, owned; never nullptr */
} GbkScanMsg;

/* An empty message of `type` (no entries, no names). */
[[nodiscard]] GbkScanMsg *gbk_scan_msg_new (GbkScanMsgType type);
void gbk_scan_msg_free (GbkScanMsg *m);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkScanMsg, gbk_scan_msg_free)

typedef struct GbkScanner GbkScanner;

/* Starts the worker. `types` is referenced, not copied. */
[[nodiscard]] GbkScanner *gbk_scanner_start (const char *path, GbkTypeTable *types);

/* Readable while messages are (or may be) queued. Call gbk_scanner_ack() before popping. */
int gbk_scanner_wake_fd (const GbkScanner *s);
void gbk_scanner_ack (const GbkScanner *s);
/* Next message, or nullptr if the queue is empty. Caller owns it. */
[[nodiscard]] GbkScanMsg *gbk_scanner_pop (GbkScanner *s);
/* Make the wake fd readable again (used when the consumer stops early with work left). */
void gbk_scanner_rearm (const GbkScanner *s);

/* Cancel the worker and release the caller's reference. `s` must not be used afterwards. */
void gbk_scanner_stop (GbkScanner *s);

/* Worker threads that haven't exited yet (tests wait for 0). */
int gbk_scanner_live_workers (void);

/* A worker for `root` or a folder under it hasn't exited yet, so it may still hold that
 * folder open (which makes unmounting fail as busy). */
bool gbk_scanner_busy_under (const char *root);

/* `path` is `root` or inside it, by name ("/a/bc" is not inside "/a/b"). Absolute paths. */
bool gbk_path_is_under (const char *path, const char *root);

/* Test hooks: lower the pending-name cap (forces the rescan path); count rescans. */
void gbk_scanner_set_pending_max_for_test (unsigned n);
/* Workers keep their folder open this long after finishing (a slow device). */
void gbk_scanner_set_release_delay_for_test (unsigned ms);
int gbk_scanner_rescans (void);

/* Tunables, exposed for tests. */
#define GBK_SCAN_FIRST_CHUNK 256      /* small first chunk: fast first paint */
#define GBK_SCAN_CHUNK_MAX 8192
#define GBK_SCAN_QUIET_MS 10          /* coalesce events until this long without new ones */
#define GBK_SCAN_MAX_DELAY_MS 50      /* ...but never hold an event longer than this */
#define GBK_SCAN_PENDING_MAX 4096     /* more distinct changed names than this: rescan */
