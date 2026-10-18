#include "fetch.h"
#include "job.h"
#include "jobui.h"
#include "lifetime.h"

#define KEY_FETCH "gbk-fetch"

typedef struct
{
  GPtrArray *items;        /* GbkItem* (the originals: names, types) */
  GAppInfo *app;           /* nullable */
  char *dir;               /* the cache folder they went to */
  GtkWindow *parent;       /* borrowed: the callback runs only while it lives */
} Fetch;

static void
fetch_free (Fetch *f)
{
  g_ptr_array_unref (f->items);
  g_clear_object (&f->app);
  g_free (f->dir);
  g_free (f);
}

char *
gbk_fetch_cache_dir (void)
{
  char *dir = g_build_filename (g_get_user_cache_dir (), "quoin", "remote", nullptr);
  g_mkdir_with_parents (dir, 0700);
  return dir;
}

/* A copy is for looking at: nobody's saves would reach the other machine yet. */
static void
make_read_only (const char *path)
{
  struct stat st;
  if (lstat (path, &st) == 0 && S_ISREG (st.st_mode))
    (void) !chmod (path, st.st_mode & 0555);
}

static void
on_fetched (GbkJob *job, GError *error G_GNUC_UNUSED, GtkWindow *parent)
{
  Fetch *f = g_object_get_data (G_OBJECT (job), KEY_FETCH);
  GPtrArray *created = gbk_job_created (job);
  if (f == nullptr || created == nullptr || created->len == 0)
    return;                              /* nothing came: the job UI has said why */
  g_autoptr (GPtrArray) local = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  for (guint i = 0; i < created->len; i++)
    {
      const char *name = g_ptr_array_index (created, i);
      for (guint k = 0; k < f->items->len; k++)
        {
          const GbkItem *orig = g_ptr_array_index (f->items, k);
          g_autofree char *orig_name = gbk_loc_name (orig->loc);
          if (!g_str_equal (orig_name, name))
            continue;
          g_autofree char *path = g_build_filename (f->dir, name, nullptr);
          make_read_only (path);
          GbkItem *it = g_new0 (GbkItem, 1);
          it->loc = gbk_loc_new_local (path);
          it->display = g_strdup (orig->display);
          it->mime = g_strdup (orig->mime);
          it->regular = orig->regular;
          g_ptr_array_add (local, it);
          break;
        }
    }
  GdkAppLaunchContext *lc
      = gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (parent)));
  g_autoptr (GError) launch_error = nullptr;
  bool ok = f->app != nullptr
                ? gbk_open_with (f->app, local, G_APP_LAUNCH_CONTEXT (lc), &launch_error)
                : gbk_open_default (local, G_APP_LAUNCH_CONTEXT (lc), &launch_error);
  g_object_unref (lc);
  if (!ok)
    gbk_jobui_error (parent, "Couldn't open", launch_error->message);
}

void
gbk_fetch_open (GtkWindow *parent, GPtrArray *items, GAppInfo *app)
{
  if (items->len == 0)
    return;
  g_autofree char *cache = gbk_fetch_cache_dir ();
  g_autofree char *tmpl = g_build_filename (cache, "XXXXXX", nullptr);
  if (g_mkdtemp_full (tmpl, 0700) == nullptr)
    {
      gbk_jobui_error (parent, "Couldn't open", g_strerror (errno));
      return;
    }
  Fetch *f = g_new0 (Fetch, 1);
  f->items = g_ptr_array_ref (items);
  f->app = app != nullptr ? g_object_ref (app) : nullptr;
  f->dir = g_strdup (tmpl);
  f->parent = parent;

  g_autoptr (GPtrArray) locs = gbk_loc_array_new ();
  for (guint i = 0; i < items->len; i++)
    g_ptr_array_add (locs, gbk_loc_ref (((GbkItem *) g_ptr_array_index (items, i))->loc));
  g_autoptr (GbkLoc) dest = gbk_loc_new_local (f->dir);
  g_autoptr (GbkJob) job = gbk_job_new (GBK_OP_COPY, locs, dest);
  g_object_set_data_full (G_OBJECT (job), KEY_FETCH, f, (GDestroyNotify) fetch_free);
  gbk_connect (G_OBJECT (parent), job, "finished", G_CALLBACK (on_fetched), parent);
  gbk_jobui_start (job, parent);
}

/* ---- cleaning up ---- */

static GbkChoice
cleanup_error (const char *path G_GNUC_UNUSED, const GError *error G_GNUC_UNUSED,
               void *data G_GNUC_UNUSED)
{
  return GBK_CHOICE_SKIP;                /* leave what can't go; next time, maybe */
}

static void *
cleanup_worker (void *data)
{
  int days = GPOINTER_TO_INT (data);
  g_autofree char *cache = gbk_fetch_cache_dir ();
  g_autoptr (GDir) d = g_dir_open (cache, 0, nullptr);
  g_autoptr (GPtrArray) old = g_ptr_array_new_with_free_func (g_free);
  time_t limit = time (nullptr) - (time_t) days * 24 * 3600;
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    {
      g_autofree char *path = g_build_filename (cache, name, nullptr);
      struct stat st;
      if (lstat (path, &st) == 0 && S_ISDIR (st.st_mode) && st.st_mtime < limit)
        g_ptr_array_add (old, g_steal_pointer (&path));
    }
  GbkOpsHooks hooks = { .error = cleanup_error };
  g_autoptr (GError) error = nullptr;
  if (old->len > 0 && !gbk_ops_run (GBK_OP_DELETE, old, nullptr, &hooks, nullptr, &error))
    g_warning ("cleaning the remote file cache: %s", error->message);
  return nullptr;
}

void
gbk_fetch_cleanup_start (int days)
{
  GThread *t
      = g_thread_try_new ("gbk-cache-clean", cleanup_worker, GINT_TO_POINTER (days), nullptr);
  if (t != nullptr)
    g_thread_unref (t);
}
