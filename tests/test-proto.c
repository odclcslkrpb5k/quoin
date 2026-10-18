/* The wire protocol (pure): every message survives a round trip; big listings split into
 * frames under the cap and reassemble into the same model; anything malformed is refused. */
#include "dirmodel.h"
#include "proto.h"

static GbkTypeTable *types;

/* The frames in `out`, in order. Fails the test on anything but whole frames. */
static GArray *
frames_of (const GByteArray *out)
{
  GArray *frames = g_array_new (false, false, sizeof (GbkFrame));
  size_t at = 0;
  while (at < out->len)
    {
      GbkFrame f;
      size_t used;
      g_assert_cmpint (gbk_proto_next_frame (out->data + at, out->len - at, &f, &used), ==, 1);
      g_assert_cmpuint (used - 4, <=, GBK_PROTO_FRAME_MAX);
      g_array_append_val (frames, f);
      at += used;
    }
  return frames;
}

static const GbkFrame *
only_frame (const GByteArray *out, GArray **keep)
{
  *keep = frames_of (out);
  g_assert_cmpuint ((*keep)->len, ==, 1);
  return &g_array_index (*keep, GbkFrame, 0);
}

static GbkEntry *
entry (const char *name, GbkFileKind kind, uint64_t size)
{
  GbkEntryInfo info = { .kind = kind, .size = size, .mtime_ns = 1234567890123, .mode = 0644 };
  return gbk_entry_new (name, strlen (name), &info, types);
}

static void
test_magic (void)
{
  g_autoptr (GByteArray) out = g_byte_array_new ();
  gbk_proto_put_magic (out, "abc123-dirty");
  uint32_t version = 0;
  g_autofree char *build = nullptr;
  size_t used = 0;
  g_assert_cmpint (gbk_proto_magic (out->data, out->len, &version, &build, &used), ==, 1);
  g_assert_cmpuint (version, ==, GBK_PROTO_VERSION);
  g_assert_cmpstr (build, ==, "abc123-dirty");
  g_assert_cmpuint (used, ==, out->len);
  /* Not complete yet: wait. */
  g_assert_cmpint (gbk_proto_magic (out->data, out->len - 1, &version, &build, &used), ==, 0);

  /* What a talkative .bashrc does: refused, never skipped. */
  const char *junk[] = {
    "Welcome to ws1!\nQUOIN-SERVE 1 abc\n",
    "QUOIN-SERVE x abc\n",
    "QUOIN-SERVE 1 ABC\n",
    "QUOIN-SERVE 1 \n",
    "QUOIN-SERVE 1\n",
    "QUOIN-SERVE  1 abc\n",
  };
  for (size_t i = 0; i < G_N_ELEMENTS (junk); i++)
    {
      g_autofree char *b = nullptr;
      g_assert_cmpint (
          gbk_proto_magic ((const uint8_t *) junk[i], strlen (junk[i]), &version, &b, &used), ==,
          -1);
    }
  /* A line that never ends is refused once it's longer than any magic line. */
  g_autofree char *endless = g_strnfill (GBK_PROTO_MAGIC_MAX, 'x');
  g_assert_cmpint (
      gbk_proto_magic ((const uint8_t *) endless, strlen (endless), &version, &build, &used), ==,
      -1);
}

