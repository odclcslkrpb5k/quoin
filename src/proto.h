/* The wire protocol between Quoin and quoin-serve on another machine (docs/remote-plan.md,
 * section 4). Encoding and decoding only: no I/O, no threads. Fuzzed.
 *
 * The stream starts with one text line from the server, GBK_PROTO_MAGIC " <version>
 * <build>\n", so junk printed by a shell startup file is recognised as such. After it, both
 * directions are frames:
 *
 *   u32 length | u8 type | u64 channel | payload        (little-endian; length counts
 *                                                        type + channel + payload)
 *
 * Everything decoded came from another machine and is untrusted: lengths are checked
 * against what's left, names are validated as filenames, enums against their range, and
 * a payload must be used up exactly. Anything malformed is refused (false / nullptr / -1)
 * and the caller ends the connection; nothing is guessed. */
#pragma once
#include "fileops.h"
#include "scanner.h"

#define GBK_PROTO_MAGIC "QUOIN-SERVE"
/* Wire format version: both ends are always the same build (quoin-serve is pushed by the
 * laptop), so this only has to change with the format, and a mismatch is refused. */
#define GBK_PROTO_VERSION 1
/* Largest frame body (type + channel + payload). */
#define GBK_PROTO_FRAME_MAX (1u << 20)
#define GBK_PROTO_MAGIC_MAX 256         /* longest acceptable magic line */

typedef enum : uint8_t
{
  /* server -> client */
  GBK_MSG_HELLO = 1,      /* channel 0: who the server is (GbkHello) */
  GBK_MSG_SCAN = 2,       /* a watch's GbkScanMsg (a big one arrives as several) */
  GBK_MSG_INFO = 3,       /* answer to GET_INFO: a GbkFileInfo */
  GBK_MSG_FAILED = 4,     /* a request failed: a G_IO_ERROR code and its message */
  GBK_MSG_PONG = 5,
  /* client -> server */
  GBK_MSG_WATCH = 64,     /* start watching a folder (path) on this channel */
  GBK_MSG_UNWATCH = 65,   /* stop it */
  GBK_MSG_GET_INFO = 66,  /* a file's details (path) */
  GBK_MSG_PING = 67,
} GbkMsgType;

typedef struct
{
  GbkMsgType type;
  uint64_t channel;
  const uint8_t *payload; /* borrowed from the input buffer */
  size_t len;
} GbkFrame;

typedef struct
{
  uint32_t version;       /* GBK_PROTO_VERSION */
  char *build;            /* git hash the server was built from */
  char *machine_id;       /* /etc/machine-id, "" if unknown */
  uint32_t uid;
  char *home;             /* absolute */
} GbkHello;

void gbk_hello_clear (GbkHello *h);

/* ---- framing ---- */

/* The magic line at the start of `data`: 1 with *version, *build (g_free) and *used set; 0
 * if the line isn't complete yet; -1 if it isn't ours (or too long). */
int gbk_proto_magic (const uint8_t *data, size_t len, uint32_t *version, char **build,
                     size_t *used);
/* Append the magic line. */
void gbk_proto_put_magic (GByteArray *out, const char *build);

/* The first frame in `data`: 1 with *frame and *used set; 0 if more bytes are needed; -1 if
 * malformed (too long, too short, unknown type). The frame borrows from `data`. */
int gbk_proto_next_frame (const uint8_t *data, size_t len, GbkFrame *frame, size_t *used);

/* ---- writers: each appends whole frames to `out` ---- */

void gbk_proto_put_hello (GByteArray *out, const GbkHello *h);
/* WATCH, GET_INFO: a path. */
void gbk_proto_put_path (GByteArray *out, GbkMsgType type, uint64_t channel, const char *path);
/* UNWATCH, PING, PONG. */
void gbk_proto_put_empty (GByteArray *out, GbkMsgType type, uint64_t channel);
void gbk_proto_put_failed (GByteArray *out, uint64_t channel, const GError *error);
void gbk_proto_put_info (GByteArray *out, uint64_t channel, const GbkFileInfo *info);
/* One or more SCAN frames, each under GBK_PROTO_FRAME_MAX. A split snapshot chunk keeps
 * `first` on its first piece and `last` (and free space) on its last; a split delta sends
 * its removals before its additions, as the model applies them. */
void gbk_proto_put_scan (GByteArray *out, uint64_t channel, const GbkScanMsg *m);

/* ---- readers ---- */

[[nodiscard]] bool gbk_proto_get_hello (const GbkFrame *f, GbkHello *h);
/* An absolute path without NUL bytes, or nullptr. g_free. */
[[nodiscard]] char *gbk_proto_get_path (const GbkFrame *f);
[[nodiscard]] bool gbk_proto_get_empty (const GbkFrame *f);
/* The failure as a G_IO_ERROR in *error (its message made valid UTF-8). */
[[nodiscard]] bool gbk_proto_get_failed (const GbkFrame *f, GError **error);
[[nodiscard]] GbkFileInfo *gbk_proto_get_info (const GbkFrame *f);
/* Entries are built here with the caller's type table, so the remote never decides types,
 * and every name passes gbk_entry_name_valid. */
[[nodiscard]] GbkScanMsg *gbk_proto_get_scan (const GbkFrame *f, const GbkTypeTable *types);
