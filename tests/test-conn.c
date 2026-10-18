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

/* ---- copying from the other machine (a worker thread's view: blocking calls) ---- */

typedef struct
{
  int conflicts;
  GbkChoice answer;
  int errors;
  int progress_calls;
  int cancel_after;        /* >0: cancelled after this many progress calls */
  bool kill_server;        /* at cancel_after, kill the server instead */
  bool cancelled;
  const char *edit_at_start; /* changed at the first progress call (after listing) */
  const char *edit_on_conflict; /* changed when a conflict is asked */
} Hooks;

static GbkChoice
h_conflict (const char *src G_GNUC_UNUSED, const char *dst G_GNUC_UNUSED,
            bool dst_is_dir G_GNUC_UNUSED, bool *all G_GNUC_UNUSED, void *data)
{
  Hooks *h = data;
  h->conflicts++;
  if (h->edit_on_conflict != nullptr)
    {
      g_usleep (20000);                  /* a later mtime, on any filesystem */
      g_assert_true (g_file_set_contents (h->edit_on_conflict, "edited", -1, nullptr));
    }
  return h->answer;
}

static GbkChoice
h_error (const char *path G_GNUC_UNUSED, const GError *error, void *data)
{
  Hooks *h = data;
  h->errors++;
  g_test_message ("copy error: %s", error->message);
  return GBK_CHOICE_SKIP;
}

static void
h_progress (uint64_t done G_GNUC_UNUSED, uint64_t total G_GNUC_UNUSED,
            const char *current G_GNUC_UNUSED, void *data)
{
  Hooks *h = data;
  h->progress_calls++;
  if (h->progress_calls == 1 && h->edit_at_start != nullptr)
    {
      g_usleep (20000);                  /* a later mtime, on any filesystem */
      g_assert_true (g_file_set_contents (h->edit_at_start, "edited", -1, nullptr));
    }
  if (h->cancel_after > 0 && h->progress_calls == h->cancel_after)
    {
      if (h->kill_server)
        kill (server_pid (), SIGKILL);
      else
        h->cancelled = true;
    }
}

static bool
h_cancelled (void *data)
{
  return ((const Hooks *) data)->cancelled;
}

[[nodiscard]] static bool
run_copy (GbkConn *c, const char *src_rel, const char *dst_rel, Hooks *h, GPtrArray *created,
          GError **error)
{
  GbkOpsHooks hooks = { h_conflict, h_error, nullptr, h_progress, h_cancelled, h };
  g_autoptr (GPtrArray) srcs = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (srcs, g_build_filename (home, src_rel, nullptr));
  g_autofree char *dst = g_build_filename (home, dst_rel, nullptr);
  g_mkdir_with_parents (dst, 0755);
  g_autoptr (GbkSource) source = gbk_conn_source (c, false);
  return gbk_ops_run_from (source, GBK_OP_COPY, srcs, dst, &hooks, created, error);
}

static void
assert_same_file (const char *a_rel, const char *b_rel)
{
  g_autofree char *a = g_build_filename (home, a_rel, nullptr);
  g_autofree char *b = g_build_filename (home, b_rel, nullptr);
  g_autofree char *ad = nullptr, *bd = nullptr;
  gsize al, bl;
  g_assert_true (g_file_get_contents (a, &ad, &al, nullptr));
  g_assert_true (g_file_get_contents (b, &bd, &bl, nullptr));
  g_assert_cmpmem (ad, al, bd, bl);
}

static bool
temp_left (const char *dir_rel)
{
  g_autofree char *dir = g_build_filename (home, dir_rel, nullptr);
  g_autoptr (GDir) d = g_dir_open (dir, 0, nullptr);
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    if (g_str_has_prefix (name, ".gbk-tmp-"))
      return true;
  return false;
}

static void
put_big (const char *rel, size_t size)
{
  g_autofree char *data = g_malloc (size);
  for (size_t i = 0; i < size; i++)
    data[i] = (char) (i * 7 + i / 4096);
  g_autofree char *p = g_build_filename (home, rel, nullptr);
  g_autofree char *d = g_path_get_dirname (p);
  g_mkdir_with_parents (d, 0755);
  g_assert_true (g_file_set_contents (p, data, (gssize) size, nullptr));
}