static void
test_framing (void)
{
  g_autoptr (GByteArray) out = g_byte_array_new ();
  gbk_proto_put_empty (out, GBK_MSG_PING, 42);
  GbkFrame f;
  size_t used;
  /* Every prefix is "not yet", including exactly the length and nothing more (found by
   * fuzzing: the type byte was read before it had arrived). Copied, so ASan sees the end. */
  for (size_t n = 0; n < out->len; n++)
    {
      g_autofree uint8_t *prefix = g_memdup2 (out->data, n);
      g_assert_cmpint (gbk_proto_next_frame (prefix, n, &f, &used), ==, 0);
    }
  g_assert_cmpint (gbk_proto_next_frame (out->data, out->len, &f, &used), ==, 1);
  g_assert_cmpint (f.type, ==, GBK_MSG_PING);
  g_assert_cmpuint (f.channel, ==, 42);
  g_assert_true (gbk_proto_get_empty (&f));

  /* Unknown type, a length too short for the header, a length over the cap. */
  uint8_t bad_type[] = { 9, 0, 0, 0, 200, 0, 0, 0, 0, 0, 0, 0, 0 };
  uint8_t too_short[] = { 8, 0, 0, 0, GBK_MSG_PING, 0, 0, 0, 0, 0, 0, 0 };
  uint32_t big = GBK_PROTO_FRAME_MAX + 1;
  uint8_t too_long[] = { (uint8_t) big, (uint8_t) (big >> 8), (uint8_t) (big >> 16),
                         (uint8_t) (big >> 24), GBK_MSG_PING };
  g_assert_cmpint (gbk_proto_next_frame (bad_type, sizeof bad_type, &f, &used), ==, -1);
  g_assert_cmpint (gbk_proto_next_frame (too_short, sizeof too_short, &f, &used), ==, -1);
  g_assert_cmpint (gbk_proto_next_frame (too_long, sizeof too_long, &f, &used), ==, -1);
}

