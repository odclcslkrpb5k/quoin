#include "proto.h"

#define HEADER_BODY 9u                 /* type + channel */
#define NAME_MAX_BYTES 255u
#define PATH_MAX_BYTES 4095u
#define BUILD_MAX 64u
#define MACHINE_ID_MAX 64u
#define USER_NAME_MAX 255u
#define MESSAGE_MAX 1024u
#define IO_ERROR_CODE_MAX 100u            /* GIOErrorEnum values are well under this */

/* Scan payload, fixed part: type, flags, free bytes, error, removal count, entry count. */
#define SCAN_FIXED (1u + 1u + 8u + 4u + 4u + 4u)
#define SCAN_BUDGET (GBK_PROTO_FRAME_MAX - HEADER_BODY - SCAN_FIXED)

enum : uint8_t
{
  SCAN_FIRST = 1 << 0,
  SCAN_LAST = 1 << 1,
  SCAN_LIVE = 1 << 2,
  SCAN_HAS_FREE = 1 << 3,
  SCAN_FLAGS = SCAN_FIRST | SCAN_LAST | SCAN_LIVE | SCAN_HAS_FREE,
};

enum : uint8_t
{
  ENTRY_SYMLINK = 1 << 0,
  ENTRY_STAT_FAILED = 1 << 1,
  ENTRY_EXEC = 1 << 2,
  ENTRY_FLAGS = ENTRY_SYMLINK | ENTRY_STAT_FAILED | ENTRY_EXEC,
};

enum : uint8_t
{
  INFO_SYMLINK = 1 << 0,
  INFO_DANGLING = 1 << 1,
  INFO_HAS_BTIME = 1 << 2,
  INFO_HAS_OWNER = 1 << 3,
  INFO_HAS_GROUP = 1 << 4,
  INFO_HAS_TARGET = 1 << 5,
  INFO_FLAGS = INFO_SYMLINK | INFO_DANGLING | INFO_HAS_BTIME | INFO_HAS_OWNER | INFO_HAS_GROUP
               | INFO_HAS_TARGET,
};

void
gbk_hello_clear (GbkHello *h)
{
  g_free (h->build);
  g_free (h->machine_id);
  g_free (h->home);
  h->build = h->machine_id = h->home = nullptr;
}

/* ---- writing ---- */

static void
put_u8 (GByteArray *out, uint8_t v)
{
  g_byte_array_append (out, &v, 1);
}

static void
put_le (GByteArray *out, uint64_t v, unsigned bytes)
{
  uint8_t b[8] = { 0 };
  if (bytes > sizeof b)
    gbk_fatal ("protocol: a %u-byte integer", bytes);
  for (unsigned i = 0; i < bytes; i++)
    b[i] = (uint8_t) (v >> (8 * i));
  g_byte_array_append (out, b, bytes);
}

static void
put_u16 (GByteArray *out, uint16_t v)
{
  put_le (out, v, 2);
}

static void
put_u32 (GByteArray *out, uint32_t v)
{
  put_le (out, v, 4);
}

static void
put_u64 (GByteArray *out, uint64_t v)
{
  put_le (out, v, 8);
}

static void
put_i64 (GByteArray *out, int64_t v)
{
  put_le (out, (uint64_t) v, 8);
}

/* A string of at most `max` bytes (the writer's own data: longer is a bug of ours). */
static void
put_str16 (GByteArray *out, const char *s, size_t max)
{
  size_t len = strlen (s);
  if (len > max || len > UINT16_MAX)
    gbk_fatal ("protocol: string of %zu bytes over its limit of %zu", len, max);
  put_u16 (out, (uint16_t) len);
  g_byte_array_append (out, (const guint8 *) s, (guint) len);
}

static size_t
begin_frame (GByteArray *out, GbkMsgType type, uint64_t channel)
{
  size_t start = out->len;
  put_u32 (out, 0);                    /* patched by end_frame */
  put_u8 (out, type);
  put_u64 (out, channel);
  return start;
}

static void
end_frame (GByteArray *out, size_t start)
{
  size_t body = out->len - start - 4;
  if (body > GBK_PROTO_FRAME_MAX)
    gbk_fatal ("protocol: frame of %zu bytes over the limit", body);
  for (unsigned i = 0; i < 4; i++)
    out->data[start + i] = (uint8_t) (body >> (8 * i));
}

/* A message cut at a character boundary to fit MESSAGE_MAX. */
static void
put_message (GByteArray *out, const char *msg)
{
  size_t len = strlen (msg);
  if (len > MESSAGE_MAX)
    len = (size_t) (g_utf8_find_prev_char (msg, msg + MESSAGE_MAX + 1) - msg);
  put_u16 (out, (uint16_t) len);
  g_byte_array_append (out, (const guint8 *) msg, (guint) len);
}

/* A GError as a G_IO_ERROR code (another domain: FAILED). */
static uint32_t
io_code (const GError *error)
{
  bool io = error->domain == G_IO_ERROR && error->code >= 0
            && (unsigned) error->code < IO_ERROR_CODE_MAX;
  return io ? (uint32_t) error->code : (uint32_t) G_IO_ERROR_FAILED;
}

void
gbk_proto_put_magic (GByteArray *out, const char *build)
{
  g_autofree char *line = g_strdup_printf ("%s %d %s\n", GBK_PROTO_MAGIC, GBK_PROTO_VERSION, build);
  g_byte_array_append (out, (const guint8 *) line, (guint) strlen (line));
}

