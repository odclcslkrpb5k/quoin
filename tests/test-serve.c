/* quoin-serve for real (QUOIN_SERVE names the binary): spawned with pipes, driven with the
 * protocol, as the laptop will drive it over ssh. */
#include "dirmodel.h"
#include "proto.h"

static GbkTypeTable *types;
static char *root;

typedef struct
{
  GPid pid;
  int in, out;          /* its stdin (we write), its stdout (we read) */
  GByteArray *buf;      /* read, not yet consumed */
  bool magic_seen;
} Server;

static Server *
server_start (void)
{
  g_autofree char *bin = g_strdup (g_getenv ("QUOIN_SERVE"));
  g_assert_nonnull (bin);
  char *argv[] = { bin, nullptr };
  Server *s = g_new0 (Server, 1);
  s->buf = g_byte_array_new ();
  g_autoptr (GError) error = nullptr;
  int err_fd = -1;
  g_assert_true (g_spawn_async_with_pipes (nullptr, argv, nullptr, G_SPAWN_DO_NOT_REAP_CHILD,
                                           nullptr, nullptr, &s->pid, &s->in, &s->out, &err_fd,
                                           &error));
  g_assert_no_error (error);
  close (err_fd);       /* its messages go nowhere; exit codes are what we check */
  return s;
}

/* Wait for it to exit; its exit status. */
static int
server_wait (const Server *s)
{
  int status = 0;
  int64_t deadline = g_get_monotonic_time () + INT64_C (10) * G_USEC_PER_SEC;
  for (;;)
    {
      pid_t r = waitpid (s->pid, &status, WNOHANG);
      if (r == s->pid)
        break;
      g_assert_cmpint (r, ==, 0);
      g_assert_cmpint (g_get_monotonic_time (), <, deadline);
      g_usleep (G_USEC_PER_SEC / 100);
    }
  g_assert_true (WIFEXITED (status));
  return WEXITSTATUS (status);
}

static void
server_free (Server *s)
{
  if (s->in >= 0)
    close (s->in);
  close (s->out);
  g_spawn_close_pid (s->pid);
  g_byte_array_unref (s->buf);
  g_free (s);
}

static void
send_bytes (const Server *s, const GByteArray *b)
{
  size_t at = 0;
  while (at < b->len)
    {
      ssize_t n = write (s->in, b->data + at, b->len - at);
      g_assert_cmpint (n, >, 0);
      at += (size_t) n;
    }
}

static void
send_path (const Server *s, GbkMsgType type, uint64_t channel, const char *path)
{
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_path (b, type, channel, path);
  send_bytes (s, b);
}

#define SESSION 5                        /* the session test requests are made in */

static void
send_request (const Server *s, GbkMsgType type, uint64_t channel, const char *path)
{
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_request (b, type, channel, SESSION, path);
  send_bytes (s, b);
}

static void
open_session (const Server *s, uint64_t session, bool record)
{
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_session (b, session, record);
  send_bytes (s, b);
}

/* Read more from the server, waiting up to 10 s. False at end of output. */
static bool
fill (Server *s)
{
  struct pollfd p = { s->out, POLLIN, 0 };
  g_assert_cmpint (poll (&p, 1, 10000), ==, 1);
  uint8_t chunk[65536];
  ssize_t n = read (s->out, chunk, sizeof chunk);
  g_assert_cmpint (n, >=, 0);
  g_byte_array_append (s->buf, chunk, (guint) n);
  return n > 0;
}

