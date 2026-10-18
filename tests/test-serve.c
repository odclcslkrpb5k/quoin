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
  int status = g_test_run ();
  gbk_type_table_unref (types);
  g_free (root);
  return status;
}