void
gbk_proto_put_hello (GByteArray *out, const GbkHello *h)
{
  size_t f = begin_frame (out, GBK_MSG_HELLO, 0);
  put_u32 (out, h->version);
  put_str16 (out, h->build, BUILD_MAX);
  put_str16 (out, h->machine_id, MACHINE_ID_MAX);
  put_u32 (out, h->uid);
  put_str16 (out, h->home, PATH_MAX_BYTES);
  end_frame (out, f);
}

void
gbk_proto_put_path (GByteArray *out, GbkMsgType type, uint64_t channel, const char *path)
{
  size_t f = begin_frame (out, type, channel);
  put_str16 (out, path, PATH_MAX_BYTES);
  end_frame (out, f);
}

void
gbk_proto_put_empty (GByteArray *out, GbkMsgType type, uint64_t channel)
{
  end_frame (out, begin_frame (out, type, channel));
}

void
gbk_proto_put_failed (GByteArray *out, uint64_t channel, const GError *error)
{
  size_t f = begin_frame (out, GBK_MSG_FAILED, channel);
  put_u32 (out, io_code (error));
  put_message (out, error->message);
  end_frame (out, f);
}

void
gbk_proto_put_info (GByteArray *out, uint64_t channel, const GbkFileInfo *info)
{
  size_t f = begin_frame (out, GBK_MSG_INFO, channel);
  uint8_t flags = (info->symlink ? INFO_SYMLINK : 0) | (info->dangling ? INFO_DANGLING : 0)
                  | (info->has_btime ? INFO_HAS_BTIME : 0)
                  | (info->owner != nullptr ? INFO_HAS_OWNER : 0)
                  | (info->group != nullptr ? INFO_HAS_GROUP : 0)
                  | (info->link_target != nullptr ? INFO_HAS_TARGET : 0);
  put_u8 (out, flags);
  put_u32 (out, info->mode);
  put_u64 (out, info->size);
  put_u32 (out, info->uid);
  put_u32 (out, info->gid);
  put_i64 (out, info->atime_ns);
  put_i64 (out, info->mtime_ns);
  put_i64 (out, info->ctime_ns);
  put_i64 (out, info->btime_ns);
  if (info->owner != nullptr)
    put_str16 (out, info->owner, USER_NAME_MAX);
  if (info->group != nullptr)
    put_str16 (out, info->group, USER_NAME_MAX);
  if (info->link_target != nullptr)
    put_str16 (out, info->link_target, PATH_MAX_BYTES);
  end_frame (out, f);
}

static size_t
entry_size (const GbkEntry *e)
{
  return 2u + e->name_len + 1u + 1u + 8u + 8u;
}

static void
put_entry (GByteArray *out, const GbkEntry *e)
{
  put_u16 (out, (uint16_t) e->name_len);
  g_byte_array_append (out, (const guint8 *) gbk_entry_name (e), e->name_len);
  put_u8 (out, e->kind);
  uint8_t flags = ((e->flags & GBK_ENTRY_SYMLINK) ? ENTRY_SYMLINK : 0)
                  | ((e->flags & GBK_ENTRY_STAT_FAILED) ? ENTRY_STAT_FAILED : 0)
                  | ((e->flags & GBK_ENTRY_EXECUTABLE) ? ENTRY_EXEC : 0);
  put_u8 (out, flags);
  put_u64 (out, e->size);
  put_i64 (out, e->mtime_ns);
}

/* One SCAN frame: removes[r0, r1) and upserts[u0, u1) of `m`. */
static void
put_scan_piece (GByteArray *out, uint64_t channel, const GbkScanMsg *m, uint8_t flags, guint r0,
                guint r1, guint u0, guint u1)
{
  size_t f = begin_frame (out, GBK_MSG_SCAN, channel);
  put_u8 (out, (uint8_t) m->type);
  put_u8 (out, flags);
  put_u64 (out, (flags & SCAN_HAS_FREE) ? m->free_bytes : 0);
  put_u32 (out, (uint32_t) m->error);
  put_u32 (out, r1 - r0);
  for (guint i = r0; i < r1; i++)
    put_str16 (out, g_ptr_array_index (m->removes, i), NAME_MAX_BYTES);
  put_u32 (out, u1 - u0);
  for (guint i = u0; i < u1; i++)
    put_entry (out, g_ptr_array_index (m->upserts, i));
  end_frame (out, f);
}

void
gbk_proto_put_scan (GByteArray *out, uint64_t channel, const GbkScanMsg *m)
{
  if (m->type == GBK_SCAN_CHUNK && m->removes->len > 0)
    gbk_fatal ("protocol: a snapshot chunk with removals");
  uint8_t all = (m->first ? SCAN_FIRST : 0) | (m->last ? SCAN_LAST : 0) | (m->live ? SCAN_LIVE : 0)
                | (m->has_free ? SCAN_HAS_FREE : 0);
  /* Greedy packing in apply order: removals, then additions. */
  guint r = 0, u = 0;
  bool first_piece = true;
  do
    {
      guint r0 = r, u0 = u;
      size_t used = 0;
      while (r < m->removes->len)
        {
          size_t need = 2u + strlen (g_ptr_array_index (m->removes, r));
          if (used + need > SCAN_BUDGET)
            break;
          used += need;
          r++;
        }
      while (r == m->removes->len && u < m->upserts->len)
        {
          size_t need = entry_size (g_ptr_array_index (m->upserts, u));
          if (used + need > SCAN_BUDGET)
            break;
          used += need;
          u++;
        }
      bool last_piece = r == m->removes->len && u == m->upserts->len;
      uint8_t flags = all & SCAN_LIVE;
      if (first_piece)
        flags |= all & SCAN_FIRST;
      if (last_piece)
        flags |= all & (SCAN_LAST | SCAN_HAS_FREE);
      if (r == r0 && u == u0 && !last_piece)
        gbk_fatal ("protocol: a scan item larger than a frame");
      put_scan_piece (out, channel, m, flags, r0, r, u0, u);
      first_piece = false;
    }
  while (r < m->removes->len || u < m->upserts->len);
}