/* The next frame (copied: its payload points into `copy`). The magic line comes first. */
static GbkFrame
next (Server *s, GByteArray **copy)
{
  while (!s->magic_seen)
    {
      uint32_t version;
      g_autofree char *build = nullptr;
      size_t used;
      int r = gbk_proto_magic (s->buf->data, s->buf->len, &version, &build, &used);
      g_assert_cmpint (r, >=, 0);
      if (r == 1)
        {
          g_assert_cmpuint (version, ==, GBK_PROTO_VERSION);
          g_assert_cmpstr (build, ==, GBK_VERSION);
          g_byte_array_remove_range (s->buf, 0, (guint) used);
          s->magic_seen = true;
        }
      else
        g_assert_true (fill (s));
    }
  for (;;)
    {
      GbkFrame f;
      size_t used;
      int r = gbk_proto_next_frame (s->buf->data, s->buf->len, &f, &used);
      g_assert_cmpint (r, >=, 0);
      if (r == 1)
        {
          *copy = g_byte_array_new ();
          g_byte_array_append (*copy, f.payload, (guint) f.len);
          f.payload = (*copy)->data;
          g_byte_array_remove_range (s->buf, 0, (guint) used);
          return f;
        }
      g_assert_true (fill (s));
    }
}

static void
expect_hello (Server *s)
{
  g_autoptr (GByteArray) keep = nullptr;
  GbkFrame f = next (s, &keep);
  GbkHello h;
  g_assert_true (gbk_proto_get_hello (&f, &h));
  g_assert_cmpstr (h.build, ==, GBK_VERSION);
  g_assert_cmpuint (h.uid, ==, getuid ());
  g_assert_cmpstr (h.home, ==, g_get_home_dir ());
  gbk_hello_clear (&h);
}

/* Apply SCAN frames for `channel` to `m` until `done` says stop. */
static void
scan_until (Server *s, uint64_t channel, GbkDirModel *m, bool (*done) (GbkDirModel *, const void *),
            const void *data)
{
  while (!done (m, data))
    {
      g_autoptr (GByteArray) keep = nullptr;
      GbkFrame f = next (s, &keep);
      g_assert_cmpint (f.type, ==, GBK_MSG_SCAN);
      g_assert_cmpuint (f.channel, ==, channel);
      GbkScanMsg *msg = gbk_proto_get_scan (&f, types);
      g_assert_nonnull (msg);
      gbk_dir_model_apply_for_test (m, msg);
    }
}

static bool
settled (GbkDirModel *m, const void *data G_GNUC_UNUSED)
{
  return gbk_dir_model_state (m) != GBK_DIR_LOADING;
}

static bool
has (GbkDirModel *m, const void *name)
{
  return gbk_dir_model_find (m, name) != GBK_HANDLE_NONE;
}

static bool
lacks (GbkDirModel *m, const void *name)
{
  return gbk_dir_model_find (m, name) == GBK_HANDLE_NONE;
}

static char *
in_root (const char *rel)
{
  return g_build_filename (root, rel, nullptr);
}

static void
put (const char *rel, const char *content)
{
  g_autofree char *p = in_root (rel);
  g_autofree char *d = g_path_get_dirname (p);
  g_mkdir_with_parents (d, 0755);
  g_assert_true (g_file_set_contents (p, content, -1, nullptr));
}

