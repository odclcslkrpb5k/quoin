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
  bool io = error->domain == G_IO_ERROR && error->code >= 0
            && (unsigned) error->code < IO_ERROR_CODE_MAX;
  put_u32 (out, io ? (uint32_t) error->code : (uint32_t) G_IO_ERROR_FAILED);
  /* Cut at a character boundary to fit. */
  const char *msg = error->message;
  size_t len = strlen (msg);
  if (len > MESSAGE_MAX)
    len = (size_t) (g_utf8_find_prev_char (msg, msg + MESSAGE_MAX + 1) - msg);
  put_u16 (out, (uint16_t) len);
  g_byte_array_append (out, (const guint8 *) msg, (guint) len);
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
    case GBK_MSG_PING: return true;
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
  return (f->type == GBK_MSG_UNWATCH || f->type == GBK_MSG_PING || f->type == GBK_MSG_PONG)
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