/* ---- copies: details, listings, contents ---- */

/* Fixed part of an entry: mode, uid, gid, nlink, size, dev, ino, rdev, three times. */
#define ENTRY_FIXED (4u + 4u + 4u + 8u + 8u + 8u + 8u + 8u + 8u + 8u + 8u)

static size_t
stat_entry_size (const GbkStatEntry *e)
{
  return 2u + (e->name != nullptr ? strlen (e->name) : 0) + ENTRY_FIXED + 2u
         + (e->target != nullptr ? strlen (e->target) : 0);
}

static void
put_stat_entry (GByteArray *out, const GbkStatEntry *e)
{
  put_str16 (out, e->name != nullptr ? e->name : "", NAME_MAX_BYTES);
  put_u32 (out, e->mode);
  put_u32 (out, e->uid);
  put_u32 (out, e->gid);
  put_u64 (out, e->nlink);
  put_u64 (out, e->size);
  put_u64 (out, e->dev);
  put_u64 (out, e->ino);
  put_u64 (out, e->rdev);
  put_i64 (out, e->atime_ns);
  put_i64 (out, e->mtime_ns);
  put_i64 (out, e->ctime_ns);
  put_str16 (out, e->target != nullptr ? e->target : "", PATH_MAX_BYTES);
}

void
gbk_proto_put_listing (GByteArray *out, uint64_t channel, const GPtrArray *entries, bool last)
{
  const size_t budget = GBK_PROTO_FRAME_MAX - HEADER_BODY - 1u - 4u;
  guint i = 0;
  do
    {
      guint first = i;
      size_t used = 0;
      while (i < entries->len)
        {
          size_t need = stat_entry_size (g_ptr_array_index (entries, i));
          if (used + need > budget)
            break;
          used += need;
          i++;
        }
      if (i == first && i < entries->len)
        gbk_fatal ("protocol: a listing entry larger than a frame");
      size_t f = begin_frame (out, GBK_MSG_LISTING, channel);
      put_u8 (out, last && i == entries->len ? 1 : 0);
      put_u32 (out, i - first);
      for (guint k = first; k < i; k++)
        put_stat_entry (out, g_ptr_array_index (entries, k));
      end_frame (out, f);
    }
  while (i < entries->len);
}

void
gbk_proto_put_stated (GByteArray *out, uint64_t channel, const GbkStatEntry *e)
{
  GbkStatEntry nameless = *e;
  nameless.name = nullptr;
  size_t f = begin_frame (out, GBK_MSG_STATED, channel);
  put_stat_entry (out, &nameless);
  end_frame (out, f);
}

void
gbk_proto_put_data (GByteArray *out, uint64_t channel, const void *data, size_t len)
{
  if (len > GBK_PROTO_DATA_MAX)
    gbk_fatal ("protocol: %zu bytes of data in one frame", len);
  size_t f = begin_frame (out, GBK_MSG_DATA, channel);
  g_byte_array_append (out, data, (guint) len);
  end_frame (out, f);
}

void
gbk_proto_put_end (GByteArray *out, uint64_t channel, bool changed)
{
  size_t f = begin_frame (out, GBK_MSG_END, channel);
  put_u8 (out, changed ? 1 : 0);
  end_frame (out, f);
}

void
gbk_proto_put_request (GByteArray *out, GbkMsgType type, uint64_t channel, uint64_t session,
                       const char *path)
{
  size_t f = begin_frame (out, type, channel);
  put_u64 (out, session);
  put_str16 (out, path, PATH_MAX_BYTES);
  end_frame (out, f);
}

void
gbk_proto_put_read (GByteArray *out, uint64_t channel, uint64_t session, const char *path,
                    uint32_t credit)
{
  size_t f = begin_frame (out, GBK_MSG_READ, channel);
  put_u64 (out, session);
  put_str16 (out, path, PATH_MAX_BYTES);
  put_u32 (out, credit);
  end_frame (out, f);
}

void
gbk_proto_put_session (GByteArray *out, uint64_t session, bool record)
{
  size_t f = begin_frame (out, GBK_MSG_SESSION, session);
  put_u8 (out, record ? 1 : 0);
  end_frame (out, f);
}

/* Strings in as many frames of `type` as they need: u32 count, then each one. */
static void
put_strings (GByteArray *out, GbkMsgType type, uint64_t channel, const GPtrArray *strs, size_t max)
{
  const size_t budget = GBK_PROTO_FRAME_MAX - HEADER_BODY - 4u;
  guint i = 0;
  while (i < strs->len)
    {
      guint first = i;
      size_t used = 0;
      while (i < strs->len && used + 2u + strlen (g_ptr_array_index (strs, i)) <= budget)
        used += 2u + strlen (g_ptr_array_index (strs, i++));
      size_t f = begin_frame (out, type, channel);
      put_u32 (out, i - first);
      for (guint k = first; k < i; k++)
        put_str16 (out, g_ptr_array_index (strs, k), max);
      end_frame (out, f);
    }
}

