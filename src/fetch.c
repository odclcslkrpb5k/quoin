#include "fetch.h"
#include "edits.h"
#include "job.h"
#include "jobui.h"
#include "lifetime.h"
#include "remote.h"

#define KEY_FETCH "gbk-fetch"

typedef struct
{
  GPtrArray *items;        /* GbkItem* (the originals: names, types, where they are) */
  GAppInfo *app;           /* nullable: each one's default */
  GPtrArray *dirs;         /* char*: the folder each one goes into */
  char *cache;
} Fetch;

static void
fetch_free (Fetch *f)
{
  g_ptr_array_unref (f->items);
  g_clear_object (&f->app);
  g_ptr_array_unref (f->dirs);
  g_free (f->cache);
  g_free (f);
}

/* The app `it` opens with, as asked. */
static GAppInfo *
app_for (const Fetch *f, const GbkItem *it)
{
  return f->app != nullptr ? g_object_ref (f->app)
                           : g_app_info_get_default_for_type (it->mime, false);
}

/* What to look for among running processes to tell the app is still open. */
static char *
executable_of (GAppInfo *app)
{
  const char *exe = app != nullptr ? g_app_info_get_executable (app) : nullptr;
  return exe != nullptr ? g_path_get_basename (exe) : nullptr;
}

/* Which watch each launch belongs to (launches are grouped by app). */
typedef struct
{
  GAppInfo *app;
  uint64_t watch;
} Launch;

static void
launch_free (Launch *l)
{
  g_clear_object (&l->app);
  g_free (l);
}

static void
on_launched (GAppLaunchContext *lc G_GNUC_UNUSED, GAppInfo *info, GVariant *platform,
             GPtrArray *launches)
{
  int pid = 0;
  if (platform == nullptr || !g_variant_lookup (platform, "pid", "i", &pid) || pid <= 0)
    return;
  for (guint i = 0; i < launches->len; i++)
    {
      const Launch *l = g_ptr_array_index (launches, i);
      if (l->app != nullptr && g_app_info_equal (l->app, info))
        gbk_edits_launched (l->watch, (GPid) pid);
    }
}

/* Open copies here (GbkItem*), telling the watches which processes took them. */
static void
launch (GtkWindow *parent, GPtrArray *local, GAppInfo *app, GPtrArray *launches)
{
  GdkAppLaunchContext *lc
      = gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (parent)));
  gbk_connect (G_OBJECT (lc), lc, "launched", G_CALLBACK (on_launched), launches);
  g_autoptr (GError) error = nullptr;
  bool ok = app != nullptr ? gbk_open_with (app, local, G_APP_LAUNCH_CONTEXT (lc), &error)
                           : gbk_open_default (local, G_APP_LAUNCH_CONTEXT (lc), &error);
  g_object_unref (lc);
  if (!ok)
    gbk_jobui_error (parent, "Couldn't open", error->message);
}

static GbkItem *
copy_item (const GbkItem *orig, const char *path)
{
  GbkItem *it = g_new0 (GbkItem, 1);
  it->loc = gbk_loc_new_local (path);
  it->display = g_strdup (orig->display);
  it->mime = g_strdup (orig->mime);
  it->regular = true;
  return it;
}

/* A download folder that got nothing. */
static void
remove_empty (const char *dir)
{
  if (rmdir (dir) != 0 && errno != ENOENT)
    g_warning ("an unused download folder stays in the cache: %s", g_strerror (errno));
}

/* Without a watch (none set up, or the machine's identity unknown), a copy is only for
 * looking at: nobody's saves would reach the other machine. */
static void
make_read_only (const char *path)
{
  struct stat st;
  if (lstat (path, &st) == 0 && S_ISREG (st.st_mode))
    (void) !chmod (path, st.st_mode & 0555);
}

/* Record a downloaded copy and start watching it; false if it can't be (read-only then). */
static bool
watch (const Fetch *f, const GbkFetched *got, const GbkHello *hello, const char *copy, uint64_t *id)
{
  const GbkItem *orig = g_ptr_array_index (f->items, got->index);
  struct stat st;
  if (!gbk_edits_ready () || hello == nullptr || lstat (copy, &st) != 0)
    return false;
  g_autoptr (GAppInfo) app = app_for (f, orig);
  g_autoptr (GbkEditRecord) rec = g_new0 (GbkEditRecord, 1);
  rec->id = g_path_get_basename (g_ptr_array_index (f->dirs, got->index));
  rec->name = g_strdup (got->name);
  rec->host = g_strdup (gbk_loc_host (orig->loc));
  rec->machine_id = g_strdup (hello->machine_id);
  rec->uid = hello->uid;
  rec->remote = g_strdup (got->remote);
  rec->app = g_strdup (app != nullptr ? g_app_info_get_display_name (app) : "");
  rec->base = got->token;
  rec->synced = gbk_file_token_of (&st);
  int lock = gbk_edit_lock (f->cache, rec->id);
  g_autoptr (GError) error = nullptr;
  if (lock < 0 || !gbk_edit_record_save (f->cache, rec, &error))
    {
      if (lock >= 0)
        close (lock);
      return false;
    }
  g_autofree char *exe = executable_of (app);
  *id = gbk_edits_watch (g_steal_pointer (&rec), lock, gbk_loc_path (orig->loc), exe);
  return true;
}

