/* Records of other machines' files being edited here (docs/remote-plan.md, section 7). Each
 * opened file is a copy in the cache, with a record and a lock beside its folder:
 *
 *   ~/.cache/quoin/remote/<id>/<name>   the copy the app edits
 *   ~/.cache/quoin/remote/<id>.ini      where it came from; the state there and here at the
 *                                       last download or upload
 *   ~/.cache/quoin/remote/<id>.lock     flock'ed by the process watching it
 *
 * The copy is "dirty" when it differs from the state recorded at the last download or
 * upload: a save not uploaded yet, wherever it happened (also after Quoin ended). Plain file
 * work, any thread. */
#pragma once
#include "fileops.h"

typedef struct
{
  char *id;
  char *name;             /* the copy's file name (bytes) */
  char *host;             /* the ssh destination it came from */
  char *machine_id;       /* the machine that answered then */
  uint32_t uid;           /* and the user there */
  char *remote;           /* the file there, links resolved */
  char *app;              /* what it was opened with, for people ("Mousepad"); "" unknown */
  GbkFileToken base;      /* the file there at the last download or upload */
  GbkFileToken synced;    /* the copy here then (or when an upload was declined) */
} GbkEditRecord;

void gbk_edit_record_free (GbkEditRecord *r);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkEditRecord, gbk_edit_record_free)

/* The copy's folder and the copy. g_free. */
[[nodiscard]] char *gbk_edit_record_dir (const char *cache, const GbkEditRecord *r);
[[nodiscard]] char *gbk_edit_record_copy (const char *cache, const GbkEditRecord *r);

/* Write the record (atomically: a crash leaves the old one or the new one). */
[[nodiscard]] bool gbk_edit_record_save (const char *cache, const GbkEditRecord *r, GError **error);
/* Read <id>.ini. */
[[nodiscard]] GbkEditRecord *gbk_edit_record_load (const char *cache, const char *id,
                                                   GError **error);

/* The copy's state now in *now; true if it differs from `synced` (saved since). A copy that
 * is gone (deleted, or mid-way through an app's save) is not dirty: false, *now zeroed. */
bool gbk_edit_record_dirty (const char *cache, const GbkEditRecord *r, GbkFileToken *now);

/* Take <id>.lock without waiting: its descriptor (keep it open to keep the lock), or -1
 * if another process holds it. */
[[nodiscard]] int gbk_edit_lock (const char *cache, const char *id);

/* Remove the copy's folder, record and lock (the caller holds the lock). */
void gbk_edit_record_remove (const char *cache, const char *id);