void
gbk_proto_put_job (GByteArray *out, uint64_t channel, const GbkJobRequest *r,
                   const GPtrArray *sources)
{
  put_strings (out, GBK_MSG_SOURCES, channel, sources, PATH_MAX_BYTES);
  size_t f = begin_frame (out, GBK_MSG_JOB, channel);
  put_u8 (out, (uint8_t) r->kind);
  put_u8 (out, r->sources_here ? 1 : 0);
  put_u64 (out, r->session);
  put_str16 (out, r->dest != nullptr ? r->dest : "", PATH_MAX_BYTES);
  end_frame (out, f);
}

void
gbk_proto_put_question (GByteArray *out, uint64_t channel, GbkAskKind kind, const char *path,
                        const char *other, bool other_is_dir, const GError *error)
{
  size_t f = begin_frame (out, GBK_MSG_QUESTION, channel);
  put_u8 (out, (uint8_t) kind);
  put_u8 (out, kind == GBK_ASK_CONFLICT && other_is_dir ? 1 : 0);
  put_str16 (out, path, PATH_MAX_BYTES);
  switch (kind)
    {
    case GBK_ASK_CONFLICT: put_str16 (out, other, PATH_MAX_BYTES); break;
    case GBK_ASK_ERROR:
      put_u32 (out, io_code (error));
      put_message (out, error->message);
      break;
    case GBK_ASK_UNTRASHABLE: break;
    default: gbk_fatal ("protocol: question kind %d", (int) kind);
    }
  end_frame (out, f);
}

void
gbk_proto_put_answer (GByteArray *out, uint64_t channel, GbkChoice choice, bool apply_all)
{
  size_t f = begin_frame (out, GBK_MSG_ANSWER, channel);
  put_u8 (out, (uint8_t) choice);
  put_u8 (out, apply_all ? 1 : 0);
  end_frame (out, f);
}

void
gbk_proto_put_progress (GByteArray *out, uint64_t channel, uint64_t done, uint64_t total,
                        const char *current)
{
  size_t f = begin_frame (out, GBK_MSG_PROGRESS, channel);
  put_u64 (out, done);
  put_u64 (out, total);
  put_str16 (out, current, PATH_MAX_BYTES);
  end_frame (out, f);
}

void
gbk_proto_put_created (GByteArray *out, uint64_t channel, const GPtrArray *names)
{
  put_strings (out, GBK_MSG_CREATED, channel, names, NAME_MAX_BYTES);
}

void
gbk_proto_put_credit (GByteArray *out, uint64_t channel, uint32_t bytes)
{
  size_t f = begin_frame (out, GBK_MSG_CREDIT, channel);
  put_u32 (out, bytes);
  end_frame (out, f);
}

/* ---- framing ---- */

static uint64_t
le (const uint8_t *p, unsigned bytes)
{
  uint64_t v = 0;
  for (unsigned i = 0; i < bytes; i++)
    v |= (uint64_t) p[i] << (8 * i);
  return v;
}

static bool
known_type (uint8_t t)
{
  switch (t)
    {
    case GBK_MSG_HELLO:
    case GBK_MSG_SCAN:
    case GBK_MSG_INFO:
    case GBK_MSG_FAILED:
    case GBK_MSG_PONG:
    case GBK_MSG_WATCH:
    case GBK_MSG_UNWATCH:
    case GBK_MSG_GET_INFO:
    case GBK_MSG_PING:
    case GBK_MSG_LISTING:
    case GBK_MSG_STATED:
    case GBK_MSG_DATA:
    case GBK_MSG_END:
    case GBK_MSG_LIST:
    case GBK_MSG_LSTAT:
    case GBK_MSG_READ:
    case GBK_MSG_CREDIT:
    case GBK_MSG_CANCEL:
    case GBK_MSG_REMOVED:
    case GBK_MSG_QUESTION:
    case GBK_MSG_PROGRESS:
    case GBK_MSG_CREATED:
    case GBK_MSG_DONE:
    case GBK_MSG_REMOVE:
    case GBK_MSG_SESSION:
    case GBK_MSG_FORGET:
    case GBK_MSG_SOURCES:
    case GBK_MSG_JOB:
    case GBK_MSG_ANSWER: return true;
    default: return false;
    }
}

int
gbk_proto_next_frame (const uint8_t *data, size_t len, GbkFrame *frame, size_t *used)
{
  if (len < 4)
    return 0;
  uint64_t body = le (data, 4);
  if (body < HEADER_BODY || body > GBK_PROTO_FRAME_MAX)
    return -1;
  if (len < 5)
    return 0;                            /* the type byte hasn't arrived yet */
  if (!known_type (data[4]))
    return -1;
  if (len - 4 < body)
    return 0;
  frame->type = (GbkMsgType) data[4];
  frame->channel = le (data + 5, 8);
  frame->payload = data + 4 + HEADER_BODY;
  frame->len = (size_t) body - HEADER_BODY;
  *used = 4 + (size_t) body;
  return 1;
}

static bool
build_char (char c)
{
  return g_ascii_isdigit (c) || (c >= 'a' && c <= 'z') || c == '-';
}

static bool
valid_build (const char *s, size_t len)
{
  if (len == 0 || len > BUILD_MAX)
    return false;
  for (size_t i = 0; i < len; i++)
    if (!build_char (s[i]))
      return false;
  return true;
}