static void
test_watch (void)
{
  put ("w/a.txt", "aaa");
  put ("w/sub/x", "");
  Server *s = server_start ();
  expect_hello (s);
  g_autofree char *dir = in_root ("w");
  send_path (s, GBK_MSG_WATCH, 1, dir);
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  scan_until (s, 1, m, settled, nullptr);
  g_assert_cmpint (gbk_dir_model_state (m), ==, GBK_DIR_READY);
  g_assert_true (gbk_dir_model_live (m));
  g_assert_cmpuint (gbk_dir_model_n_entries (m), ==, 2);
  const GbkEntry *a = gbk_dir_model_get (m, gbk_dir_model_find (m, "a.txt"));
  g_assert_cmpuint (a->size, ==, 3);
  g_assert_true (gbk_entry_is_dir (gbk_dir_model_get (m, gbk_dir_model_find (m, "sub"))));

  /* Live: a new file arrives, a removed one goes. */
  put ("w/new", "n");
  scan_until (s, 1, m, has, "new");
  g_autofree char *a_path = in_root ("w/a.txt");
  g_assert_cmpint (unlink (a_path), ==, 0);
  scan_until (s, 1, m, lacks, "a.txt");
  gbk_dir_model_check_order (m);

  /* Unwatching an unknown channel is harmless; a folder that isn't there is an error. */
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_empty (b, GBK_MSG_UNWATCH, 1);
  gbk_proto_put_empty (b, GBK_MSG_UNWATCH, 99);
  send_bytes (s, b);
  send_path (s, GBK_MSG_WATCH, 2, "/nonexistent/quoin/dir");
  g_autoptr (GbkDirModel) missing = gbk_dir_model_new_detached (types);
  scan_until (s, 2, missing, settled, nullptr);
  g_assert_cmpint (gbk_dir_model_state (missing), ==, GBK_DIR_ERROR);
  g_assert_cmpint (gbk_dir_model_error (missing), ==, ENOENT);

  close (s->in);
  s->in = -1;
  g_assert_cmpint (server_wait (s), ==, 0);   /* end of input: a clean exit */
  server_free (s);
}

