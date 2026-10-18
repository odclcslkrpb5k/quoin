/* Answering the other machine's requests on this one (peer.h), in-process: a restricted
 * answerer (the laptop's) answers only inside sessions it opened, only about the files given
 * to them and what is inside them, never through "..", a symlink or a longer name that
 * merely starts the same; and a server can't open, close or widen sessions here. */
#include "peer.h"

static char *dir;

static char *
in_dir (const char *rel)
{
  return g_build_filename (dir, rel, nullptr);
}

static void
put (const char *rel, const char *content)
{
  g_autofree char *p = in_dir (rel);
  g_autofree char *d = g_path_get_dirname (p);
  g_mkdir_with_parents (d, 0755);
  g_assert_true (g_file_set_contents (p, content, -1, nullptr));
}

typedef struct
{
  GbkOutbox *out;
  GbkAnswerer *a;
  uint64_t session;
  uint64_t next_channel;
} Peer;

/* Hand `frames` to the answerer as if they had arrived; false if any was refused. */
static bool
deliver (Peer *p, const GByteArray *frames)
{
  size_t at = 0;
  bool ok = true;
  while (at < frames->len)
    {
      GbkFrame f;
      size_t used;
      g_assert_cmpint (gbk_proto_next_frame (frames->data + at, frames->len - at, &f, &used), ==,
                       1);
      g_assert_true (gbk_answerer_takes (f.type));
      ok = gbk_answerer_handle (p->a, &f) && ok;
      at += used;
    }
  return ok;
}

static bool
idle (Peer *p)
{
  for (int i = 0; i < 2000 && gbk_answerer_busy (p->a) > 0; i++)
    g_usleep (1000);
  return gbk_answerer_busy (p->a) == 0;
}

/* The answer frames for `channel`, as a copy of their bytes (the outbox is emptied). */
static GByteArray *
answers (Peer *p, uint64_t channel)
{
  g_assert_true (idle (p));
  GByteArray *all = gbk_outbox_begin (p->out);
  GByteArray *mine = g_byte_array_new ();
  g_autoptr (GByteArray) rest = g_byte_array_new ();
  size_t at = 0;
  while (at < all->len)
    {
      GbkFrame f;
      size_t used;
      g_assert_cmpint (gbk_proto_next_frame (all->data + at, all->len - at, &f, &used), ==, 1);
      g_byte_array_append (f.channel == channel ? mine : rest, all->data + at, (guint) used);
      at += used;
    }
  g_byte_array_set_size (all, 0);
  g_byte_array_append (all, rest->data, rest->len);
  gbk_outbox_end (p->out);
  return mine;
}

static GbkFrame
first (const GByteArray *frames)
{
  GbkFrame f;
  size_t used;
  g_assert_cmpint (gbk_proto_next_frame (frames->data, frames->len, &f, &used), ==, 1);
  return f;
}

/* Ask `type` about `rel` (absolute if it starts with '/') in `session`: the first answer's
 * type; a failure's code in *code. */
static GbkMsgType
ask (Peer *p, GbkMsgType type, uint64_t session, const char *rel, int *code)
{
  g_autofree char *path = rel[0] == '/' ? g_strdup (rel) : in_dir (rel);
  uint64_t ch = p->next_channel++;
  g_autoptr (GByteArray) req = g_byte_array_new ();
  if (type == GBK_MSG_READ)
    gbk_proto_put_read (req, ch, session, path, 1 << 20);
  else
    gbk_proto_put_request (req, type, ch, session, path);
  g_assert_true (deliver (p, req));
  g_autoptr (GByteArray) got = answers (p, ch);
  g_assert_cmpuint (got->len, >, 0);
  GbkFrame f = first (got);
  *code = -1;
  g_autoptr (GError) e = nullptr;
  if (f.type == GBK_MSG_FAILED && gbk_proto_get_failed (&f, &e))
    *code = e->code;
  return f.type;
}

static void
assert_refused (Peer *p, GbkMsgType type, const char *rel)
{
  int code;
  g_assert_cmpint (ask (p, type, p->session, rel, &code), ==, GBK_MSG_FAILED);
  g_test_message ("%s refused (code %d)", rel, code);
}