int
gbk_proto_magic (const uint8_t *data, size_t len, uint32_t *version, char **build, size_t *used)
{
  if (len == 0)
    return 0;                            /* (and an empty buffer's data may be nullptr) */
  const uint8_t *nl = memchr (data, '\n', MIN (len, (size_t) GBK_PROTO_MAGIC_MAX));
  if (nl == nullptr)
    return len >= GBK_PROTO_MAGIC_MAX ? -1 : 0;
  size_t line_len = (size_t) (nl - data);
  const size_t prefix = strlen (GBK_PROTO_MAGIC " ");
  if (line_len <= prefix || memcmp (data, GBK_PROTO_MAGIC " ", prefix) != 0)
    return -1;
  const char *p = (const char *) data + prefix;
  const char *end = (const char *) nl;
  const char *space = memchr (p, ' ', (size_t) (end - p));
  if (space == nullptr || space == p || space - p > 9)
    return -1;
  uint64_t v = 0;
  for (const char *c = p; c < space; c++)
    {
      if (!g_ascii_isdigit (*c))
        return -1;
      v = v * 10 + (uint64_t) (*c - '0');
    }
  const char *b = space + 1;
  if (!valid_build (b, (size_t) (end - b)))
    return -1;
  *version = (uint32_t) v;
  *build = g_strndup (b, (size_t) (end - b));
  *used = line_len + 1;
  return 1;
}

/* ---- reading ---- */

typedef struct
{
  const uint8_t *p;
  size_t left;
  bool ok;
} Reader;

static Reader
reader (const GbkFrame *f)
{
  return (Reader) { f->payload, f->len, true };
}

static uint64_t
get_le (Reader *r, unsigned bytes)
{
  if (!r->ok || r->left < bytes)
    {
      r->ok = false;
      return 0;
    }
  uint64_t v = le (r->p, bytes);
  r->p += bytes;
  r->left -= bytes;
  return v;
}

static uint8_t
get_u8 (Reader *r)
{
  return (uint8_t) get_le (r, 1);
}

static uint32_t
get_u32 (Reader *r)
{
  return (uint32_t) get_le (r, 4);
}

static uint64_t
get_u64 (Reader *r)
{
  return get_le (r, 8);
}

static int64_t
get_i64 (Reader *r)
{
  return (int64_t) get_le (r, 8);
}

/* A length-prefixed byte string of at most `max` bytes: borrowed, not terminated. */
static const char *
get_bytes (Reader *r, size_t max, size_t *len)
{
  size_t n = (size_t) get_le (r, 2);
  if (!r->ok || n > max || n > r->left)
    {
      r->ok = false;
      *len = 0;
      return nullptr;
    }
  const char *s = (const char *) r->p;
  r->p += n;
  r->left -= n;
  *len = n;
  return s;
}

/* A string without NUL bytes, as an owned terminated copy. */
static char *
get_str (Reader *r, size_t max)
{
  size_t len;
  const char *s = get_bytes (r, max, &len);
  if (s == nullptr || memchr (s, '\0', len) != nullptr)
    {
      r->ok = false;
      return nullptr;
    }
  return g_strndup (s, len);
}

/* All of the payload used, and nothing went wrong. */
static bool
done (const Reader *r)
{
  return r->ok && r->left == 0;
}

static bool
absolute (const char *path)
{
  return path != nullptr && path[0] == '/';
}

bool
gbk_proto_get_hello (const GbkFrame *f, GbkHello *h)
{
  *h = (GbkHello) { 0 };
  if (f->type != GBK_MSG_HELLO || f->channel != 0)
    return false;
  Reader r = reader (f);
  h->version = get_u32 (&r);
  h->build = get_str (&r, BUILD_MAX);
  h->machine_id = get_str (&r, MACHINE_ID_MAX);
  h->uid = get_u32 (&r);
  h->home = get_str (&r, PATH_MAX_BYTES);
  bool ok = done (&r) && valid_build (h->build, strlen (h->build)) && absolute (h->home);
  for (const char *c = ok ? h->machine_id : ""; *c != '\0'; c++)
    ok = ok && g_ascii_isxdigit (*c);
  if (!ok)
    gbk_hello_clear (h);
  return ok;
}

char *
gbk_proto_get_path (const GbkFrame *f)
{
  if (f->type != GBK_MSG_WATCH && f->type != GBK_MSG_GET_INFO)
    return nullptr;
  Reader r = reader (f);
  g_autofree char *path = get_str (&r, PATH_MAX_BYTES);
  return done (&r) && absolute (path) ? g_steal_pointer (&path) : nullptr;
}

bool
gbk_proto_get_empty (const GbkFrame *f)
{
  return (f->type == GBK_MSG_UNWATCH || f->type == GBK_MSG_PING || f->type == GBK_MSG_PONG
          || f->type == GBK_MSG_CANCEL || f->type == GBK_MSG_REMOVED || f->type == GBK_MSG_FORGET
          || f->type == GBK_MSG_DONE)
         && f->len == 0;
}

bool
gbk_proto_get_failed (const GbkFrame *f, GError **error)
{
  if (f->type != GBK_MSG_FAILED)
    return false;
  Reader r = reader (f);
  uint32_t code = get_u32 (&r);
  g_autofree char *msg = get_str (&r, MESSAGE_MAX);
  if (!done (&r) || code >= IO_ERROR_CODE_MAX)
    return false;
  g_autofree char *shown = g_utf8_make_valid (msg, -1);
  g_set_error_literal (error, G_IO_ERROR, (int) code, shown);
  return true;
}