static void
test_hello_path_failed (void)
{
  char build[] = "0123abc", mid[] = "deadbeef", home[] = "/home/u", rel_home[] = "home/u";
  GbkHello h = { GBK_PROTO_VERSION, build, mid, 1000, home };
  g_autoptr (GByteArray) out = g_byte_array_new ();
  gbk_proto_put_hello (out, &h);
  g_autoptr (GArray) keep = nullptr;
  GbkHello got;
  g_assert_true (gbk_proto_get_hello (only_frame (out, &keep), &got));
  g_assert_cmpstr (got.build, ==, "0123abc");
  g_assert_cmpstr (got.machine_id, ==, "deadbeef");
  g_assert_cmpuint (got.uid, ==, 1000);
  g_assert_cmpstr (got.home, ==, "/home/u");
  gbk_hello_clear (&got);

  /* A home that isn't absolute is refused. */
  GbkHello rel = h;
  rel.home = rel_home;
  g_autoptr (GByteArray) out2 = g_byte_array_new ();
  gbk_proto_put_hello (out2, &rel);
  g_autoptr (GArray) keep2 = nullptr;
  g_assert_false (gbk_proto_get_hello (only_frame (out2, &keep2), &got));

  g_autoptr (GByteArray) out3 = g_byte_array_new ();
  gbk_proto_put_path (out3, GBK_MSG_WATCH, 5, "/srv/a b");
  g_autoptr (GArray) keep3 = nullptr;
  g_autofree char *path = gbk_proto_get_path (only_frame (out3, &keep3));
  g_assert_cmpstr (path, ==, "/srv/a b");

  g_autoptr (GByteArray) out4 = g_byte_array_new ();
  gbk_proto_put_path (out4, GBK_MSG_GET_INFO, 6, "relative");
  g_autoptr (GArray) keep4 = nullptr;
  g_assert_null (gbk_proto_get_path (only_frame (out4, &keep4)));

  g_autoptr (GError) e = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No such file");
  g_autoptr (GByteArray) out5 = g_byte_array_new ();
  gbk_proto_put_failed (out5, 7, e);
  g_autoptr (GArray) keep5 = nullptr;
  g_autoptr (GError) back = nullptr;
  g_assert_true (gbk_proto_get_failed (only_frame (out5, &keep5), &back));
  g_assert_error (back, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_assert_cmpstr (back->message, ==, "No such file");
}

static void
test_info (void)
{
  char target[] = "../t\xff", owner[] = "gbk", group[] = "us\xe9rs";
  GbkFileInfo in = {
    .symlink = true,
    .link_target = target,
    .mode = 0100755,
    .size = 1ull << 40,
    .uid = 1000,
    .gid = 100,
    .owner = owner,
    .group = group,
    .atime_ns = 1,
    .mtime_ns = -2,
    .ctime_ns = 3,
    .btime_ns = 4,
    .has_btime = true,
  };
  g_autoptr (GByteArray) out = g_byte_array_new ();
  gbk_proto_put_info (out, 3, &in);
  g_autoptr (GArray) keep = nullptr;
  g_autoptr (GbkFileInfo) got = gbk_proto_get_info (only_frame (out, &keep));
  g_assert_nonnull (got);
  g_assert_true (got->symlink);
  g_assert_false (got->dangling);
  g_assert_cmpstr (got->link_target, ==, "../t\xff");   /* raw bytes, as on disk */
  g_assert_cmpuint (got->mode, ==, 0100755);
  g_assert_cmpuint (got->size, ==, 1ull << 40);
  g_assert_cmpstr (got->owner, ==, "gbk");
  g_assert_true (g_utf8_validate (got->group, -1, nullptr));  /* shown in a label */
  g_assert_cmpint (got->mtime_ns, ==, -2);
  g_assert_true (got->has_btime);
  g_assert_cmpint (got->btime_ns, ==, 4);
}

/* Apply every SCAN frame in `out` to `m`. */
static void
apply_all (GbkDirModel *m, const GByteArray *out)
{
  g_autoptr (GArray) frames = frames_of (out);
  for (guint i = 0; i < frames->len; i++)
    {
      GbkScanMsg *msg = gbk_proto_get_scan (&g_array_index (frames, GbkFrame, i), types);
      g_assert_nonnull (msg);
      gbk_dir_model_apply_for_test (m, msg);
    }
}

static void
test_scan_round_trip (void)
{
  g_autoptr (GbkScanMsg) chunk = gbk_scan_msg_new (GBK_SCAN_CHUNK);
  chunk->first = chunk->last = chunk->live = chunk->has_free = true;
  chunk->free_bytes = 99;
  g_ptr_array_add (chunk->upserts, entry ("a.txt", GBK_KIND_REGULAR, 5));
  g_ptr_array_add (chunk->upserts, entry ("dir", GBK_KIND_DIRECTORY, 0));
  g_ptr_array_add (chunk->upserts, entry ("caf\xe9", GBK_KIND_REGULAR, 1));
  g_autoptr (GByteArray) out = g_byte_array_new ();
  gbk_proto_put_scan (out, 9, chunk);

  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  apply_all (m, out);
  g_assert_cmpint (gbk_dir_model_state (m), ==, GBK_DIR_READY);
  g_assert_true (gbk_dir_model_live (m));
  uint64_t free_bytes;
  g_assert_true (gbk_dir_model_free_space (m, &free_bytes));
  g_assert_cmpuint (free_bytes, ==, 99);
  g_assert_cmpuint (gbk_dir_model_n_entries (m), ==, 3);
  const GbkEntry *a = gbk_dir_model_get (m, gbk_dir_model_find (m, "a.txt"));
  g_assert_cmpuint (a->size, ==, 5);
  g_assert_cmpint (a->mtime_ns, ==, 1234567890123);
  g_assert_true (gbk_entry_is_dir (gbk_dir_model_get (m, gbk_dir_model_find (m, "dir"))));
  g_assert_cmpuint (gbk_dir_model_find (m, "caf\xe9"), !=, GBK_HANDLE_NONE);

  /* A delta: one removal, one addition. */
  g_autoptr (GbkScanMsg) delta = gbk_scan_msg_new (GBK_SCAN_DELTA);
  g_ptr_array_add (delta->removes, g_strdup ("a.txt"));
  g_ptr_array_add (delta->upserts, entry ("new", GBK_KIND_REGULAR, 7));
  g_autoptr (GByteArray) out2 = g_byte_array_new ();
  gbk_proto_put_scan (out2, 9, delta);
  apply_all (m, out2);
  g_assert_cmpuint (gbk_dir_model_find (m, "a.txt"), ==, GBK_HANDLE_NONE);
  g_assert_cmpuint (gbk_dir_model_find (m, "new"), !=, GBK_HANDLE_NONE);

  g_autoptr (GbkScanMsg) err = gbk_scan_msg_new (GBK_SCAN_ERROR);
  err->error = EACCES;
  g_autoptr (GByteArray) out3 = g_byte_array_new ();
  gbk_proto_put_scan (out3, 9, err);
  apply_all (m, out3);
  g_assert_cmpint (gbk_dir_model_state (m), ==, GBK_DIR_ERROR);
  g_assert_cmpint (gbk_dir_model_error (m), ==, EACCES);
}

/* 20000 entries with 255-byte names: about 5 MB, so many frames. The model that results is
 * the same as from one message. */
static void
test_scan_split (void)
{
  g_autoptr (GbkScanMsg) chunk = gbk_scan_msg_new (GBK_SCAN_CHUNK);
  chunk->first = chunk->last = chunk->has_free = true;
  chunk->free_bytes = 7;
  for (int i = 0; i < 20000; i++)
    {
      char name[256];
      memset (name, 'n', 255);
      name[255] = '\0';
      g_snprintf (name, 12, "%010d", i);
      name[10] = 'n';
      g_ptr_array_add (chunk->upserts, entry (name, GBK_KIND_REGULAR, (uint64_t) i));
    }
  g_autoptr (GByteArray) out = g_byte_array_new ();
  gbk_proto_put_scan (out, 1, chunk);
  g_autoptr (GArray) frames = frames_of (out);
  g_assert_cmpuint (frames->len, >, 4);

  /* Only the first piece starts a snapshot, only the last ends it (and has the space). */
  for (guint i = 0; i < frames->len; i++)
    {
      g_autoptr (GbkScanMsg) piece
          = gbk_proto_get_scan (&g_array_index (frames, GbkFrame, i), types);
      g_assert_nonnull (piece);
      g_assert_cmpint (piece->first, ==, i == 0);
      g_assert_cmpint (piece->last, ==, i == frames->len - 1);
      g_assert_cmpint (piece->has_free, ==, i == frames->len - 1);
    }
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  apply_all (m, out);
  g_assert_cmpint (gbk_dir_model_state (m), ==, GBK_DIR_READY);
  g_assert_cmpuint (gbk_dir_model_n_entries (m), ==, 20000);
  gbk_dir_model_check_order (m);

  /* A delta removing 20000 and adding one: removals go first, whatever the split. */
  g_autoptr (GbkScanMsg) delta = gbk_scan_msg_new (GBK_SCAN_DELTA);
  for (guint i = 0; i < chunk->upserts->len; i++)
    g_ptr_array_add (delta->removes,
                     g_strdup (gbk_entry_name (g_ptr_array_index (chunk->upserts, i))));
  g_ptr_array_add (delta->upserts, entry (gbk_entry_name (g_ptr_array_index (chunk->upserts, 0)),
                                          GBK_KIND_DIRECTORY, 0));
  g_autoptr (GByteArray) out2 = g_byte_array_new ();
  gbk_proto_put_scan (out2, 1, delta);
  apply_all (m, out2);
  g_assert_cmpuint (gbk_dir_model_n_entries (m), ==, 1);
  gbk_dir_model_check_order (m);
}

/* Build a SCAN payload by hand, to say things our encoder never would. */
typedef struct
{
  GByteArray *b;
} Raw;

static void
raw_le (Raw *r, uint64_t v, unsigned n)
{
  for (unsigned i = 0; i < n; i++)
    {
      uint8_t c = (uint8_t) (v >> (8 * i));
      g_byte_array_append (r->b, &c, 1);
    }
}

static void
raw_name (Raw *r, const char *name, size_t len)
{
  raw_le (r, len, 2);
  g_byte_array_append (r->b, (const guint8 *) name, (guint) len);
}

static void
raw_entry (Raw *r, const char *name, size_t len, uint8_t kind, uint8_t flags)
{
  raw_name (r, name, len);
  raw_le (r, kind, 1);
  raw_le (r, flags, 1);
  raw_le (r, 0, 8);
  raw_le (r, 0, 8);
}

/* A SCAN frame around `payload`. */
static GbkFrame
raw_frame (const GByteArray *payload)
{
  return (GbkFrame) { GBK_MSG_SCAN, 1, payload->data, payload->len };
}

static void
raw_header (Raw *r, uint8_t type, uint8_t flags, uint32_t error)
{
  raw_le (r, type, 1);
  raw_le (r, flags, 1);
  raw_le (r, 0, 8);
  raw_le (r, error, 4);
}

/* A hostile or broken server: each of these is refused outright. */
static void
test_scan_refused (void)
{
  const struct
  {
    const char *what;
    const char *name;
    size_t len;
    uint8_t kind, flags;
  } bad_entries[] = {
    { "empty name", "", 0, 0, 0 },       { "dot", ".", 1, 0, 0 },
    { "dot dot", "..", 2, 0, 0 },        { "slash", "a/b", 3, 0, 0 },
    { "NUL", "a\0b", 3, 0, 0 },          { "kind out of range", "a", 1, 7, 0 },
    { "unknown flag", "a", 1, 0, 0x80 },
  };
  for (size_t i = 0; i < G_N_ELEMENTS (bad_entries); i++)
    {
      g_autoptr (GByteArray) b = g_byte_array_new ();
      Raw r = { b };
      raw_header (&r, GBK_SCAN_CHUNK, 3, 0);
      raw_le (&r, 0, 4);                  /* no removals */
      raw_le (&r, 1, 4);
      raw_entry (&r, bad_entries[i].name, bad_entries[i].len, bad_entries[i].kind,
                 bad_entries[i].flags);
      GbkFrame f = raw_frame (b);
      g_test_message ("%s", bad_entries[i].what);
      g_assert_null (gbk_proto_get_scan (&f, types));
    }

  const char *bad_removes[] = { "", ".", "..", "x/y" };
  for (size_t i = 0; i < G_N_ELEMENTS (bad_removes); i++)
    {
      g_autoptr (GByteArray) b = g_byte_array_new ();
      Raw r = { b };
      raw_header (&r, GBK_SCAN_DELTA, 0, 0);
      raw_le (&r, 1, 4);
      raw_name (&r, bad_removes[i], strlen (bad_removes[i]));
      raw_le (&r, 0, 4);
      GbkFrame f = raw_frame (b);
      g_assert_null (gbk_proto_get_scan (&f, types));
    }

  /* Structure: a chunk with removals, an error without an errno (and the reverse), a type
   * out of range, counts larger than the payload, and trailing bytes. */
  {
    g_autoptr (GByteArray) b = g_byte_array_new ();
    Raw r = { b };
    raw_header (&r, GBK_SCAN_CHUNK, 3, 0);
    raw_le (&r, 1, 4);
    raw_name (&r, "a", 1);
    raw_le (&r, 0, 4);
    GbkFrame f = raw_frame (b);
    g_assert_null (gbk_proto_get_scan (&f, types));
  }
  const struct
  {
    uint8_t type;
    uint32_t error;
  } bad_headers[]
      = { { GBK_SCAN_ERROR, 0 }, { GBK_SCAN_CHUNK, EIO }, { 4, 0 }, { GBK_SCAN_ERROR, 5000 } };
  for (size_t i = 0; i < G_N_ELEMENTS (bad_headers); i++)
    {
      g_autoptr (GByteArray) b = g_byte_array_new ();
      Raw r = { b };
      raw_header (&r, bad_headers[i].type, 0, bad_headers[i].error);
      raw_le (&r, 0, 4);
      raw_le (&r, 0, 4);
      GbkFrame f = raw_frame (b);
      g_assert_null (gbk_proto_get_scan (&f, types));
    }
  {
    g_autoptr (GByteArray) b = g_byte_array_new ();
    Raw r = { b };
    raw_header (&r, GBK_SCAN_CHUNK, 3, 0);
    raw_le (&r, 0, 4);
    raw_le (&r, 1000000, 4);             /* promises far more than follows */
    raw_entry (&r, "a", 1, 0, 0);
    GbkFrame f = raw_frame (b);
    g_assert_null (gbk_proto_get_scan (&f, types));
  }
  {
    g_autoptr (GByteArray) b = g_byte_array_new ();
    Raw r = { b };
    raw_header (&r, GBK_SCAN_CHUNK, 3, 0);
    raw_le (&r, 0, 4);
    raw_le (&r, 0, 4);
    raw_le (&r, 0, 1);                   /* one byte too many */
    GbkFrame f = raw_frame (b);
    g_assert_null (gbk_proto_get_scan (&f, types));
  }
}

/* A job: its sources (many: several frames), the job itself, then its questions, answers,
 * progress and the names it made, each round trip intact; what can't be is refused. */
static void
test_job_messages (void)
{
  g_autoptr (GPtrArray) sources = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *long_dir = g_strnfill (3000, 'd');
  for (int i = 0; i < 1000; i++)
    g_ptr_array_add (sources, g_strdup_printf ("/%s/%d", long_dir, i));
  char dest[] = "/x y", rel[] = "rel", absolute[] = "/x";
  GbkJobRequest r = { .kind = GBK_OP_MOVE, .sources_here = true, .session = 77, .dest = dest };
  g_autoptr (GByteArray) out = g_byte_array_new ();
  gbk_proto_put_job (out, 9, &r, sources);
  g_autoptr (GArray) frames = frames_of (out);
  g_assert_cmpuint (frames->len, >, 2);    /* the sources took several frames */
  g_autoptr (GPtrArray) got = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i + 1 < frames->len; i++)
    g_assert_true (gbk_proto_get_sources (&g_array_index (frames, GbkFrame, i), got));
  g_assert_cmpuint (got->len, ==, sources->len);
  for (guint i = 0; i < got->len; i++)
    g_assert_cmpstr (g_ptr_array_index (got, i), ==, g_ptr_array_index (sources, i));
  GbkJobRequest back;
  g_assert_true (gbk_proto_get_job (&g_array_index (frames, GbkFrame, frames->len - 1), &back));
  g_assert_cmpint (back.kind, ==, GBK_OP_MOVE);
  g_assert_true (back.sources_here);
  g_assert_cmpuint (back.session, ==, 77);
  g_assert_cmpstr (back.dest, ==, "/x y");
  gbk_job_request_clear (&back);

  /* A copy needs an absolute destination; a trash has none. */
  GbkJobRequest bad[] = { { .kind = GBK_OP_COPY, .dest = rel },
                          { .kind = GBK_OP_COPY, .dest = nullptr },
                          { .kind = GBK_OP_TRASH, .dest = absolute } };
  for (size_t i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_autoptr (GByteArray) b = g_byte_array_new ();
      gbk_proto_put_job (b, 9, &bad[i], sources);
      g_autoptr (GArray) fs = frames_of (b);
      g_assert_false (gbk_proto_get_job (&g_array_index (fs, GbkFrame, fs->len - 1), &back));
    }

  g_autoptr (GError) e = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "No");
  g_autoptr (GByteArray) q = g_byte_array_new ();
  gbk_proto_put_question (q, 9, GBK_ASK_CONFLICT, "/a", "/b", true, nullptr);
  gbk_proto_put_question (q, 9, GBK_ASK_ERROR, "/a", nullptr, false, e);
  gbk_proto_put_question (q, 9, GBK_ASK_UNTRASHABLE, "/a", nullptr, false, nullptr);
  g_autoptr (GArray) qs = frames_of (q);
  GbkAsk a;
  g_assert_true (gbk_proto_get_question (&g_array_index (qs, GbkFrame, 0), &a));
  g_assert_true (a.kind == GBK_ASK_CONFLICT && a.other_is_dir);
  g_assert_cmpstr (a.other, ==, "/b");
  gbk_ask_clear (&a);
  g_assert_true (gbk_proto_get_question (&g_array_index (qs, GbkFrame, 1), &a));
  g_assert_error (a.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_null (a.other);
  gbk_ask_clear (&a);
  g_assert_true (gbk_proto_get_question (&g_array_index (qs, GbkFrame, 2), &a));
  g_assert_true (a.kind == GBK_ASK_UNTRASHABLE && a.error == nullptr && a.other == nullptr);
  gbk_ask_clear (&a);

  g_autoptr (GByteArray) rest = g_byte_array_new ();
  gbk_proto_put_answer (rest, 9, GBK_CHOICE_KEEP_BOTH, true);
  gbk_proto_put_progress (rest, 9, 5, 10, "/now");
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (names, g_strdup ("made"));
  gbk_proto_put_created (rest, 9, names);
  gbk_proto_put_empty (rest, GBK_MSG_DONE, 9);
  g_autoptr (GArray) rs = frames_of (rest);
  GbkChoice ch;
  bool all;
  g_assert_true (gbk_proto_get_answer (&g_array_index (rs, GbkFrame, 0), &ch, &all));
  g_assert_true (ch == GBK_CHOICE_KEEP_BOTH && all);
  uint64_t done, total;
  g_autofree char *current = nullptr;
  g_assert_true (
      gbk_proto_get_progress (&g_array_index (rs, GbkFrame, 1), &done, &total, &current));
  g_assert_true (done == 5 && total == 10);
  g_assert_cmpstr (current, ==, "/now");
  g_autoptr (GPtrArray) made = g_ptr_array_new_with_free_func (g_free);
  g_assert_true (gbk_proto_get_created (&g_array_index (rs, GbkFrame, 2), made));
  g_assert_cmpstr (g_ptr_array_index (made, 0), ==, "made");
  g_assert_true (gbk_proto_get_empty (&g_array_index (rs, GbkFrame, 3)));

  /* A made name that isn't one name, and an empty list: refused. */
  g_free (g_ptr_array_index (names, 0));
  g_ptr_array_index (names, 0) = g_strdup ("a/b");
  g_autoptr (GByteArray) badname = g_byte_array_new ();
  gbk_proto_put_created (badname, 9, names);
  g_autoptr (GArray) keep = nullptr;
  g_assert_false (gbk_proto_get_created (only_frame (badname, &keep), made));
  g_assert_cmpuint (made->len, ==, 1);
  g_autoptr (GByteArray) none = g_byte_array_new ();
  g_autoptr (GPtrArray) empty = g_ptr_array_new ();
  gbk_proto_put_created (none, 9, empty);
  g_assert_cmpuint (none->len, ==, 0);   /* no names, no frame */
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  types = gbk_type_builder_finish (gbk_type_builder_new ());
  g_test_add_func ("/proto/magic", test_magic);
  g_test_add_func ("/proto/job-messages", test_job_messages);
  g_test_add_func ("/proto/framing", test_framing);
  g_test_add_func ("/proto/hello-path-failed", test_hello_path_failed);
  g_test_add_func ("/proto/info", test_info);
  g_test_add_func ("/proto/scan-round-trip", test_scan_round_trip);
  g_test_add_func ("/proto/scan-split", test_scan_split);
  g_test_add_func ("/proto/scan-refused", test_scan_refused);
  int status = g_test_run ();
  gbk_type_table_unref (types);
  return status;
}
