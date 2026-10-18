/* Scan/sort/memory benchmark. Usage: bench-scan <dir> <n-files>
 * Creates <dir>/<n> with n entries (once), then measures the real pipeline: scanner thread,
 * messages, model, sort. Run via ./build bench (release build, real host filesystem). */
#include "dirmodel.h"
#include "lifetime.h"

/* A model of the local folder `path`. */
static GbkDirModel *
model_at (const char *path, GbkTypeTable *t)
{
  g_autoptr (GbkLoc) loc = gbk_loc_new_local (path);
  return gbk_dir_model_new (loc, t);
}

static int64_t
now_us (void)
{
  return g_get_monotonic_time ();
}

static size_t
heap_used (void)
{
  return mallinfo2 ().uordblks;
}

static void
populate (const char *dir, int n)
{
  g_autofree char *stamp = g_build_filename (dir, ".complete", nullptr);
  if (g_file_test (stamp, G_FILE_TEST_EXISTS))
    return;
  g_mkdir_with_parents (dir, 0755);
  static const char *const exts[] = { "txt", "jpg", "tar.gz", "c", "PDF", "", "mp4", "py" };
  for (int i = 0; i < n; i++)
    {
      char name[96];
      if (i % 50 == 0)
        g_snprintf (name, sizeof name, "%s/Folder %d", dir, i);
      else
        g_snprintf (name, sizeof name, "%s/%s_%d%s%s", dir, (i % 3) ? "photo" : "Report",
                    i * 7919 % 100003, exts[i % 8][0] ? "." : "", exts[i % 8]);
      if (i % 50 == 0)
        mkdir (name, 0755);
      else
        {
          int fd = open (name, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
          if (fd >= 0)
            close (fd);
        }
    }
  g_autoptr (GError) error = nullptr;
  if (!g_file_set_contents (stamp, "", 0, &error))
    gbk_fatal ("bench data stamp: %s", error->message);
}

typedef struct
{
  int64_t start, first_change;
} Timing;

static void
on_changed (GbkDirModel *m G_GNUC_UNUSED, Timing *t)
{
  if (t->first_change == 0)
    t->first_change = now_us ();
}

static void
run_until (const GbkDirModel *m, GbkDirState state)
{
  while (gbk_dir_model_state (m) != state)
    g_main_context_iteration (nullptr, true);
}

/* Per-stage costs, measured outside the pipeline, to see where scan time goes. */
static void
breakdown (const char *dir, GbkTypeTable *types)
{
  int dfd = open (dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0)
    {
      g_printerr ("open %s: %s\n", dir, g_strerror (errno));
      return;
    }
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GArray) dtypes = g_array_new (false, false, 1);

  int64_t t0 = now_us ();
  DIR *d = opendir (dir);
  if (d == nullptr)
    {
      g_printerr ("opendir %s: %s\n", dir, g_strerror (errno));
      close (dfd);
      return;
    }
  struct dirent *de;
  while ((de = readdir (d)) != nullptr)
    {
      g_ptr_array_add (names, g_strdup (de->d_name));
      g_array_append_val (dtypes, de->d_type);
    }
  closedir (d);
  int64_t t1 = now_us ();

  struct statx st;
  for (guint i = 0; i < names->len; i++)
    statx (dfd, g_ptr_array_index (names, i),
           AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_STATX_DONT_SYNC,
           STATX_TYPE | STATX_MODE | STATX_MTIME | STATX_SIZE, &st);
  int64_t t2 = now_us ();

  g_autoptr (GPtrArray) entries = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_entry_free);
  GbkEntryInfo info = { .kind = GBK_KIND_REGULAR, .mode = 0644 };
  for (guint i = 0; i < names->len; i++)
    {
      const char *n = g_ptr_array_index (names, i);
      GbkEntry *e = gbk_entry_new (n, strlen (n), &info, types);
      if (e != nullptr)
        g_ptr_array_add (entries, e);
    }
  int64_t t3 = now_us ();

  GbkScanMsg *msg = g_new0 (GbkScanMsg, 1);
  msg->type = GBK_SCAN_CHUNK;
  msg->first = msg->last = true;
  msg->upserts = g_steal_pointer (&entries);
  msg->removes = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  int64_t t4 = now_us ();
  gbk_dir_model_apply_for_test (m, msg);
  int64_t t5 = now_us ();

  double n = (double) names->len;
  printf ("  readdir             %.2f us/entry\n", (double) (t1 - t0) / n);
  printf ("  statx               %.2f us/entry\n", (double) (t2 - t1) / n);
  printf ("  build entry         %.2f us/entry\n", (double) (t3 - t2) / n);
  printf ("  model insert+sort   %.2f us/entry\n", (double) (t5 - t4) / n);
  close (dfd);
}

