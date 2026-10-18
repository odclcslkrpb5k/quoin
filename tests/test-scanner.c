/* Directory model + scanner against a real filesystem (temp dirs inside the container). */
#include "dirmodel.h"
#include "inotify-hub.h"
#include "lifetime.h"

static GbkTypeTable *types;

/* A model of the local folder `path`. */
static GbkDirModel *
model_at (const char *path, GbkTypeTable *t)
{
  g_autoptr (GbkLoc) loc = gbk_loc_new_local (path);
  return gbk_dir_model_new (loc, t);
}

/* ---- helpers ---- */

typedef bool (*Pred) (void *data);

static bool
wait_until (Pred pred, void *data, int timeout_ms)
{
  int64_t deadline = g_get_monotonic_time () + (int64_t) timeout_ms * 1000;
  while (g_get_monotonic_time () < deadline)
    {
      while (g_main_context_iteration (nullptr, false))
        ;
      if (pred (data))
        return true;
      g_usleep (1000);
    }
  return pred (data);
}

static bool
is_ready (void *data)
{
  return gbk_dir_model_state (data) == GBK_DIR_READY;
}

static bool
not_loading (void *data)
{
  return gbk_dir_model_state (data) != GBK_DIR_LOADING;
}

static bool
is_gone (void *data)
{
  return gbk_dir_model_state (data) == GBK_DIR_GONE;
}

static bool
no_workers (void *data G_GNUC_UNUSED)
{
  return gbk_scanner_live_workers () == 0;
}

static bool
no_workers_but_one (void *data G_GNUC_UNUSED)
{
  return gbk_scanner_live_workers () == 1;
}

