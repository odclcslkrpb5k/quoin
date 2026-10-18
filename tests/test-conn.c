/* Connections, end to end, with tests/fake-ssh standing in for ssh: the first connection
 * sends quoin-serve over; later ones use it; a refused key, a chatty .bashrc, a hung
 * network, a killed server and a frozen one (a laptop asleep) each end the connection with
 * the right message, and no request is left unanswered. */
#include "conn.h"
#include "dirmodel.h"
#include "lifetime.h"

static char *home;
static char *log_path;
static char *pid_path;
static char *mode_path;
static GbkTypeTable *types;

static bool
wait_until (bool (*cond) (void *), void *data, int ms)
{
  int64_t deadline = g_get_monotonic_time () + (int64_t) ms * 1000;
  while (!cond (data))
    {
      if (g_get_monotonic_time () > deadline)
        return false;
      g_main_context_iteration (nullptr, false);
      g_usleep (1000);
    }
  return true;
}

static bool
settled (void *c)
{
  return gbk_conn_state (c) != GBK_CONN_CONNECTING;
}

static bool
is_dead (void *c)
{
  return gbk_conn_state (c) == GBK_CONN_DEAD;
}

static bool
no_conns (void *data G_GNUC_UNUSED)
{
  return gbk_conn_live () == 0;
}

static GbkConn *
connect_to (const char *host)
{
  g_autoptr (GError) error = nullptr;
  GbkConn *c = gbk_conn_new (host, types, &error);
  g_assert_no_error (error);
  g_assert_true (wait_until (settled, c, 20000));
  return c;
}

/* Lines in the fake ssh's log (one per ssh run). */
static unsigned
runs (void)
{
  g_autofree char *text = nullptr;
  if (!g_file_get_contents (log_path, &text, nullptr, nullptr))
    return 0;
  unsigned n = 0;
  for (const char *c = text; *c != '\0'; c++)
    n += *c == '\n';
  return n;
}

static char *
installed (void)
{
  return g_build_filename (home, ".cache", "quoin", "serve", GBK_VERSION, "quoin-serve", nullptr);
}

static pid_t
server_pid (void)
{
  g_autofree char *text = nullptr;
  g_assert_true (g_file_get_contents (pid_path, &text, nullptr, nullptr));
  int64_t pid = 0;
  g_assert_true (g_ascii_string_to_signed (g_strstrip (text), 10, 2, INT32_MAX, &pid, nullptr));
  return (pid_t) pid;
}

typedef struct
{
  GbkDirModel *m;
  bool ended;
} Watching;

static void
on_scan (GbkScanMsg *msg, void *data)
{
  Watching *w = data;
  if (msg == nullptr)
    w->ended = true;
  else
    gbk_dir_model_apply_for_test (w->m, msg);
}

static bool
listed (void *data)
{
  const Watching *w = data;
  return gbk_dir_model_state (w->m) == GBK_DIR_READY;
}

static bool
has_new (void *data)
{
  const Watching *w = data;
  return gbk_dir_model_find (w->m, "new") != GBK_HANDLE_NONE;
}

static bool
watch_ended (void *data)
{
  return ((const Watching *) data)->ended;
}

typedef struct
{
  int calls;
  uint64_t size;
  GError *error;
} Answer;

static void
on_info (GbkFileInfo *info, const GError *error, void *data)
{
  Answer *a = data;
  a->calls++;
  if (info != nullptr)
    a->size = info->size;
  if (error != nullptr)
    a->error = g_error_copy (error);
  gbk_file_info_free (info);
}

static bool
answered (void *data)
{
  return ((const Answer *) data)->calls > 0;
}

static void
put (const char *rel, const char *content)
{
  g_autofree char *p = g_build_filename (home, rel, nullptr);
  g_autofree char *d = g_path_get_dirname (p);
  g_mkdir_with_parents (d, 0755);
  g_assert_true (g_file_set_contents (p, content, -1, nullptr));
}