static void
test_copy_from_remote (void)
{
  put_big ("tree/big", 3u << 20);               /* more than the read window: credit flows */
  put ("tree/small", "small");
  put ("tree/empty", "");
  put ("tree/sub/inner", "inner");
  put ("tree/hl1", "linked");
  put ("tree/suid", "#!/bin/sh\n");
  g_autofree char *p_hl1 = g_build_filename (home, "tree", "hl1", nullptr);
  g_autofree char *p_hl2 = g_build_filename (home, "tree", "hl2", nullptr);
  g_assert_cmpint (link (p_hl1, p_hl2), ==, 0);
  g_autofree char *p_ln = g_build_filename (home, "tree", "ln", nullptr);
  g_assert_cmpint (symlink ("small", p_ln), ==, 0);
  g_autofree char *p_fifo = g_build_filename (home, "tree", "pipe", nullptr);
  g_assert_cmpint (mkfifo (p_fifo, 0640), ==, 0);
  g_autofree char *p_suid = g_build_filename (home, "tree", "suid", nullptr);
  g_assert_cmpint (chmod (p_suid, 04755), ==, 0);
  const struct timespec old[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
  g_autofree char *p_small = g_build_filename (home, "tree", "small", nullptr);
  g_assert_cmpint (utimensat (AT_FDCWD, p_small, old, 0), ==, 0);

  GbkConn *c = connect_to ("ws");
  Hooks h = { .answer = GBK_CHOICE_SKIP };
  g_autoptr (GPtrArray) created = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GError) error = nullptr;
  g_assert_true (run_copy (c, "tree", "dst", &h, created, &error));
  g_assert_no_error (error);
  g_assert_cmpint (h.errors, ==, 0);
  g_assert_cmpuint (created->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (created, 0), ==, "tree");
  assert_same_file ("tree/big", "dst/tree/big");
  assert_same_file ("tree/small", "dst/tree/small");
  assert_same_file ("tree/empty", "dst/tree/empty");
  assert_same_file ("tree/sub/inner", "dst/tree/sub/inner");

  struct stat st, st2;
  g_autofree char *d_ln = g_build_filename (home, "dst", "tree", "ln", nullptr);
  g_autofree char *target = g_file_read_link (d_ln, nullptr);
  g_assert_cmpstr (target, ==, "small");
  g_autofree char *d_small = g_build_filename (home, "dst", "tree", "small", nullptr);
  g_assert_cmpint (stat (d_small, &st), ==, 0);
  g_assert_cmpint (st.st_mtim.tv_sec, ==, 1000000000);          /* times travel */
  g_autofree char *d_hl1 = g_build_filename (home, "dst", "tree", "hl1", nullptr);
  g_autofree char *d_hl2 = g_build_filename (home, "dst", "tree", "hl2", nullptr);
  g_assert_cmpint (stat (d_hl1, &st), ==, 0);
  g_assert_cmpint (stat (d_hl2, &st2), ==, 0);
  g_assert_cmpuint (st.st_ino, ==, st2.st_ino);                  /* hard links stay linked */
  g_autofree char *d_fifo = g_build_filename (home, "dst", "tree", "pipe", nullptr);
  g_assert_cmpint (lstat (d_fifo, &st), ==, 0);
  g_assert_true (S_ISFIFO (st.st_mode));
  g_autofree char *d_suid = g_build_filename (home, "dst", "tree", "suid", nullptr);
  g_assert_cmpint (stat (d_suid, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 07777, ==, 0755);               /* no setuid from elsewhere */

  /* Again: everything conflicts; Skip leaves it all. */
  Hooks again = { .answer = GBK_CHOICE_SKIP };
  g_assert_true (run_copy (c, "tree", "dst", &again, nullptr, &error));
  g_assert_cmpint (again.conflicts, ==, 1);

  /* Cancelled halfway through the big file: no half file left anywhere. */
  Hooks cancel = { .answer = GBK_CHOICE_SKIP, .cancel_after = 3 };
  g_autoptr (GError) cancelled = nullptr;
  g_assert_false (run_copy (c, "tree/big", "dst2", &cancel, nullptr, &cancelled));
  g_assert_error (cancelled, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_false (temp_left ("dst2"));
  g_autofree char *d2_big = g_build_filename (home, "dst2", "big", nullptr);
  g_assert_false (g_file_test (d2_big, G_FILE_TEST_EXISTS));

  /* A source that doesn't record what it listed can't remove anything: no move. */
  GbkOpsHooks hooks = { h_conflict, h_error, nullptr, h_progress, h_cancelled, &h };
  g_autoptr (GPtrArray) srcs = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (srcs, g_build_filename (home, "tree", "small", nullptr));
  g_autofree char *dst3 = g_build_filename (home, "dst3", nullptr);
  g_mkdir_with_parents (dst3, 0755);
  g_autoptr (GbkSource) source = gbk_conn_source (c, false);
  int errors_before = h.errors;
  g_autoptr (GError) unrecorded = nullptr;
  g_assert_true (gbk_ops_run_from (source, GBK_OP_MOVE, srcs, dst3, &hooks, nullptr, &unrecorded));
  g_assert_cmpint (h.errors, ==, errors_before + 1);  /* "kept the original" */
  g_assert_true (g_file_test (p_small, G_FILE_TEST_EXISTS));

  /* The connection dies mid-file: that item fails, nothing half-written stays. */
  Hooks dies = { .answer = GBK_CHOICE_SKIP, .cancel_after = 3, .kill_server = true };
  g_autoptr (GError) died = nullptr;
  bool ok = run_copy (c, "tree/big", "dst4", &dies, nullptr, &died);
  g_assert_true (ok || died != nullptr);
  g_assert_cmpint (dies.errors, ==, 1);
  g_assert_false (temp_left ("dst4"));
  g_autofree char *d4_big = g_build_filename (home, "dst4", "big", nullptr);
  g_assert_false (g_file_test (d4_big, G_FILE_TEST_EXISTS));
  g_object_unref (c);
  g_assert_true (wait_until (no_conns, nullptr, 10000));
}

/* Run a COPY or MOVE of `src_rels` (here) into `dst_rel` there, as a job's worker does:
 * the other machine runs it and reads the files from here, in a session for just them. */
[[nodiscard]] static bool
run_there (GbkConn *c, GbkOpKind kind, const char *const *src_rels, const char *dst_rel, Hooks *h,
           GPtrArray *created, GError **error)
{
  GbkOpsHooks hooks = { h_conflict, h_error, nullptr, h_progress, h_cancelled, h };
  g_autoptr (GPtrArray) srcs = g_ptr_array_new_with_free_func (g_free);
  for (size_t i = 0; src_rels[i] != nullptr; i++)
    g_ptr_array_add (srcs, g_build_filename (home, src_rels[i], nullptr));
  g_autofree char *dst = g_build_filename (home, dst_rel, nullptr);
  g_mkdir_with_parents (dst, 0755);
  g_autoptr (GbkAsker) asker = gbk_conn_asker (c);
  g_autoptr (GbkAnswerer) answerer = gbk_conn_answerer (c);
  uint64_t session = gbk_answerer_open (answerer, srcs, kind == GBK_OP_MOVE);
  GbkJobRequest r = { .kind = kind, .sources_here = true, .session = session, .dest = dst };
  bool ok = gbk_asker_run_job (asker, &r, srcs, &hooks, created, error);
  gbk_answerer_close (answerer, session);
  return ok;
}

static bool
exists (const char *rel)
{
  g_autofree char *p = g_build_filename (home, rel, nullptr);
  return g_file_test (p, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK);
}

/* Copy and move from here to there: the other machine runs the job with the same file
 * operations as here, asks its questions here, and reads the files from here. */
static void
test_copy_to_remote (void)
{
  put_big ("up/big", 3u << 20);
  put ("up/sub/inner", "inner");
  g_autofree char *p_ln = g_build_filename (home, "up", "ln", nullptr);
  g_assert_cmpint (symlink ("sub/inner", p_ln), ==, 0);
  GbkConn *c = connect_to ("ws");

  Hooks h = { .answer = GBK_CHOICE_SKIP };
  g_autoptr (GPtrArray) created = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GError) error = nullptr;
  const char *const up[] = { "up", nullptr };
  g_assert_true (run_there (c, GBK_OP_COPY, up, "there", &h, created, &error));
  g_assert_no_error (error);
  g_assert_cmpint (h.errors, ==, 0);
  g_assert_cmpint (h.conflicts, ==, 0);
  g_assert_cmpuint (created->len, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (created, 0), ==, "up");
  g_assert_cmpint (h.progress_calls, >, 0);
  assert_same_file ("up/big", "there/up/big");
  assert_same_file ("up/sub/inner", "there/up/sub/inner");
  g_autofree char *d_ln = g_build_filename (home, "there", "up", "ln", nullptr);
  g_autofree char *target = g_file_read_link (d_ln, nullptr);
  g_assert_cmpstr (target, ==, "sub/inner");

  /* Again: the conflict is asked here; Keep Both makes "up (1)" there. */
  Hooks both = { .answer = GBK_CHOICE_KEEP_BOTH };
  g_autoptr (GPtrArray) created2 = g_ptr_array_new_with_free_func (g_free);
  g_assert_true (run_there (c, GBK_OP_COPY, up, "there", &both, created2, &error));
  g_assert_cmpint (both.conflicts, ==, 1);
  g_assert_cmpstr (g_ptr_array_index (created2, 0), ==, "up (1)");
  assert_same_file ("up/big", "there/up (1)/big");

  /* Cancelled here: it stops there, leaving no half-written file. */
  Hooks cancel = { .answer = GBK_CHOICE_SKIP, .cancel_after = 1 };
  const char *const big[] = { "up/big", nullptr };
  g_autoptr (GError) cancelled = nullptr;
  gbk_answerer_set_read_delay_for_test (20);    /* 48 chunks: about a second */
  g_assert_false (run_there (c, GBK_OP_COPY, big, "there2", &cancel, nullptr, &cancelled));
  gbk_answerer_set_read_delay_for_test (0);
  g_assert_error (cancelled, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_false (temp_left ("there2"));
  g_assert_false (exists ("there2/big"));

  /* Move: the originals go once the copy is safely there, except one that changed after
   * it was listed: kept here, and the job says so. (The change is made while the job waits
   * for an answer about "b", which is in the way there, before "a" is copied.) */
  put ("mv/a/same", "1");
  put ("mv/a/edited", "2");
  put ("mv/b", "3");
  put ("moved/b", "old");
  g_autofree char *edited = g_build_filename (home, "mv", "a", "edited", nullptr);
  Hooks mv = { .answer = GBK_CHOICE_REPLACE, .edit_on_conflict = edited };
  const char *const both_srcs[] = { "mv/b", "mv/a", nullptr };
  g_autoptr (GError) mv_error = nullptr;
  g_assert_true (run_there (c, GBK_OP_MOVE, both_srcs, "moved", &mv, nullptr, &mv_error));
  g_assert_no_error (mv_error);
  g_assert_cmpint (mv.conflicts, ==, 1);
  g_assert_cmpint (mv.errors, ==, 1);
  g_autofree char *moved_b = g_build_filename (home, "moved", "b", nullptr);
  g_autofree char *b_text = nullptr;
  g_assert_true (g_file_get_contents (moved_b, &b_text, nullptr, nullptr));
  g_assert_cmpstr (b_text, ==, "3");                  /* replaced, as answered */
  g_assert_false (exists ("mv/b"));
  g_assert_false (exists ("mv/a/same"));
  g_assert_true (exists ("mv/a/edited"));
  g_assert_true (exists ("moved/b"));
  g_assert_true (exists ("moved/a/same"));
  g_assert_true (exists ("moved/a/edited"));
  g_object_unref (c);
  g_assert_true (wait_until (no_conns, nullptr, 10000));
}

/* Move from there to here: the originals there go once the copy here is safely stored,
 * except one that changed after it was listed. */
static void
test_move_from_remote (void)
{
  put ("far/a/same", "1");
  put ("far/a/edited", "2");
  GbkConn *c = connect_to ("ws");
  g_autofree char *edited = g_build_filename (home, "far", "a", "edited", nullptr);
  Hooks h = { .answer = GBK_CHOICE_SKIP, .edit_at_start = edited };
  GbkOpsHooks hooks = { h_conflict, h_error, nullptr, h_progress, h_cancelled, &h };
  g_autoptr (GPtrArray) srcs = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (srcs, g_build_filename (home, "far", "a", nullptr));
  g_autofree char *dst = g_build_filename (home, "near", nullptr);
  g_mkdir_with_parents (dst, 0755);
  int flushes = gbk_ops_flushes ();
  g_autoptr (GbkSource) source = gbk_conn_source (c, true);
  g_autoptr (GError) error = nullptr;
  g_assert_true (gbk_ops_run_from (source, GBK_OP_MOVE, srcs, dst, &hooks, nullptr, &error));
  g_assert_no_error (error);
  g_assert_cmpint (gbk_ops_flushes (), ==, flushes + 1);   /* stored safely first */
  g_assert_cmpint (h.errors, ==, 1);
  g_assert_false (exists ("far/a/same"));
  g_assert_true (exists ("far/a/edited"));
  g_assert_true (exists ("near/a/same"));
  g_assert_true (exists ("near/a/edited"));

  /* Without the change, everything goes. */
  put ("far2/x", "x");
  Hooks h2 = { .answer = GBK_CHOICE_SKIP };
  GbkOpsHooks hooks2 = { h_conflict, h_error, nullptr, h_progress, h_cancelled, &h2 };
  g_autoptr (GPtrArray) srcs2 = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (srcs2, g_build_filename (home, "far2", nullptr));
  g_autoptr (GbkSource) source2 = gbk_conn_source (c, true);
  g_assert_true (gbk_ops_run_from (source2, GBK_OP_MOVE, srcs2, dst, &hooks2, nullptr, &error));
  g_assert_cmpint (h2.errors, ==, 0);
  g_assert_false (exists ("far2"));
  g_assert_true (exists ("near/far2/x"));
  g_clear_pointer (&source, gbk_source_free);
  g_clear_pointer (&source2, gbk_source_free);
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
  g_test_add_func ("/conn/copy-from-remote", test_copy_from_remote);
  g_test_add_func ("/conn/copy-to-remote", test_copy_to_remote);
  g_test_add_func ("/conn/move-from-remote", test_move_from_remote);
  int status = g_test_run ();
  gbk_type_table_unref (types);
  if (gbk_lifetime_live () != 0)
    gbk_fatal ("registrations leaked");
  return status;
}