int
main (int argc, char **argv)
{
  if (argc != 3)
    {
      fprintf (stderr, "usage: %s <dir> <n>\n", argv[0]);
      return 2;
    }
  int n = (int) g_ascii_strtoll (argv[2], nullptr, 10);
  g_autofree char *dir = g_strdup_printf ("%s/%d", argv[1], n);
  populate (dir, n);

  g_autoptr (GbkTypeTable) types = gbk_type_table_new_system ();
  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);

  /* Warm-up pass: page cache and dentries hot, like browsing a folder twice. */
  {
    g_autoptr (GbkDirModel) w = model_at (dir, types);
    run_until (w, GBK_DIR_READY);
  }

  size_t heap0 = heap_used ();
  Timing t = { .start = now_us () };
  GbkDirModel *m = model_at (dir, types);
  gbk_connect (owner, m, "changed", G_CALLBACK (on_changed), &t);
  run_until (m, GBK_DIR_READY);
  int64_t ready = now_us ();
  size_t heap1 = heap_used ();
  size_t entries = gbk_dir_model_n_entries (m);

  int64_t s0 = now_us ();
  for (int i = 0; i < 5; i++)
    {
      gbk_dir_model_set_sort (m, GBK_SORT_MTIME);
      gbk_dir_model_set_sort (m, GBK_SORT_NAME);
    }
  double sort_ms = (double) (now_us () - s0) / 10.0 / 1000.0;

  int64_t h0 = now_us ();
  for (int i = 0; i < 10; i++)
    gbk_dir_model_set_show_hidden (m, i % 2 == 0);
  double hidden_ms = (double) (now_us () - h0) / 10.0 / 1000.0;

  /* Live update latency: file created by "another app" until it's in the model. */
  double lat_total = 0;
  const int probes = 10;
  for (int i = 0; i < probes; i++)
    {
      g_autofree char *name = g_strdup_printf ("zz-probe-%d", i);
      g_autofree char *path = g_build_filename (dir, name, nullptr);
      int64_t c0 = now_us ();
      int fd = open (path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
      close (fd);
      while (gbk_dir_model_find (m, name) == GBK_HANDLE_NONE)
        g_main_context_iteration (nullptr, true);
      lat_total += (double) (now_us () - c0) / 1000.0;
      unlink (path);
    }
  while (gbk_dir_model_n_entries (m) != entries)
    g_main_context_iteration (nullptr, true);

  printf ("entries               %zu\n", entries);
  printf ("first paint data      %.1f ms\n", (double) (t.first_change - t.start) / 1000.0);
  printf ("full listing ready    %.1f ms\n", (double) (ready - t.start) / 1000.0);
  printf ("per entry             %.2f us\n", (double) (ready - t.start) / (double) entries);
  printf ("model heap            %.1f MiB (%.0f bytes/entry)\n",
          (double) (heap1 - heap0) / 1048576.0, (double) (heap1 - heap0) / (double) entries);
  printf ("full re-sort          %.1f ms\n", sort_ms);
  printf ("toggle hidden         %.1f ms\n", hidden_ms);
  printf ("live create latency   %.1f ms (avg of %d)\n", lat_total / probes, probes);

  breakdown (dir, types);

  g_object_unref (m);
  while (gbk_scanner_live_workers () > 0)
    g_usleep (1000);
  return 0;
}