/* First contact: quoin-serve isn't there, so it is sent, then started. */
static void
test_install_then_use (void)
{
  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);
  put ("data/a.txt", "aaa");
  GbkConn *c = connect_to ("ws");
  g_assert_cmpint (gbk_conn_state (c), ==, GBK_CONN_READY);
  g_assert_cmpuint (runs (), ==, 3);     /* not there (127), install, start */
  g_autofree char *bin = installed ();
  struct stat st;
  g_assert_cmpint (stat (bin, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);
  const GbkHello *h = gbk_conn_hello (c);
  g_assert_nonnull (h);
  g_assert_cmpstr (h->home, ==, home);
  g_assert_cmpuint (h->uid, ==, getuid ());
  g_autofree char *mine = gbk_conn_local_machine_id ();
  g_assert_cmpint (gbk_conn_is_this_machine (c), ==, mine[0] != '\0');

  /* A watch: the listing, then live changes. */
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  Watching w = { m, false };
  g_autofree char *data_dir = g_build_filename (home, "data", nullptr);
  GbkConnWatch *watch = gbk_conn_watch (c, data_dir, owner, on_scan, &w);
  g_assert_true (wait_until (listed, &w, 10000));
  g_assert_cmpuint (gbk_dir_model_find (m, "a.txt"), !=, GBK_HANDLE_NONE);
  put ("data/new", "n");
  g_assert_true (wait_until (has_new, &w, 10000));

  /* Details. */
  Answer a = { 0 };
  g_autofree char *a_path = g_build_filename (home, "data", "a.txt", nullptr);
  gbk_conn_info (c, a_path, owner, on_info, &a);
  g_assert_true (wait_until (answered, &a, 10000));
  g_assert_no_error (a.error);
  g_assert_cmpuint (a.size, ==, 3);

  /* Closing: the watch is told, and nothing is left running. */
  gbk_conn_close (c);
  g_assert_true (wait_until (is_dead, c, 10000));
  g_assert_true (w.ended);
  gbk_conn_unwatch (watch);
  g_object_unref (c);
  g_assert_true (wait_until (no_conns, nullptr, 10000));

  /* The second time it's there already: one run. */
  unsigned before = runs ();
  GbkConn *again = connect_to ("ws");
  g_assert_cmpint (gbk_conn_state (again), ==, GBK_CONN_READY);
  g_assert_cmpuint (runs (), ==, before + 1);
  g_object_unref (again);
  g_assert_true (wait_until (no_conns, nullptr, 10000));
}

/* Another build answering at that path is replaced, not used. */
static void
test_other_build_replaced (void)
{
  g_autofree char *bin = installed ();
  g_autofree char *dir = g_path_get_dirname (bin);
  g_mkdir_with_parents (dir, 0700);
  g_assert_true (
      g_file_set_contents (bin, "#!/bin/sh\necho 'QUOIN-SERVE 1 someotherbuild'\n", -1, nullptr));
  g_assert_cmpint (chmod (bin, 0700), ==, 0);
  GbkConn *c = connect_to ("ws");
  g_assert_cmpstr (gbk_conn_error (c), ==, nullptr);
  g_assert_cmpint (gbk_conn_state (c), ==, GBK_CONN_READY);
  struct stat st;
  g_assert_cmpint (stat (bin, &st), ==, 0);
  g_assert_cmpint (st.st_size, >, 1000);   /* the real thing now */
  g_object_unref (c);
  g_assert_true (wait_until (no_conns, nullptr, 10000));
}

static GbkConn *
connect_in_mode (const char *mode)
{
  g_assert_true (g_file_set_contents (mode_path, mode, -1, nullptr));
  GbkConn *c = connect_to ("ws");
  g_assert_cmpint (unlink (mode_path), ==, 0);
  return c;
}

static void
test_refusals (void)
{
  /* A refused key: ssh's own words, and a hint that logging in once may fix it. */
  GbkConn *denied = connect_in_mode ("deny");
  g_assert_cmpint (gbk_conn_state (denied), ==, GBK_CONN_DEAD);
  g_assert_nonnull (strstr (gbk_conn_error (denied), "Permission denied"));
  g_assert_true (gbk_conn_needs_login (denied));
  g_object_unref (denied);

  /* A .bashrc that prints: named, never skipped over. */
  GbkConn *noisy = connect_in_mode ("noisy");
  g_assert_cmpint (gbk_conn_state (noisy), ==, GBK_CONN_DEAD);
  g_assert_nonnull (strstr (gbk_conn_error (noisy), "Welcome to ws!"));
  g_assert_false (gbk_conn_needs_login (noisy));
  g_object_unref (noisy);

  /* No answer at all: given up on in time. */
  gbk_conn_set_timeouts_for_test (0, 0, 500);
  int64_t start = g_get_monotonic_time ();
  GbkConn *hung = connect_in_mode ("hang");
  gbk_conn_set_timeouts_for_test (0, 0, 0);
  g_assert_cmpint (gbk_conn_state (hung), ==, GBK_CONN_DEAD);
  g_assert_cmpint (g_get_monotonic_time () - start, <, INT64_C (5) * G_USEC_PER_SEC);
  g_assert_nonnull (strstr (gbk_conn_error (hung), "didn't answer"));
  g_object_unref (hung);

  /* A host that would be an option to ssh never reaches it. */
  unsigned before = runs ();
  const char *bad[] = { "-oProxyCommand=touch /tmp/x", "", "a b", "ws;id", "ws\n" };
  for (size_t i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      g_assert_false (gbk_host_valid (bad[i]));
      g_autoptr (GError) error = nullptr;
      g_assert_null (gbk_conn_new (bad[i], types, &error));
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    }
  g_assert_cmpuint (runs (), ==, before);
  g_assert_true (gbk_host_valid ("gbk@ws1.local"));
  g_assert_true (gbk_host_valid ("[::1]"));
  g_assert_true (wait_until (no_conns, nullptr, 10000));
}