static void
test_info_and_ping (void)
{
  put ("i/f", "12345");
  Server *s = server_start ();
  expect_hello (s);
  g_autofree char *f_path = in_root ("i/f");
  send_path (s, GBK_MSG_GET_INFO, 5, f_path);
  send_path (s, GBK_MSG_GET_INFO, 6, "/nonexistent/quoin/file");
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_empty (b, GBK_MSG_PING, 7);
  send_bytes (s, b);

  /* Answers come in any order: details are read on their own threads. */
  bool got_info = false, got_failed = false, got_pong = false;
  while (!(got_info && got_failed && got_pong))
    {
      g_autoptr (GByteArray) keep = nullptr;
      GbkFrame f = next (s, &keep);
      if (f.type == GBK_MSG_INFO)
        {
          g_assert_cmpuint (f.channel, ==, 5);
          g_autoptr (GbkFileInfo) info = gbk_proto_get_info (&f);
          g_assert_nonnull (info);
          g_assert_cmpuint (info->size, ==, 5);
          g_assert_true (S_ISREG (info->mode));
          got_info = true;
        }
      else if (f.type == GBK_MSG_FAILED)
        {
          g_assert_cmpuint (f.channel, ==, 6);
          g_autoptr (GError) e = nullptr;
          g_assert_true (gbk_proto_get_failed (&f, &e));
          g_assert_error (e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
          got_failed = true;
        }
      else
        {
          g_assert_cmpint (f.type, ==, GBK_MSG_PONG);
          g_assert_cmpuint (f.channel, ==, 7);
          g_assert_true (gbk_proto_get_empty (&f));
          got_pong = true;
        }
    }
  close (s->in);
  s->in = -1;
  g_assert_cmpint (server_wait (s), ==, 0);
  server_free (s);
}

static void
send_frame (const Server *s, void (*encode) (GByteArray *, const void *), const void *arg)
{
  g_autoptr (GByteArray) b = g_byte_array_new ();
  encode (b, arg);
  send_bytes (s, b);
}

typedef struct
{
  uint64_t channel;
  const char *path;
  uint32_t n;
} Req;

static void
put_read (GByteArray *b, const void *arg)
{
  const Req *r = arg;
  gbk_proto_put_read (b, r->channel, SESSION, r->path, r->n);
}

static void
put_credit (GByteArray *b, const void *arg)
{
  const Req *r = arg;
  gbk_proto_put_credit (b, r->channel, r->n);
}

static void
put_cancel (GByteArray *b, const void *arg)
{
  gbk_proto_put_empty (b, GBK_MSG_CANCEL, ((const Req *) arg)->channel);
}

/* LIST: every entry with its details, links with their targets; LSTAT doesn't follow. */
static void
test_list_and_lstat (void)
{
  put ("l/file", "12345");
  put ("l/sub/x", "");
  g_autofree char *link = in_root ("l/link");
  g_assert_cmpint (symlink ("file", link), ==, 0);
  Server *s = server_start ();
  expect_hello (s);
  open_session (s, SESSION, false);
  g_autofree char *dir = in_root ("l");
  send_request (s, GBK_MSG_LIST, 9, dir);
  g_autoptr (GByteArray) keep = nullptr;
  GbkFrame f = next (s, &keep);
  GPtrArray *entries;
  bool last;
  g_assert_true (gbk_proto_get_listing (&f, &entries, &last));
  g_assert_true (last);
  g_assert_cmpuint (entries->len, ==, 3);
  for (guint i = 0; i < entries->len; i++)
    {
      const GbkStatEntry *e = g_ptr_array_index (entries, i);
      if (g_str_equal (e->name, "file"))
        g_assert_cmpuint (e->size, ==, 5);
      else if (g_str_equal (e->name, "link"))
        {
          g_assert_true (S_ISLNK (e->mode));
          g_assert_cmpstr (e->target, ==, "file");
        }
      else
        g_assert_true (S_ISDIR (e->mode) && g_str_equal (e->name, "sub"));
    }
  g_ptr_array_unref (entries);

  send_request (s, GBK_MSG_LSTAT, 10, link);
  g_autoptr (GByteArray) keep2 = nullptr;
  GbkFrame f2 = next (s, &keep2);
  g_autoptr (GbkStatEntry) e = gbk_proto_get_stated (&f2);
  g_assert_nonnull (e);
  g_assert_true (S_ISLNK (e->mode));

  send_request (s, GBK_MSG_LIST, 11, "/nonexistent/quoin/dir");
  g_autoptr (GByteArray) keep3 = nullptr;
  GbkFrame f3 = next (s, &keep3);
  g_autoptr (GError) err = nullptr;
  g_assert_true (gbk_proto_get_failed (&f3, &err));
  g_assert_error (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);

  /* Outside a session it opened: refused, and still serving. */
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_request (b, GBK_MSG_LIST, 12, SESSION + 1, dir);
  send_bytes (s, b);
  g_autoptr (GByteArray) keep4 = nullptr;
  GbkFrame f4 = next (s, &keep4);
  g_autoptr (GError) err4 = nullptr;
  g_assert_true (gbk_proto_get_failed (&f4, &err4));
  close (s->in);
  s->in = -1;
  g_assert_cmpint (server_wait (s), ==, 0);
  server_free (s);
}

/* READ sends no more than the credit allows; more credit, more data; at the end, whether the
 * file changed meanwhile (here it did: it grew while waiting for credit). */
static void
test_read_credit_and_change (void)
{
  put ("r/data", "0123456789");
  g_autofree char *path = in_root ("r/data");
  Server *s = server_start ();
  expect_hello (s);
  open_session (s, SESSION, false);
  Req r = { 20, path, 4 };
  send_frame (s, put_read, &r);
  g_autoptr (GByteArray) k1 = nullptr;
  GbkFrame f1 = next (s, &k1);
  const uint8_t *d;
  size_t n;
  g_assert_true (gbk_proto_get_data (&f1, &d, &n));
  g_assert_cmpuint (n, ==, 4);
  g_assert_cmpmem (d, n, "0123", 4);
  /* Nothing more comes without credit. */
  struct pollfd p = { s->out, POLLIN, 0 };
  g_assert_cmpint (poll (&p, 1, 300), ==, 0);
  /* It changes now; the rest is read with more credit, and the end says so. */
  g_autofree char *grown = g_strdup ("0123456789abc");
  g_assert_true (g_file_set_contents (path, grown, -1, nullptr));
  Req more = { 20, path, 1000 };
  send_frame (s, put_credit, &more);
  bool changed = false, ended = false;
  while (!ended)
    {
      g_autoptr (GByteArray) k = nullptr;
      GbkFrame f = next (s, &k);
      if (!gbk_proto_get_data (&f, &d, &n))
        {
          g_assert_true (gbk_proto_get_end (&f, &changed));
          ended = true;
        }
    }
  g_assert_true (changed);

  /* An unchanged file ends unchanged; CANCEL stops one waiting for credit. */
  Req again = { 21, path, 1000 };
  send_frame (s, put_read, &again);
  for (bool done = false; !done;)
    {
      g_autoptr (GByteArray) k = nullptr;
      GbkFrame f = next (s, &k);
      done = gbk_proto_get_end (&f, &changed);
    }
  g_assert_false (changed);
  Req stalled = { 22, path, 0 };
  send_frame (s, put_read, &stalled);
  send_frame (s, put_cancel, &stalled);
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_empty (b, GBK_MSG_PING, 23);
  send_bytes (s, b);
  g_autoptr (GByteArray) k3 = nullptr;
  GbkFrame pong = next (s, &k3);
  g_assert_cmpint (pong.type, ==, GBK_MSG_PONG);  /* still serving; the read said nothing */

  /* Not a regular file: refused. */
  g_autofree char *dir = in_root ("r");
  Req bad = { 24, dir, 1000 };
  send_frame (s, put_read, &bad);
  g_autoptr (GByteArray) k4 = nullptr;
  GbkFrame f4 = next (s, &k4);
  g_autoptr (GError) err = nullptr;
  g_assert_true (gbk_proto_get_failed (&f4, &err));
  close (s->in);
  s->in = -1;
  g_assert_cmpint (server_wait (s), ==, 0);
  server_free (s);
}

static GError *
expect_removed (Server *s, uint64_t channel, const char *path)
{
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_request (b, GBK_MSG_REMOVE, channel, SESSION, path);
  send_bytes (s, b);
  g_autoptr (GByteArray) k = nullptr;
  GbkFrame f = next (s, &k);
  g_assert_cmpuint (f.channel, ==, channel);
  GError *error = nullptr;
  if (f.type != GBK_MSG_REMOVED)
    g_assert_true (gbk_proto_get_failed (&f, &error));
  return error;
}

static void
listed (Server *s, GbkMsgType type, uint64_t channel, const char *path)
{
  send_request (s, type, channel, path);
  g_autoptr (GByteArray) k = nullptr;
  GbkFrame f = next (s, &k);
  g_assert_true (f.type == (type == GBK_MSG_LIST ? GBK_MSG_LISTING : GBK_MSG_STATED));
}

/* What a move copied is removed only where it is as it was listed: a file changed since is
 * kept (and so is its folder), and nothing is removed outside a recording session. */
static void
test_remove_unchanged (void)
{
  put ("m/a/same", "1");
  put ("m/a/edited", "2");
  put ("m/b", "3");
  g_autofree char *a = in_root ("m/a");
  g_autofree char *b = in_root ("m/b");
  g_autofree char *edited = in_root ("m/a/edited");
  g_autofree char *same = in_root ("m/a/same");
  Server *s = server_start ();
  expect_hello (s);

  open_session (s, SESSION, false);
  listed (s, GBK_MSG_LSTAT, 30, b);
  g_autoptr (GError) refused = expect_removed (s, 31, b);
  g_assert_error (refused, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_assert_true (g_file_test (b, G_FILE_TEST_EXISTS));
  g_autoptr (GByteArray) forget = g_byte_array_new ();
  gbk_proto_put_empty (forget, GBK_MSG_FORGET, SESSION);
  send_bytes (s, forget);

  open_session (s, SESSION, true);
  listed (s, GBK_MSG_LSTAT, 32, a);
  listed (s, GBK_MSG_LIST, 33, a);
  listed (s, GBK_MSG_LSTAT, 34, b);
  g_usleep (20000);                      /* a later mtime, on any filesystem */
  g_assert_true (g_file_set_contents (edited, "changed", -1, nullptr));
  g_autoptr (GError) kept = expect_removed (s, 35, a);
  g_assert_error (kept, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_false (g_file_test (same, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (edited, G_FILE_TEST_EXISTS));
  g_autoptr (GError) none = expect_removed (s, 36, b);
  g_assert_no_error (none);
  g_assert_false (g_file_test (b, G_FILE_TEST_EXISTS));
  close (s->in);
  s->in = -1;
  g_assert_cmpint (server_wait (s), ==, 0);
  server_free (s);
}

/* Anything it doesn't understand ends it (exit status 3): it never guesses. */
static void
test_protocol_errors (void)
{
  for (int i = 0; i < 5; i++)
    {
      Server *s = server_start ();
      expect_hello (s);
      g_autoptr (GByteArray) b = g_byte_array_new ();
      switch (i)
        {
        case 0: g_byte_array_append (b, (const guint8 *) "garbage, not a frame", 20); break;
        case 1: gbk_proto_put_path (b, GBK_MSG_WATCH, 0, "/tmp"); break;     /* channel 0 */
        case 2:
          gbk_proto_put_path (b, GBK_MSG_WATCH, 3, "/tmp");
          gbk_proto_put_path (b, GBK_MSG_WATCH, 3, "/tmp");                  /* in use */
          break;
        case 3: gbk_proto_put_path (b, GBK_MSG_WATCH, 4, "tmp"); break;      /* relative */
        default: gbk_proto_put_empty (b, GBK_MSG_PONG, 0); break;            /* server's own */
        }
      send_bytes (s, b);
      g_assert_cmpint (server_wait (s), ==, 3);
      server_free (s);
    }
}

/* Run quoin-serve with `args` (nullptr-terminated) and the environment `envp`. */
/* Silence ends it (status 4); anything at all, a ping included, keeps it going. */
static void
test_idle (void)
{
  g_setenv ("GBK_SERVE_IDLE_MS", "300", true);
  Server *s = server_start ();
  g_unsetenv ("GBK_SERVE_IDLE_MS");
  expect_hello (s);
  for (int i = 0; i < 6; i++)          /* 0.6 s of pings, every 0.1 s: still there */
    {
      g_autoptr (GByteArray) b = g_byte_array_new ();
      gbk_proto_put_empty (b, GBK_MSG_PING, (uint64_t) i + 1);
      send_bytes (s, b);
      g_autoptr (GByteArray) keep = nullptr;
      GbkFrame f = next (s, &keep);
      g_assert_cmpint (f.type, ==, GBK_MSG_PONG);
      g_usleep (G_USEC_PER_SEC / 10);
    }
  int64_t start = g_get_monotonic_time ();
  g_assert_cmpint (server_wait (s), ==, 4);
  g_assert_cmpint (g_get_monotonic_time () - start, <, INT64_C (2) * G_USEC_PER_SEC);
  server_free (s);
}

static int
run_sync (const char *const *args, char **envp, char **out)
{
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  g_strv_builder_add (b, g_getenv ("QUOIN_SERVE"));
  for (size_t i = 0; args[i] != nullptr; i++)
    g_strv_builder_add (b, args[i]);
  g_auto (GStrv) argv = g_strv_builder_end (b);
  int status = 0;
  g_autoptr (GError) error = nullptr;
  /* stdin from /dev/null: the shell reads its commands from it, and ends at once. */
  g_assert_true (g_spawn_sync (nullptr, argv, envp, G_SPAWN_STDIN_FROM_DEV_NULL, nullptr, nullptr,
                               out, nullptr, &status, &error));
  g_assert_no_error (error);
  g_assert_true (WIFEXITED (status));
  return WEXITSTATUS (status);
}

static char *
hex (const char *bytes)
{
  GString *s = g_string_new ("");
  for (const char *c = bytes; *c != '\0'; c++)
    g_string_append_printf (s, "%02x", (unsigned) (unsigned char) *c);
  return g_string_free (s, false);
}

/* --shell-at: the folder arrives hex-encoded, so a name full of shell syntax is just a name;
 * the shell starts there, as a login shell. */
static void
test_shell_at (void)
{
  put ("sh/x';touch pwned;'$(id)/keep", "");
  g_autofree char *odd = in_root ("sh/x';touch pwned;'$(id)");
  g_autofree char *report = in_root ("sh/report");
  /* The "shell" is /bin/sh under another name, with its own home: a login shell (argv[0]
   * starting with '-') reads ~/.profile, which reports where it started and as what. A
   * script can't stand in: the kernel replaces a script's argv[0] with its path. */
  g_autofree char *shell = in_root ("fake-shell");
  g_assert_cmpint (symlink ("/bin/sh", shell), ==, 0);
  g_autofree char *home = in_root ("sh-home");
  g_mkdir_with_parents (home, 0755);
  g_autofree char *profile = g_build_filename (home, ".profile", nullptr);
  g_autofree char *script
      = g_strdup_printf ("printf '%%s\\n%%s\\n' \"$(pwd)\" \"$0\" > '%s'\n", report);
  g_assert_true (g_file_set_contents (profile, script, -1, nullptr));

  g_autofree char *arg = hex (odd);
  g_autofree char *shell_env = g_strconcat ("SHELL=", shell, nullptr);
  g_autofree char *home_env = g_strconcat ("HOME=", home, nullptr);
  g_autofree char *path_env = g_strdup ("PATH=/usr/bin:/bin");
  char *envp[] = { shell_env, home_env, path_env, nullptr };
  const char *args[] = { "--shell-at", arg, nullptr };
  g_assert_cmpint (run_sync (args, envp, nullptr), ==, 0);
  g_autofree char *got = nullptr;
  g_assert_true (g_file_get_contents (report, &got, nullptr, nullptr));
  g_autofree char *want = g_strdup_printf ("%s\n-fake-shell\n", odd);
  g_assert_cmpstr (got, ==, want);
  g_autofree char *pwned = in_root ("sh/pwned");
  g_assert_false (g_file_test (pwned, G_FILE_TEST_EXISTS));

  /* Not hex, odd length, a NUL byte: refused. */
  const char *bad[] = { "zz", "abc", "2f00", "" };
  for (size_t i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      const char *bad_args[] = { "--shell-at", bad[i], nullptr };
      g_assert_cmpint (run_sync (bad_args, envp, nullptr), ==, 2);
    }

  const char *version_args[] = { "--version", nullptr };
  g_autofree char *version = nullptr;
  g_assert_cmpint (run_sync (version_args, envp, &version), ==, 0);
  g_assert_true (g_str_has_prefix (version, "quoin-serve " GBK_VERSION));
}

int
main (int argc, char **argv)
{
  signal (SIGPIPE, SIG_IGN);     /* a server that already quit is checked by its exit status */
  root = g_dir_make_tmp ("gbk-serve-XXXXXX", nullptr);
  types = gbk_type_builder_finish (gbk_type_builder_new ());
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/serve/watch", test_watch);
  g_test_add_func ("/serve/info-and-ping", test_info_and_ping);
  g_test_add_func ("/serve/protocol-errors", test_protocol_errors);
  g_test_add_func ("/serve/shell-at", test_shell_at);
  g_test_add_func ("/serve/idle", test_idle);
  g_test_add_func ("/serve/list-and-lstat", test_list_and_lstat);
  g_test_add_func ("/serve/read-credit-and-change", test_read_credit_and_change);
  g_test_add_func ("/serve/remove-unchanged", test_remove_unchanged);
  int status = g_test_run ();
  gbk_type_table_unref (types);
  g_free (root);
  return status;
}