/* "notes.txt from ws1", or "3 files from ws1". */
static char *
describe (const GPtrArray *items)
{
  const GbkItem *first = g_ptr_array_index (items, 0);
  if (items->len == 1)
    return g_strdup_printf ("%s from %s", first->display, gbk_loc_host (first->loc));
  return g_strdup_printf ("%u files from %s", items->len, gbk_loc_host (first->loc));
}

static void
on_fetched (GbkJob *job, GError *error G_GNUC_UNUSED, GtkWindow *parent)
{
  Fetch *f = g_object_get_data (G_OBJECT (job), KEY_FETCH);
  const GPtrArray *fetched = gbk_job_fetched (job);
  if (f == nullptr)
    return;
  /* Folders that got nothing go again (the job said why). */
  g_autofree bool *got_one = g_new0 (bool, f->dirs->len);
  for (guint i = 0; fetched != nullptr && i < fetched->len; i++)
    got_one[((const GbkFetched *) g_ptr_array_index (fetched, i))->index] = true;
  for (guint i = 0; i < f->dirs->len; i++)
    if (!got_one[i])
      remove_empty (g_ptr_array_index (f->dirs, i));
  if (fetched == nullptr || fetched->len == 0)
    {
      g_autofree char *what = describe (f->items);
      g_autofree char *said = g_strdup_printf ("Couldn't open %s", what);
      gbk_edits_say (said);
      return;
    }

  const char *host = gbk_loc_host (((const GbkItem *) g_ptr_array_index (f->items, 0))->loc);
  GbkRemote *r = gbk_remote_default ();
  const GbkConn *c = r != nullptr ? gbk_remote_peek (r, host) : nullptr;
  const GbkHello *hello = c != nullptr ? gbk_conn_hello (c) : nullptr;
  g_autoptr (GPtrArray) local = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  GPtrArray *launches = g_ptr_array_new_with_free_func ((GDestroyNotify) launch_free);
  for (guint i = 0; i < fetched->len; i++)
    {
      const GbkFetched *got = g_ptr_array_index (fetched, i);
      const GbkItem *orig = g_ptr_array_index (f->items, got->index);
      g_autofree char *copy
          = g_build_filename (g_ptr_array_index (f->dirs, got->index), got->name, nullptr);
      uint64_t id = 0;
      if (watch (f, got, hello, copy, &id))
        {
          Launch *l = g_new0 (Launch, 1);
          l->app = app_for (f, orig);
          l->watch = id;
          g_ptr_array_add (launches, l);
        }
      else
        make_read_only (copy);
      g_ptr_array_add (local, copy_item (orig, copy));
    }
  launch (parent, local, f->app, launches);
  g_ptr_array_unref (launches);
  g_autofree char *what = describe (f->items);
  g_autofree char *said = g_strdup_printf ("Opened %s", what);
  gbk_edits_say (said);
}

void
gbk_fetch_open (GtkWindow *parent, GPtrArray *items, GAppInfo *app)
{
  if (items->len == 0)
    return;
  /* Already open this way: the same copy again (two copies would race each other there). */
  g_autoptr (GPtrArray) again = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  g_autoptr (GPtrArray) wanted = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  for (guint i = 0; i < items->len; i++)
    {
      const GbkItem *it = g_ptr_array_index (items, i);
      g_autofree char *copy = gbk_edits_find (gbk_loc_host (it->loc), gbk_loc_path (it->loc));
      g_ptr_array_add (copy != nullptr ? again : wanted,
                       copy != nullptr ? copy_item (it, copy) : gbk_item_copy (it));
    }
  if (again->len > 0)
    {
      g_autoptr (GPtrArray) none = g_ptr_array_new ();
      launch (parent, again, app, none);
    }
  if (wanted->len == 0)
    return;

  Fetch *f = g_new0 (Fetch, 1);
  f->items = g_ptr_array_ref (wanted);
  f->app = app != nullptr ? g_object_ref (app) : nullptr;
  f->dirs = g_ptr_array_new_with_free_func (g_free);
  f->cache = gbk_edits_cache ();
  g_autoptr (GPtrArray) locs = gbk_loc_array_new ();
  for (guint i = 0; i < wanted->len; i++)
    {
      g_autofree char *tmpl = g_build_filename (f->cache, "XXXXXX", nullptr);
      if (g_mkdtemp_full (tmpl, 0700) == nullptr)
        {
          gbk_jobui_error (parent, "Couldn't open", g_strerror (errno));
          for (guint k = 0; k < f->dirs->len; k++)
            remove_empty (g_ptr_array_index (f->dirs, k));
          fetch_free (f);
          return;
        }
      g_ptr_array_add (f->dirs, g_steal_pointer (&tmpl));
      g_ptr_array_add (locs, gbk_loc_ref (((GbkItem *) g_ptr_array_index (wanted, i))->loc));
    }
  g_autoptr (GbkJob) job = gbk_job_new_fetch (locs, f->dirs);
  g_object_set_data_full (G_OBJECT (job), KEY_FETCH, f, (GDestroyNotify) fetch_free);
  gbk_connect (G_OBJECT (parent), job, "finished", G_CALLBACK (on_fetched), parent);
  g_autofree char *what = describe (wanted);
  g_autofree char *said = g_strdup_printf ("Downloading %s…", what);
  gbk_edits_say (said);
  gbk_jobui_start (job, parent);
}