/* An owner or group name, for showing: made valid UTF-8. */
static char *
get_user_name (Reader *r)
{
  g_autofree char *raw = get_str (r, USER_NAME_MAX);
  return raw != nullptr ? g_utf8_make_valid (raw, -1) : nullptr;
}

GbkFileInfo *
gbk_proto_get_info (const GbkFrame *f)
{
  if (f->type != GBK_MSG_INFO)
    return nullptr;
  Reader r = reader (f);
  g_autoptr (GbkFileInfo) info = g_new0 (GbkFileInfo, 1);
  uint8_t flags = get_u8 (&r);
  info->symlink = (flags & INFO_SYMLINK) != 0;
  info->dangling = (flags & INFO_DANGLING) != 0;
  info->has_btime = (flags & INFO_HAS_BTIME) != 0;
  info->mode = get_u32 (&r);
  info->size = get_u64 (&r);
  info->uid = get_u32 (&r);
  info->gid = get_u32 (&r);
  info->atime_ns = get_i64 (&r);
  info->mtime_ns = get_i64 (&r);
  info->ctime_ns = get_i64 (&r);
  info->btime_ns = get_i64 (&r);
  if (flags & INFO_HAS_OWNER)
    info->owner = get_user_name (&r);
  if (flags & INFO_HAS_GROUP)
    info->group = get_user_name (&r);
  if (flags & INFO_HAS_TARGET)
    info->link_target = get_str (&r, PATH_MAX_BYTES);
  if (!done (&r) || (flags & ~INFO_FLAGS) != 0)
    return nullptr;
  return g_steal_pointer (&info);
}

static GbkEntry *
get_entry (Reader *r, const GbkTypeTable *types)
{
  size_t len;
  const char *name = get_bytes (r, NAME_MAX_BYTES, &len);
  /* One read per statement: the order of reads inside an initializer is unspecified. */
  uint8_t kind = get_u8 (r);
  uint8_t flags = get_u8 (r);
  uint64_t size = get_u64 (r);
  int64_t mtime_ns = get_i64 (r);
  GbkEntryInfo info = {
    .kind = (GbkFileKind) kind,
    .symlink = (flags & ENTRY_SYMLINK) != 0,
    .stat_failed = (flags & ENTRY_STAT_FAILED) != 0,
    .mode = (flags & ENTRY_EXEC) ? 0100u : 0u,
    .size = size,
    .mtime_ns = mtime_ns,
  };
  if (!r->ok || kind > GBK_KIND_BROKEN_LINK || (flags & ~ENTRY_FLAGS) != 0)
    {
      r->ok = false;
      return nullptr;
    }
  GbkEntry *e = gbk_entry_new (name, len, &info, types);   /* validates the name */
  if (e == nullptr)
    r->ok = false;
  return e;
}

GbkScanMsg *
gbk_proto_get_scan (const GbkFrame *f, const GbkTypeTable *types)
{
  if (f->type != GBK_MSG_SCAN)
    return nullptr;
  Reader r = reader (f);
  uint8_t type = get_u8 (&r);
  uint8_t flags = get_u8 (&r);
  uint64_t free_bytes = get_u64 (&r);
  uint32_t error = get_u32 (&r);
  if (!r.ok || type > GBK_SCAN_GONE || (flags & ~SCAN_FLAGS) != 0)
    return nullptr;
  /* An error carries an errno value; nothing else carries one. */
  if ((type == GBK_SCAN_ERROR) != (error != 0) || error > 4095)
    return nullptr;

  g_autoptr (GbkScanMsg) m = gbk_scan_msg_new ((GbkScanMsgType) type);
  m->first = (flags & SCAN_FIRST) != 0;
  m->last = (flags & SCAN_LAST) != 0;
  m->live = (flags & SCAN_LIVE) != 0;
  m->has_free = (flags & SCAN_HAS_FREE) != 0;
  m->free_bytes = m->has_free ? free_bytes : 0;
  m->error = (int) error;

  uint32_t n_removes = get_u32 (&r);
  for (uint32_t i = 0; r.ok && i < n_removes; i++)
    {
      size_t len;
      const char *name = get_bytes (&r, NAME_MAX_BYTES, &len);
      if (name == nullptr || !gbk_entry_name_valid (name, len))
        return nullptr;
      g_ptr_array_add (m->removes, g_strndup (name, len));
    }
  uint32_t n_upserts = get_u32 (&r);
  for (uint32_t i = 0; r.ok && i < n_upserts; i++)
    {
      GbkEntry *e = get_entry (&r, types);
      if (e == nullptr)
        return nullptr;
      g_ptr_array_add (m->upserts, e);
    }
  if (!done (&r))
    return nullptr;
  /* Only a delta removes; only a chunk or a delta lists entries. */
  if ((type != GBK_SCAN_DELTA && n_removes > 0)
      || ((type == GBK_SCAN_ERROR || type == GBK_SCAN_GONE) && n_upserts > 0))
    return nullptr;
  return g_steal_pointer (&m);
}

static bool
valid_file_type (uint32_t mode)
{
  switch (mode & S_IFMT)
    {
    case S_IFREG:
    case S_IFDIR:
    case S_IFLNK:
    case S_IFCHR:
    case S_IFBLK:
    case S_IFIFO:
    case S_IFSOCK: return true;
    default: return false;
    }
}