/* The link dies under a watch and a pending request: both are told. */
static void
test_server_killed (void)
{
  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);
  GbkConn *c = connect_to ("ws");
  g_assert_cmpint (gbk_conn_state (c), ==, GBK_CONN_READY);
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  Watching w = { m, false };
  GbkConnWatch *watch = gbk_conn_watch (c, home, owner, on_scan, &w);
  g_assert_true (wait_until (listed, &w, 10000));

  g_assert_cmpint (kill (server_pid (), SIGKILL), ==, 0);
  Answer a = { 0 };
  gbk_conn_info (c, home, owner, on_info, &a);
  g_assert_true (wait_until (is_dead, c, 10000));
  g_assert_true (wait_until (answered, &a, 10000));
  g_assert_cmpint (a.calls, ==, 1);
  g_assert_nonnull (a.error);
  g_clear_error (&a.error);
  g_assert_true (wait_until (watch_ended, &w, 10000));

  /* Requests on a dead connection are answered too, later, never in the call. */
  Answer late = { 0 };
  gbk_conn_info (c, home, owner, on_info, &late);
  g_assert_cmpint (late.calls, ==, 0);
  g_assert_true (wait_until (answered, &late, 10000));
  g_assert_nonnull (late.error);
  g_clear_error (&late.error);
  Watching w2 = { m, false };
  GbkConnWatch *watch2 = gbk_conn_watch (c, home, owner, on_scan, &w2);
  g_assert_false (w2.ended);
  g_assert_true (wait_until (watch_ended, &w2, 10000));

  gbk_conn_unwatch (watch);
  gbk_conn_unwatch (watch2);
  g_object_unref (c);
  g_assert_true (wait_until (no_conns, nullptr, 10000));
}

/* A frozen server is what a laptop waking from sleep finds: a connection that looks open
 * and says nothing. It's declared dead after the silence, whatever TCP thinks. */
static void
test_server_frozen (void)
{
  gbk_conn_set_timeouts_for_test (100, 600, 0);
  GbkConn *c = connect_to ("ws");
  gbk_conn_set_timeouts_for_test (0, 0, 0);
  g_assert_cmpint (gbk_conn_state (c), ==, GBK_CONN_READY);
  /* Pings keep it alive while the server answers. */
  g_usleep (G_USEC_PER_SEC);
  g_main_context_iteration (nullptr, false);
  g_assert_cmpint (gbk_conn_state (c), ==, GBK_CONN_READY);

  pid_t pid = server_pid ();
  g_assert_cmpint (kill (pid, SIGSTOP), ==, 0);
  int64_t start = g_get_monotonic_time ();
  g_assert_true (wait_until (is_dead, c, 10000));
  g_assert_cmpint (g_get_monotonic_time () - start, <, INT64_C (3) * G_USEC_PER_SEC);
  g_assert_nonnull (strstr (gbk_conn_error (c), "No answer"));
  kill (pid, SIGCONT);
  g_object_unref (c);
  g_assert_true (wait_until (no_conns, nullptr, 10000));
}

/* Requests whose owner went away are dropped without a call. */
static void
test_owner_gone (void)
{
  GbkConn *c = connect_to ("ws");
  GObject *owner = g_object_new (G_TYPE_OBJECT, nullptr);
  Answer a = { 0 };
  gbk_conn_info (c, home, owner, on_info, &a);
  g_object_unref (owner);
  g_usleep (G_USEC_PER_SEC / 2);
  for (int i = 0; i < 100; i++)
    g_main_context_iteration (nullptr, false);
  g_assert_cmpint (a.calls, ==, 0);
  g_object_unref (c);
  g_assert_true (wait_until (no_conns, nullptr, 10000));
}

int
main (int argc, char **argv)
{
  /* A home of its own: that's where quoin-serve gets installed. */
  home = g_dir_make_tmp ("gbk-conn-XXXXXX", nullptr);
  g_setenv ("HOME", home, true);
  log_path = g_build_filename (home, "ssh.log", nullptr);
  pid_path = g_build_filename (home, "ssh.pid", nullptr);
  mode_path = g_build_filename (home, "ssh.mode", nullptr);
  g_setenv ("FAKE_SSH_MODE", mode_path, true);
  g_setenv ("FAKE_SSH_LOG", log_path, true);
  g_setenv ("FAKE_SSH_PIDFILE", pid_path, true);
  g_autofree char *fake
      = g_build_filename (g_getenv ("GBK_SOURCE_ROOT"), "tests", "fake-ssh", nullptr);
  g_setenv ("QUOIN_SSH", fake, true);

  g_test_init (&argc, &argv, nullptr);
  types = gbk_type_builder_finish (gbk_type_builder_new ());
  g_test_add_func ("/conn/install-then-use", test_install_then_use);
  g_test_add_func ("/conn/other-build-replaced", test_other_build_replaced);
  g_test_add_func ("/conn/refusals", test_refusals);
  g_test_add_func ("/conn/server-killed", test_server_killed);
  g_test_add_func ("/conn/server-frozen", test_server_frozen);
  g_test_add_func ("/conn/owner-gone", test_owner_gone);
  int status = g_test_run ();
  gbk_type_table_unref (types);
  if (gbk_lifetime_live () != 0)
    gbk_fatal ("registrations leaked");
  return status;
}
