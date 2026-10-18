/* One directory entry, immutable once built. A single allocation holds the struct and all
 * of its strings. Built on the scanner thread, then owned by the directory model. */
#pragma once

#include "filetype.h"

enum : uint8_t
{
  GBK_ENTRY_HIDDEN = 1 << 0,   /* leading '.' or trailing '~' */
  GBK_ENTRY_SYMLINK = 1 << 1,   /* the entry itself is a symlink (kind is its target) */
  GBK_ENTRY_EXECUTABLE = 1 << 2,   /* regular file with any exec bit */
  GBK_ENTRY_STAT_FAILED = 1 << 3,   /* statx failed; kind from d_type, times/size unknown */
};

typedef struct
{
  int64_t mtime_ns;
  uint64_t size;
  uint32_t name_len;      /* raw name, bytes */
  uint32_t key_len;       /* sort key, bytes */
  uint32_t display_off;   /* offset of display name in data (0 = same as raw name) */
  uint32_t key_off;       /* offset of sort key in data */
  GbkTypeId type;
  GbkFileKind kind;
  uint8_t flags;
  char data[];            /* name \0 [display \0] key \0 */
} GbkEntry;

static inline const char *
gbk_entry_name (const GbkEntry *e)
{
  return e->data;
}

/* Valid UTF-8 for showing to the user; invalid bytes in the raw name are escaped. */
static inline const char *
gbk_entry_display (const GbkEntry *e)
{
  return e->data + e->display_off;
}

static inline const char *
gbk_entry_key (const GbkEntry *e)
{
  return e->data + e->key_off;
}

static inline bool
gbk_entry_is_dir (const GbkEntry *e)
{
  return e->kind == GBK_KIND_DIRECTORY;
}

typedef struct
{
  GbkFileKind kind;       /* after following symlinks */
  bool symlink;
  bool stat_failed;
  uint32_t mode;          /* st_mode permission bits of the target */
  int64_t mtime_ns;
  uint64_t size;
} GbkEntryInfo;

/* Pure constructor. `name` must be a valid filename component: 1..255 bytes, no '/' or NUL,
 * not "." or "..". Returns nullptr for an invalid name. */
[[nodiscard]] GbkEntry *gbk_entry_new (const char *name, size_t name_len, const GbkEntryInfo *info,
                                       const GbkTypeTable *types);

/* statx `name` relative to `dirfd` (following a symlink for the target's kind).
 * `d_type` (DT_*) is the fallback kind if statx fails. Returns nullptr if the entry no
 * longer exists (ENOENT/ENOTDIR). */
[[nodiscard]] GbkEntry *gbk_entry_new_at (int dirfd, const char *name, unsigned char d_type,
                                          const GbkTypeTable *types);

void gbk_entry_free (GbkEntry *e);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkEntry, gbk_entry_free)