/* One entry; `named`: a LISTING entry (a valid filename) rather than STATED (none). */
static GbkStatEntry *
get_stat_entry (Reader *r, bool named)
{
  g_autoptr (GbkStatEntry) e = g_new0 (GbkStatEntry, 1);
  size_t name_len;
  const char *name = get_bytes (r, NAME_MAX_BYTES, &name_len);
  uint32_t mode = get_u32 (r);
  uint32_t uid = get_u32 (r);
  uint32_t gid = get_u32 (r);
  uint64_t nlink = get_u64 (r);
  uint64_t size = get_u64 (r);
  uint64_t dev = get_u64 (r);
  uint64_t ino = get_u64 (r);
  uint64_t rdev = get_u64 (r);
  int64_t atime_ns = get_i64 (r);
  int64_t mtime_ns = get_i64 (r);
  int64_t ctime_ns = get_i64 (r);
  size_t target_len;
  const char *target = get_bytes (r, PATH_MAX_BYTES, &target_len);
  if (!r->ok || !valid_file_type (mode) || size > INT64_MAX)
    {
      r->ok = false;
      return nullptr;
    }
  /* A name exactly when named; a target exactly for a link, without NUL bytes. */
  bool link = (mode & S_IFMT) == S_IFLNK;
  if ((named ? !gbk_entry_name_valid (name, name_len) : name_len != 0)
      || (link ? target_len == 0 || memchr (target, '\0', target_len) != nullptr : target_len != 0))
    {
      r->ok = false;
      return nullptr;
    }
  e->name = named ? g_strndup (name, name_len) : nullptr;
  e->mode = mode;
  e->uid = uid;
  e->gid = gid;
  e->nlink = nlink;
  e->size = size;
  e->dev = dev;
  e->ino = ino;
  e->rdev = rdev;
  e->atime_ns = atime_ns;
  e->mtime_ns = mtime_ns;
  e->ctime_ns = ctime_ns;
  e->target = link ? g_strndup (target, target_len) : nullptr;
  return g_steal_pointer (&e);
}

bool
gbk_proto_get_listing (const GbkFrame *f, GPtrArray **entries, bool *last)
{
  *entries = nullptr;
  if (f->type != GBK_MSG_LISTING)
    return false;
  Reader r = reader (f);
  uint8_t flag = get_u8 (&r);
  uint32_t n = get_u32 (&r);
  if (!r.ok || flag > 1)
    return false;
  g_autoptr (GPtrArray) out = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_stat_entry_free);
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  for (uint32_t i = 0; i < n; i++)
    {
      GbkStatEntry *e = get_stat_entry (&r, true);
      if (e == nullptr)
        return false;
      g_ptr_array_add (out, e);
      if (!g_hash_table_add (seen, e->name))
        return false;                    /* the same name twice */
    }
  if (!done (&r))
    return false;
  *entries = g_steal_pointer (&out);
  *last = flag == 1;
  return true;
}

GbkStatEntry *
gbk_proto_get_stated (const GbkFrame *f)
{
  if (f->type != GBK_MSG_STATED)
    return nullptr;
  Reader r = reader (f);
  g_autoptr (GbkStatEntry) e = get_stat_entry (&r, false);
  return e != nullptr && done (&r) ? g_steal_pointer (&e) : nullptr;
}

bool
gbk_proto_get_data (const GbkFrame *f, const uint8_t **data, size_t *len)
{
  if (f->type != GBK_MSG_DATA || f->len > GBK_PROTO_DATA_MAX)
    return false;
  *data = f->payload;
  *len = f->len;
  return true;
}

bool
gbk_proto_get_end (const GbkFrame *f, bool *changed)
{
  if (f->type != GBK_MSG_END)
    return false;
  Reader r = reader (f);
  uint8_t c = get_u8 (&r);
  if (!done (&r) || c > 1)
    return false;
  *changed = c == 1;
  return true;
}

bool
gbk_proto_get_read (const GbkFrame *f, uint64_t *session, char **path, uint32_t *credit)
{
  *path = nullptr;
  if (f->type != GBK_MSG_READ)
    return false;
  Reader r = reader (f);
  uint64_t s = get_u64 (&r);
  g_autofree char *p = get_str (&r, PATH_MAX_BYTES);
  uint32_t c = get_u32 (&r);
  if (!done (&r) || !absolute (p))
    return false;
  *session = s;
  *path = g_steal_pointer (&p);
  *credit = c;
  return true;
}

bool
gbk_proto_get_request (const GbkFrame *f, uint64_t *session, char **path)
{
  *path = nullptr;
  if (f->type != GBK_MSG_LIST && f->type != GBK_MSG_LSTAT && f->type != GBK_MSG_REMOVE)
    return false;
  Reader r = reader (f);
  uint64_t s = get_u64 (&r);
  g_autofree char *p = get_str (&r, PATH_MAX_BYTES);
  if (!done (&r) || !absolute (p))
    return false;
  *session = s;
  *path = g_steal_pointer (&p);
  return true;
}

bool
gbk_proto_get_session (const GbkFrame *f, bool *record)
{
  if (f->type != GBK_MSG_SESSION)
    return false;
  Reader r = reader (f);
  uint8_t v = get_u8 (&r);
  if (!done (&r) || v > 1)
    return false;
  *record = v == 1;
  return true;
}

/* A string list frame (SOURCES, CREATED): each one checked by `valid`, appended to `out`
 * only if all of them are. */