static void
put (const char *dir, const char *name)
{
  g_autofree char *path = g_build_filename (dir, name, nullptr);
  int fd = open (path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  g_assert_cmpint (fd, >=, 0);
  close (fd);
}

static void
rm_rf (const char *path)
{
  struct stat st;
  if (lstat (path, &st) != 0)
    return;
  if (S_ISDIR (st.st_mode))
    {
      /* No chmod here: it generates inotify events that once masked a real bug (deletion
       * detection that only worked if something else touched the directory). */
      g_autoptr (GDir) d = g_dir_open (path, 0, nullptr);
      const char *name;
      while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
        {
          g_autofree char *child = g_build_filename (path, name, nullptr);
          rm_rf (child);
        }
      rmdir (path);
    }
  else
    unlink (path);
}

static char *
mktmp (void)
{
  g_autoptr (GError) error = nullptr;
  char *dir = g_dir_make_tmp ("gbk-test-XXXXXX", &error);
  g_assert_no_error (error);
  return dir;
}

typedef struct
{
  GbkDirModel *m;
  const char *dir;
} MatchCtx;

/* True when the model holds exactly the directory's current entries. */
static bool
model_matches_fs (void *data)
{
  MatchCtx *c = data;
  DIR *d = opendir (c->dir);
  if (d == nullptr)
    gbk_fatal ("opendir %s: %s", c->dir, g_strerror (errno));
  size_t n = 0;
  bool ok = true;
  const struct dirent *de;
  while ((de = readdir (d)) != nullptr)
    {
      if (g_str_equal (de->d_name, ".") || g_str_equal (de->d_name, ".."))
        continue;
      n++;
      if (gbk_dir_model_get (c->m, gbk_dir_model_find (c->m, de->d_name)) == nullptr)
        ok = false;
    }
  closedir (d);
  return ok && n == gbk_dir_model_n_entries (c->m);
}

static const GbkEntry *
get (GbkDirModel *m, const char *name)
{
  return gbk_dir_model_get (m, gbk_dir_model_find (m, name));
}

/* ---- tests ---- */

static void
test_initial_listing (void)
{
  g_autofree char *dir = mktmp ();
  put (dir, "a.txt");
  put (dir, ".hidden");
  g_autofree char *sub = g_build_filename (dir, "sub", nullptr);
  g_assert_cmpint (mkdir (sub, 0755), ==, 0);
  g_autofree char *link = g_build_filename (dir, "link-to-sub", nullptr);
  g_assert_cmpint (symlink ("sub", link), ==, 0);
  g_autofree char *broken = g_build_filename (dir, "broken", nullptr);
  g_assert_cmpint (symlink ("does-not-exist", broken), ==, 0);
  g_autofree char *fifo = g_build_filename (dir, "pipe", nullptr);
  g_assert_cmpint (mkfifo (fifo, 0644), ==, 0);
  put (dir, "bad\xff\xfename");
  g_autofree char *exe = g_build_filename (dir, "prog", nullptr);
  put (dir, "prog");
  g_assert_cmpint (chmod (exe, 0755), ==, 0);

  g_autoptr (GbkDirModel) m = model_at (dir, types);
  g_assert_true (wait_until (is_ready, m, 5000));
  g_assert_true (gbk_dir_model_live (m));
  uint64_t free_bytes = 0;
  g_assert_true (gbk_dir_model_free_space (m, &free_bytes));
  g_assert_cmpuint (free_bytes, >, 0);
  g_assert_cmpuint (gbk_dir_model_n_entries (m), ==, 8);
  g_assert_cmpuint (gbk_dir_model_n_visible (m), ==, 7);    /* .hidden filtered */

  g_assert_cmpint (get (m, "sub")->kind, ==, GBK_KIND_DIRECTORY);
  g_assert_cmpint (get (m, "link-to-sub")->kind, ==, GBK_KIND_DIRECTORY);
  g_assert_true (get (m, "link-to-sub")->flags & GBK_ENTRY_SYMLINK);
  g_assert_cmpint (get (m, "broken")->kind, ==, GBK_KIND_BROKEN_LINK);
  g_assert_cmpint (get (m, "pipe")->kind, ==, GBK_KIND_FIFO);
  g_assert_true (get (m, ".hidden")->flags & GBK_ENTRY_HIDDEN);
  g_assert_cmpstr (gbk_type_table_mime (types, get (m, "a.txt")->type), ==, "text/plain");
  g_assert_cmpstr (gbk_type_table_mime (types, get (m, "prog")->type), ==,
                   "application/x-executable");
  const GbkEntry *bad = get (m, "bad\xff\xfename");
  g_assert_nonnull (bad);
  g_assert_true (g_utf8_validate (gbk_entry_display (bad), -1, nullptr));
  g_assert_cmpstr (gbk_entry_display (bad), !=, gbk_entry_name (bad));
  gbk_dir_model_check_order (m);

  g_clear_object (&m);
  rm_rf (dir);
}

static bool
first_mtime_updated (void *data)
{
  const GbkEntry *e = get (data, "first");
  return e != nullptr && e->mtime_ns == 1000000000000LL;
}

static void
test_live_updates (void)
{
  g_autofree char *dir = mktmp ();
  put (dir, "first");
  g_autoptr (GbkDirModel) m = model_at (dir, types);
  g_assert_true (wait_until (is_ready, m, 5000));
  MatchCtx c = { m, dir };

  put (dir, "created");
  g_assert_true (wait_until (model_matches_fs, &c, 5000));
  g_assert_nonnull (get (m, "created"));

  GbkHandle first = gbk_dir_model_find (m, "first");
  g_autofree char *p1 = g_build_filename (dir, "first", nullptr);
  const struct timespec ts[2] = { { .tv_sec = 1000 }, { .tv_sec = 1000 } };
  g_assert_cmpint (utimensat (AT_FDCWD, p1, ts, 0), ==, 0);
  g_assert_true (wait_until (first_mtime_updated, m, 5000));
  g_assert_cmpint (get (m, "first")->mtime_ns, ==, 1000000000000LL);
  g_assert_nonnull (gbk_dir_model_get (m, first));    /* update kept the handle */

  g_autofree char *p2 = g_build_filename (dir, "renamed", nullptr);
  g_assert_cmpint (rename (p1, p2), ==, 0);
  g_assert_true (wait_until (model_matches_fs, &c, 5000));
  g_assert_null (gbk_dir_model_get (m, first));       /* old name: handle is stale */
  g_assert_nonnull (get (m, "renamed"));

  g_autofree char *p3 = g_build_filename (dir, "created", nullptr);
  g_assert_cmpint (unlink (p3), ==, 0);
  g_autofree char *p4 = g_build_filename (dir, "newdir", nullptr);
  g_assert_cmpint (mkdir (p4, 0755), ==, 0);
  g_assert_true (wait_until (model_matches_fs, &c, 5000));
  g_assert_cmpint (get (m, "newdir")->kind, ==, GBK_KIND_DIRECTORY);
  gbk_dir_model_check_order (m);

  g_clear_object (&m);
  rm_rf (dir);
}

static void
test_large_directory (void)
{
  g_autofree char *dir = mktmp ();
  for (int i = 0; i < 20000; i++)
    {
      char name[32];
      g_snprintf (name, sizeof name, "f%05d.txt", i);
      put (dir, name);
    }
  g_autoptr (GbkDirModel) m = model_at (dir, types);
  g_assert_true (wait_until (is_ready, m, 30000));
  g_assert_cmpuint (gbk_dir_model_n_visible (m), ==, 20000);
  gbk_dir_model_check_order (m);
  g_assert_cmpstr (gbk_entry_name (gbk_dir_model_get (m, gbk_dir_model_visible_handle (m, 0))), ==,
                   "f00000.txt");
  g_clear_object (&m);
  rm_rf (dir);
}

typedef struct
{
  const char *dir;
  int ops;
  int done;     /* atomic */
} StormCtx;

static void *
storm (void *data)
{
  StormCtx *s = data;
  GRand *r = g_rand_new_with_seed (777);
  for (int i = 0; i < s->ops; i++)
    {
      char a[64], b[64];
      g_snprintf (a, sizeof a, "%s/n%d", s->dir, g_rand_int_range (r, 0, 500));
      g_snprintf (b, sizeof b, "%s/n%d", s->dir, g_rand_int_range (r, 0, 500));
      switch (g_rand_int_range (r, 0, 4))
        {
        case 0:
          {
            int fd = open (a, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
            if (fd >= 0)
              close (fd);
            break;
          }
        case 1: unlink (a); break;
        case 2: rename (a, b); break;
        default:
          if (mkdir (a, 0755) == 0)
            rmdir (a);
          break;
        }
    }
  g_rand_free (r);
  g_atomic_int_set (&s->done, 1);
  return nullptr;
}

static void
run_storm (unsigned pending_max)
{
  g_autofree char *dir = mktmp ();
  g_autoptr (GbkDirModel) m = model_at (dir, types);
  g_assert_true (wait_until (is_ready, m, 5000));
  gbk_scanner_set_pending_max_for_test (pending_max);
  int rescans_before = gbk_scanner_rescans ();

  StormCtx s = { .dir = dir, .ops = 30000 };
  GThread *t = g_thread_new ("storm", storm, &s);
  /* Keep the main loop running while the storm rages, like the real UI would, checking
   * the model's invariants as every batch lands. */
  while (!g_atomic_int_get (&s.done))
    {
      while (g_main_context_iteration (nullptr, false))
        gbk_dir_model_check_order (m);
      g_usleep (500);
    }
  g_thread_join (t);

  MatchCtx c = { m, dir };
  g_assert_true (wait_until (model_matches_fs, &c, 10000));
  gbk_dir_model_check_order (m);
  if (pending_max < GBK_SCAN_PENDING_MAX)
    g_assert_cmpint (gbk_scanner_rescans (), >, rescans_before);
  gbk_scanner_set_pending_max_for_test (GBK_SCAN_PENDING_MAX);

  g_clear_object (&m);
  rm_rf (dir);
}

static void
test_storm_deltas (void)
{
  run_storm (GBK_SCAN_PENDING_MAX);
}

static void
test_storm_rescans (void)
{
  run_storm (8);     /* nearly every batch overflows the cap: exercise rescans hard */
}

static void
test_gone (void)
{
  g_autofree char *parent = mktmp ();
  g_autofree char *dir = g_build_filename (parent, "victim", nullptr);
  g_assert_cmpint (mkdir (dir, 0755), ==, 0);
  put (dir, "x");
  g_autoptr (GbkDirModel) m = model_at (dir, types);
  g_assert_true (wait_until (is_ready, m, 5000));
  rm_rf (dir);
  g_assert_true (wait_until (is_gone, m, 5000));
  g_assert_false (gbk_dir_model_live (m));
  g_clear_object (&m);
  rm_rf (parent);
}

static void
test_gone_nested (void)
{
  /* Watching a/b while all of a/ is removed (children first, as rm -r does). */
  g_autofree char *parent = mktmp ();
  g_autofree char *a = g_build_filename (parent, "a", nullptr);
  g_autofree char *ab = g_build_filename (parent, "a", "b", nullptr);
  g_assert_cmpint (g_mkdir_with_parents (ab, 0755), ==, 0);
  put (ab, "f");
  g_autoptr (GbkDirModel) m = model_at (ab, types);
  g_assert_true (wait_until (is_ready, m, 5000));
  rm_rf (a);
  g_assert_true (wait_until (is_gone, m, 5000));
  g_clear_object (&m);
  rm_rf (parent);
}

/* A plain rmdir and nothing else. The scanner's own fd keeps the inode alive (as would a
 * shell sitting in the directory), so IN_DELETE_SELF can't be what detects this. */
static void
test_gone_plain_rmdir (void)
{
  g_autofree char *parent = mktmp ();
  g_autofree char *dir = g_build_filename (parent, "held", nullptr);
  g_assert_cmpint (mkdir (dir, 0755), ==, 0);
  g_autoptr (GbkDirModel) m = model_at (dir, types);
  g_assert_true (wait_until (is_ready, m, 5000));
  g_assert_cmpint (rmdir (dir), ==, 0);
  g_assert_true (wait_until (is_gone, m, 5000));
  g_clear_object (&m);
  rm_rf (parent);
}

static void
test_moved_away (void)
{
  g_autofree char *parent = mktmp ();
  g_autofree char *dir = g_build_filename (parent, "here", nullptr);
  g_autofree char *elsewhere = g_build_filename (parent, "there", nullptr);
  g_assert_cmpint (mkdir (dir, 0755), ==, 0);
  g_autoptr (GbkDirModel) m = model_at (dir, types);
  g_assert_true (wait_until (is_ready, m, 5000));
  g_assert_cmpint (rename (dir, elsewhere), ==, 0);
  g_assert_true (wait_until (is_gone, m, 5000));
  g_clear_object (&m);
  rm_rf (parent);
}

static void
test_sibling_deleted_is_not_us (void)
{
  /* Unrelated changes in the parent must not be mistaken for our deletion. */
  g_autofree char *parent = mktmp ();
  g_autofree char *dir = g_build_filename (parent, "me", nullptr);
  g_autofree char *sib = g_build_filename (parent, "me2", nullptr);
  g_assert_cmpint (mkdir (dir, 0755), ==, 0);
  g_assert_cmpint (mkdir (sib, 0755), ==, 0);
  g_autoptr (GbkDirModel) m = model_at (dir, types);
  g_assert_true (wait_until (is_ready, m, 5000));
  g_assert_cmpint (rmdir (sib), ==, 0);
  put (dir, "still-alive");                 /* then a real change inside us */
  MatchCtx c = { m, dir };
  g_assert_true (wait_until (model_matches_fs, &c, 5000));
  g_assert_cmpint (gbk_dir_model_state (m), ==, GBK_DIR_READY);
  g_clear_object (&m);
  rm_rf (parent);
}

/* ---- the shared inotify hub ---- */

static int
inotify_instances (void)
{
  int n = 0;
  g_autoptr (GDir) d = g_dir_open ("/proc/self/fd", 0, nullptr);
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    {
      g_autofree char *link = g_build_filename ("/proc/self/fd", name, nullptr);
      g_autofree char *target = g_file_read_link (link, nullptr);
      if (target != nullptr && g_str_equal (target, "anon_inode:inotify"))
        n++;
    }
  return n;
}

static void
test_hub_single_instance (void)
{
  g_autofree char *dir = mktmp ();
  GbkDirModel *models[10];
  for (int i = 0; i < 10; i++)
    {
      g_autofree char *sub = g_strdup_printf ("%s/d%d", dir, i);
      g_assert_cmpint (mkdir (sub, 0755), ==, 0);
      models[i] = model_at (sub, types);
    }
  for (int i = 0; i < 10; i++)
    {
      g_assert_true (wait_until (is_ready, models[i], 5000));
      g_assert_true (gbk_dir_model_live (models[i]));
    }
  g_assert_cmpint (inotify_instances (), ==, 1);
  for (int i = 0; i < 10; i++)
    g_object_unref (models[i]);
  g_assert_true (wait_until (no_workers, nullptr, 5000));
  rm_rf (dir);
}

static void
test_hub_same_dir_twice (void)
{
  g_autofree char *dir = mktmp ();
  GbkDirModel *a = model_at (dir, types);
  GbkDirModel *b = model_at (dir, types);
  g_assert_true (wait_until (is_ready, a, 5000));
  g_assert_true (wait_until (is_ready, b, 5000));
  put (dir, "one");
  MatchCtx ca = { a, dir }, cb = { b, dir };
  g_assert_true (wait_until (model_matches_fs, &ca, 5000));
  g_assert_true (wait_until (model_matches_fs, &cb, 5000));

  /* Closing one must not remove the watch the other still uses. */
  g_object_unref (a);
  g_assert_true (wait_until (no_workers_but_one, nullptr, 5000));
  put (dir, "two");
  g_assert_true (wait_until (model_matches_fs, &cb, 5000));
  g_assert_nonnull (gbk_dir_model_get (b, gbk_dir_model_find (b, "two")));
  g_object_unref (b);
  rm_rf (dir);
}

static void
test_hub_parent_and_child (void)
{
  /* The child's parent-watch is the parent model's self-watch: same wd, merged masks. */
  g_autofree char *dir = mktmp ();
  g_autofree char *child = g_build_filename (dir, "child", nullptr);
  g_assert_cmpint (mkdir (child, 0755), ==, 0);
  GbkDirModel *parent = model_at (dir, types);
  GbkDirModel *kid = model_at (child, types);
  g_assert_true (wait_until (is_ready, parent, 5000));
  g_assert_true (wait_until (is_ready, kid, 5000));

  g_assert_cmpint (rmdir (child), ==, 0);
  g_assert_true (wait_until (is_gone, kid, 5000));
  MatchCtx c = { parent, dir };
  g_assert_true (wait_until (model_matches_fs, &c, 5000));
  g_object_unref (kid);

  put (dir, "after");                       /* the parent is still live */
  g_assert_true (wait_until (model_matches_fs, &c, 5000));
  g_assert_cmpint (gbk_dir_model_state (parent), ==, GBK_DIR_READY);
  g_object_unref (parent);
  rm_rf (dir);
}

static void
test_hub_subscriber_overflow (void)
{
  g_autofree char *dir = mktmp ();
  g_autoptr (GbkDirModel) m = model_at (dir, types);
  g_assert_true (wait_until (is_ready, m, 5000));
  gbk_inotify_set_max_pending_for_test (0);   /* every event overflows: deterministic */
  int before = gbk_scanner_rescans ();
  for (int i = 0; i < 300; i++)
    {
      char name[16];
      g_snprintf (name, sizeof name, "f%d", i);
      put (dir, name);
    }
  MatchCtx c = { m, dir };
  g_assert_true (wait_until (model_matches_fs, &c, 10000));
  g_assert_cmpint (gbk_scanner_rescans (), >, before);
  gbk_inotify_set_max_pending_for_test (GBK_INOTIFY_MAX_PENDING);
  g_clear_object (&m);
  rm_rf (dir);
}

static void
test_errors (void)
{
  g_autoptr (GbkDirModel) missing = model_at ("/nonexistent/gbk/dir", types);
  g_assert_true (wait_until (not_loading, missing, 5000));
  g_assert_cmpint (gbk_dir_model_state (missing), ==, GBK_DIR_ERROR);
  g_assert_cmpint (gbk_dir_model_error (missing), ==, ENOENT);

  /* A folder on another machine is never read here, even though the same path exists on
   * this one: no scanner starts, and the model says why. */
  int workers = gbk_scanner_live_workers ();
  g_autoptr (GbkLoc) elsewhere = gbk_loc_new_remote_for_test ("ws", "/tmp");
  g_autoptr (GbkDirModel) remote = gbk_dir_model_new (elsewhere, types);
  g_assert_cmpint (gbk_dir_model_state (remote), ==, GBK_DIR_ERROR);
  g_assert_cmpint (gbk_dir_model_error (remote), ==, EREMOTE);
  g_assert_cmpuint (gbk_dir_model_n_entries (remote), ==, 0);
  g_assert_cmpint (gbk_scanner_live_workers (), ==, workers);

  g_autofree char *file_dir = mktmp ();
  put (file_dir, "plain");
  g_autofree char *plain = g_build_filename (file_dir, "plain", nullptr);
  g_autoptr (GbkDirModel) notdir = model_at (plain, types);
  g_assert_true (wait_until (not_loading, notdir, 5000));
  g_assert_cmpint (gbk_dir_model_error (notdir), ==, ENOTDIR);

  if (geteuid () != 0)
    {
      g_autofree char *locked = g_build_filename (file_dir, "locked", nullptr);
      g_assert_cmpint (mkdir (locked, 0000), ==, 0);
      g_autoptr (GbkDirModel) denied = model_at (locked, types);
      g_assert_true (wait_until (not_loading, denied, 5000));
      g_assert_cmpint (gbk_dir_model_state (denied), ==, GBK_DIR_ERROR);
      g_assert_cmpint (gbk_dir_model_error (denied), ==, EACCES);
      chmod (locked, 0700);
    }
  rm_rf (file_dir);
}

static void
test_cancel_mid_scan (void)
{
  g_autofree char *dir = mktmp ();
  for (int i = 0; i < 30000; i++)
    {
      char name[32];
      g_snprintf (name, sizeof name, "f%d", i);
      put (dir, name);
    }
  for (int round = 0; round < 50; round++)
    {
      GbkDirModel *m = model_at (dir, types);
      for (int i = 0; i < round % 5; i++)
        g_main_context_iteration (nullptr, false);
      g_object_unref (m);
    }
  g_assert_true (wait_until (no_workers, nullptr, 30000));
  rm_rf (dir);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  const char globs[] = "50:text/plain:*.txt\n";
  GbkTypeBuilder *b = gbk_type_builder_new ();
  gbk_type_builder_load_globs2 (b, globs, strlen (globs));
  types = gbk_type_builder_finish (b);

  g_test_add_func ("/scanner/initial-listing", test_initial_listing);
  g_test_add_func ("/scanner/live-updates", test_live_updates);
  g_test_add_func ("/scanner/large-directory", test_large_directory);
  g_test_add_func ("/scanner/storm-deltas", test_storm_deltas);
  g_test_add_func ("/scanner/storm-rescans", test_storm_rescans);
  g_test_add_func ("/scanner/gone", test_gone);
  g_test_add_func ("/scanner/gone-nested", test_gone_nested);
  g_test_add_func ("/scanner/gone-plain-rmdir", test_gone_plain_rmdir);
  g_test_add_func ("/scanner/moved-away", test_moved_away);
  g_test_add_func ("/scanner/sibling-deleted-is-not-us", test_sibling_deleted_is_not_us);
  g_test_add_func ("/scanner/errors", test_errors);
  g_test_add_func ("/scanner/hub-single-instance", test_hub_single_instance);
  g_test_add_func ("/scanner/hub-same-dir-twice", test_hub_same_dir_twice);
  g_test_add_func ("/scanner/hub-parent-and-child", test_hub_parent_and_child);
  g_test_add_func ("/scanner/hub-subscriber-overflow", test_hub_subscriber_overflow);
  g_test_add_func ("/scanner/cancel-mid-scan", test_cancel_mid_scan);
  int status = g_test_run ();

  /* Workers are detached: let them all finish so the leak checker sees a clean exit. */
  g_assert_true (wait_until (no_workers, nullptr, 30000));
  g_assert_cmpint (gbk_lifetime_live (), ==, 0);
  gbk_type_table_unref (types);
  return status;
}