static void
test_restricted (void)
{
  put ("job/f", "abc");
  put ("job/sub/g", "g");
  put ("secret/s", "secret");
  put ("other", "x");
  put ("jobber/x", "x");
  g_autofree char *secret = in_dir ("secret");
  g_autofree char *out_link = in_dir ("job/out");
  g_assert_cmpint (symlink (secret, out_link), ==, 0);
  g_autofree char *up_link = in_dir ("job/up");
  g_assert_cmpint (symlink ("..", up_link), ==, 0);

  Peer p = { .out = gbk_outbox_new (), .next_channel = 100 };
  p.a = gbk_answerer_new (true, p.out);
  g_autoptr (GPtrArray) roots = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (roots, in_dir ("job"));
  p.session = gbk_answerer_open (p.a, roots, false);

  /* Inside: answered. */
  int code;
  g_assert_cmpint (ask (&p, GBK_MSG_LIST, p.session, "job", &code), ==, GBK_MSG_LISTING);
  g_assert_cmpint (ask (&p, GBK_MSG_LSTAT, p.session, "job/sub/g", &code), ==, GBK_MSG_STATED);
  g_assert_cmpint (ask (&p, GBK_MSG_READ, p.session, "job/f", &code), ==, GBK_MSG_DATA);
  /* A link inside is described, never followed. */
  g_assert_cmpint (ask (&p, GBK_MSG_LSTAT, p.session, "job/out", &code), ==, GBK_MSG_STATED);

  /* Outside, by every route. */
  assert_refused (&p, GBK_MSG_LIST, "");                  /* the folder above */
  assert_refused (&p, GBK_MSG_LSTAT, "other");
  assert_refused (&p, GBK_MSG_READ, "secret/s");
  assert_refused (&p, GBK_MSG_LIST, "jobber");            /* starts like it, isn't it */
  assert_refused (&p, GBK_MSG_READ, "jobber/x");
  assert_refused (&p, GBK_MSG_READ, "job/../other");
  assert_refused (&p, GBK_MSG_READ, "job/./f");
  assert_refused (&p, GBK_MSG_READ, "job//f");
  assert_refused (&p, GBK_MSG_LIST, "job/out");           /* through a link */
  assert_refused (&p, GBK_MSG_READ, "job/out/s");
  assert_refused (&p, GBK_MSG_LIST, "job/up");
  assert_refused (&p, GBK_MSG_READ, "job/up/other");
  assert_refused (&p, GBK_MSG_LSTAT, "/");
  /* Another session, or none. */
  g_assert_cmpint (ask (&p, GBK_MSG_LIST, p.session + 12345, "job", &code), ==, GBK_MSG_FAILED);
  /* Nothing is removed outside a move's session. */
  assert_refused (&p, GBK_MSG_REMOVE, "job/f");
  g_autofree char *f = in_dir ("job/f");
  g_assert_true (g_file_test (f, G_FILE_TEST_EXISTS));

  /* The other machine can't open, close or ask outside sessions: that ends the connection. */
  g_autoptr (GByteArray) open = g_byte_array_new ();
  gbk_proto_put_session (open, 5, false);
  g_assert_false (deliver (&p, open));
  g_autoptr (GByteArray) forget = g_byte_array_new ();
  gbk_proto_put_empty (forget, GBK_MSG_FORGET, p.session);
  g_assert_false (deliver (&p, forget));
  g_autoptr (GByteArray) info = g_byte_array_new ();
  gbk_proto_put_path (info, GBK_MSG_GET_INFO, 7, f);
  g_assert_false (deliver (&p, info));

  /* Closed: nothing more. */
  gbk_answerer_close (p.a, p.session);
  assert_refused (&p, GBK_MSG_LIST, "job");
  g_assert_true (idle (&p));
  gbk_answerer_stop (p.a);
  gbk_answerer_unref (p.a);
  gbk_outbox_unref (p.out);
}

/* A move's session removes what was listed and is unchanged, and keeps the rest. */
static void
test_restricted_remove (void)
{
  put ("mv/keep", "1");
  put ("mv/go", "2");
  Peer p = { .out = gbk_outbox_new (), .next_channel = 500 };
  p.a = gbk_answerer_new (true, p.out);
  g_autoptr (GPtrArray) roots = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (roots, in_dir ("mv"));
  p.session = gbk_answerer_open (p.a, roots, true);
  int code;
  g_assert_cmpint (ask (&p, GBK_MSG_LSTAT, p.session, "mv", &code), ==, GBK_MSG_STATED);
  g_assert_cmpint (ask (&p, GBK_MSG_LIST, p.session, "mv", &code), ==, GBK_MSG_LISTING);
  put ("mv/new", "arrived after the listing");
  g_usleep (20000);
  put ("mv/keep", "changed");
  g_assert_cmpint (ask (&p, GBK_MSG_REMOVE, p.session, "mv", &code), ==, GBK_MSG_FAILED);
  g_autofree char *keep = in_dir ("mv/keep");
  g_autofree char *go = in_dir ("mv/go");
  g_autofree char *fresh = in_dir ("mv/new");
  g_assert_true (g_file_test (keep, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (fresh, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (go, G_FILE_TEST_EXISTS));
  gbk_answerer_close (p.a, p.session);
  gbk_answerer_stop (p.a);
  gbk_answerer_unref (p.a);
  gbk_outbox_unref (p.out);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  dir = g_dir_make_tmp ("gbk-peer-XXXXXX", nullptr);
  g_test_add_func ("/peer/restricted", test_restricted);
  g_test_add_func ("/peer/restricted-remove", test_restricted_remove);
  return g_test_run ();
}