static bool
get_strings (const GbkFrame *f, size_t max, bool (*valid) (const char *), GPtrArray *out)
{
  Reader r = reader (f);
  uint32_t n = get_u32 (&r);
  g_autoptr (GPtrArray) got = g_ptr_array_new_with_free_func (g_free);
  for (uint32_t i = 0; r.ok && i < n; i++)
    {
      char *s = get_str (&r, max);
      if (s == nullptr || !valid (s))
        {
          g_free (s);
          return false;
        }
      g_ptr_array_add (got, s);
    }
  if (!done (&r) || n == 0)
    return false;
  for (guint i = 0; i < got->len; i++)
    g_ptr_array_add (out, g_steal_pointer (&got->pdata[i]));
  return true;
}

static bool
valid_absolute (const char *s)
{
  return absolute (s);
}

static bool
valid_name (const char *s)
{
  return gbk_entry_name_valid (s, strlen (s));
}

bool
gbk_proto_get_sources (const GbkFrame *f, GPtrArray *sources)
{
  return f->type == GBK_MSG_SOURCES && get_strings (f, PATH_MAX_BYTES, valid_absolute, sources);
}

bool
gbk_proto_get_created (const GbkFrame *f, GPtrArray *names)
{
  return f->type == GBK_MSG_CREATED && get_strings (f, NAME_MAX_BYTES, valid_name, names);
}

void
gbk_job_request_clear (GbkJobRequest *r)
{
  g_free (r->dest);
  r->dest = nullptr;
}

bool
gbk_proto_get_job (const GbkFrame *f, GbkJobRequest *r)
{
  *r = (GbkJobRequest) { 0 };
  if (f->type != GBK_MSG_JOB)
    return false;
  Reader rd = reader (f);
  uint8_t kind = get_u8 (&rd);
  uint8_t here = get_u8 (&rd);
  uint64_t session = get_u64 (&rd);
  g_autofree char *dest = get_str (&rd, PATH_MAX_BYTES);
  if (!done (&rd) || kind > GBK_OP_DELETE || here > 1)
    return false;
  bool needs_dest = kind == GBK_OP_COPY || kind == GBK_OP_MOVE;
  if (needs_dest ? !absolute (dest) : dest[0] != '\0')
    return false;
  r->kind = (GbkOpKind) kind;
  r->sources_here = here == 1;
  r->session = session;
  r->dest = needs_dest ? g_steal_pointer (&dest) : nullptr;
  return true;
}

void
gbk_ask_clear (GbkAsk *a)
{
  g_clear_pointer (&a->path, g_free);
  g_clear_pointer (&a->other, g_free);
  g_clear_error (&a->error);
}

bool
gbk_proto_get_question (const GbkFrame *f, GbkAsk *a)
{
  *a = (GbkAsk) { 0 };
  if (f->type != GBK_MSG_QUESTION)
    return false;
  Reader r = reader (f);
  uint8_t kind = get_u8 (&r);
  uint8_t dir = get_u8 (&r);
  g_autofree char *path = get_str (&r, PATH_MAX_BYTES);
  if (!r.ok || kind > GBK_ASK_UNTRASHABLE || dir > (kind == GBK_ASK_CONFLICT ? 1 : 0)
      || !absolute (path))
    return false;
  g_autofree char *other = nullptr;
  g_autoptr (GError) error = nullptr;
  if (kind == GBK_ASK_CONFLICT)
    {
      other = get_str (&r, PATH_MAX_BYTES);
      if (!absolute (other))
        return false;
    }
  else if (kind == GBK_ASK_ERROR)
    {
      uint32_t code = get_u32 (&r);
      g_autofree char *msg = get_str (&r, MESSAGE_MAX);
      if (!r.ok || code >= IO_ERROR_CODE_MAX)
        return false;
      g_autofree char *shown = g_utf8_make_valid (msg, -1);
      error = g_error_new_literal (G_IO_ERROR, (int) code, shown);
    }
  if (!done (&r))
    return false;
  a->kind = (GbkAskKind) kind;
  a->other_is_dir = dir == 1;
  a->path = g_steal_pointer (&path);
  a->other = g_steal_pointer (&other);
  a->error = g_steal_pointer (&error);
  return true;
}

bool
gbk_proto_get_answer (const GbkFrame *f, GbkChoice *choice, bool *apply_all)
{
  if (f->type != GBK_MSG_ANSWER)
    return false;
  Reader r = reader (f);
  uint8_t c = get_u8 (&r);
  uint8_t all = get_u8 (&r);
  if (!done (&r) || c > GBK_CHOICE_CANCEL || all > 1)
    return false;
  *choice = (GbkChoice) c;
  *apply_all = all == 1;
  return true;
}

bool
gbk_proto_get_progress (const GbkFrame *f, uint64_t *done_bytes, uint64_t *total, char **current)
{
  *current = nullptr;
  if (f->type != GBK_MSG_PROGRESS)
    return false;
  Reader r = reader (f);
  uint64_t d = get_u64 (&r);
  uint64_t t = get_u64 (&r);
  g_autofree char *c = get_str (&r, PATH_MAX_BYTES);
  if (!done (&r))
    return false;
  *done_bytes = d;
  *total = t;
  *current = g_steal_pointer (&c);
  return true;
}

bool
gbk_proto_get_credit (const GbkFrame *f, uint32_t *bytes)
{
  if (f->type != GBK_MSG_CREDIT)
    return false;
  Reader r = reader (f);
  uint32_t b = get_u32 (&r);
  if (!done (&r))
    return false;
  *bytes = b;
  return true;
}
