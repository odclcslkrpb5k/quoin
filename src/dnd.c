#include "dnd.h"
#include "cliptext.h"
#include "edits.h"
#include "fetch.h"
#include "job.h"
#include "lifetime.h"

#define LOCS_TARGET "x-special/quoin-locations"   /* as the clipboard's (cliptext.h) */
#define KEY_DIRS "gbk-drag-dirs"

GtkTargetList *
gbk_dnd_dest_targets (void)
{
  GtkTargetList *t = gtk_target_list_new (nullptr, 0);
  gtk_target_list_add (t, gdk_atom_intern_static_string (LOCS_TARGET), 0, GBK_DND_LOCS);
  gtk_target_list_add (t, gdk_atom_intern_static_string ("text/uri-list"), 0, GBK_DND_URIS);
  return t;
}

GdkDragAction
gbk_dnd_action (GdkDragContext *ctx)
{
  GdkDragAction offered = gdk_drag_context_get_actions (ctx);
  if (offered & GDK_ACTION_ASK)
    return GDK_ACTION_ASK;
  if (offered & GDK_ACTION_COPY)
    return GDK_ACTION_COPY;
  if (offered & GDK_ACTION_MOVE)
    return GDK_ACTION_MOVE;
  return 0;
}

GPtrArray *
gbk_dnd_locs (GtkSelectionData *sd)
{
  const guchar *data = gtk_selection_data_get_data (sd);
  int len = gtk_selection_data_get_length (sd);
  bool cut;
  if (data == nullptr || len <= 0)
    return gbk_loc_array_new ();
  if (gtk_selection_data_get_target (sd) == gdk_atom_intern_static_string (LOCS_TARGET))
    return gbk_clip_parse_locs ((const char *) data, (size_t) len, &cut);
  g_autoptr (GPtrArray) paths = gbk_clip_parse ((const char *) data, (size_t) len, false, &cut);
  return gbk_loc_array_from_local (paths);
}

/* ---- dragging out ---- */

struct GbkDragOut
{
  GPtrArray *locs;         /* GbkLoc* */
  GPtrArray *paths;        /* char*: what other apps get, when the files are here */
  bool copies;             /* elsewhere, and other apps get copies (open[] or downloaded) */
  GPtrArray *open;         /* char* or nullptr, per loc: its copy already being edited here */
  GArray *slot;            /* int, per loc: its place in the download (dirs), or -1 */
  GbkJob *job;             /* downloading the others, or nullptr */
  GPtrArray *dirs;         /* char*: the download's folders (one each) */
  bool taken;              /* another app got the copies */
};

/* Nobody answers a download's questions: a file that can't come stops it. */
static void
on_fetch_question (GbkJob *job, GbkQuestion *q G_GNUC_UNUSED, void *unused G_GNUC_UNUSED)
{
  gbk_job_answer (job, GBK_CHOICE_CANCEL, false);
}

/* Download folders nobody took, with whatever is in them (only the copies: ours alone). */
static void
remove_dirs (GPtrArray *dirs)
{
  for (guint i = 0; i < dirs->len; i++)
    {
      const char *dir = g_ptr_array_index (dirs, i);
      g_autoptr (GDir) d = g_dir_open (dir, 0, nullptr);
      const char *name;
      while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
        {
          g_autofree char *p = g_build_filename (dir, name, nullptr);
          if (unlink (p) != 0)
            g_warning ("a dragged copy stays in the cache: %s", g_strerror (errno));
        }
      if (rmdir (dir) != 0 && errno != ENOENT)
        g_warning ("a drag's download folder stays in the cache: %s", g_strerror (errno));
    }
}

static void
on_unwanted_fetched (GbkJob *job, GError *error G_GNUC_UNUSED, void *unused G_GNUC_UNUSED)
{
  remove_dirs (g_object_get_data (G_OBJECT (job), KEY_DIRS));
}

/* Copies for other apps: those already being edited here, the rest downloaded, each into a
 * folder of its own. False if a folder can't be made (then other apps get text only). */
static bool
fetch (GbkDragOut *d)
{
  g_autofree char *cache = gbk_edits_cache ();
  d->dirs = g_ptr_array_new_with_free_func (g_free);
  d->open = g_ptr_array_new_with_free_func (g_free);
  d->slot = g_array_new (false, false, sizeof (int));
  g_autoptr (GPtrArray) wanted = gbk_loc_array_new ();
  for (guint i = 0; i < d->locs->len; i++)
    {
      GbkLoc *loc = g_ptr_array_index (d->locs, i);
      char *open = gbk_fetch_open_copy (loc);
      int slot = -1;
      g_ptr_array_add (d->open, open);
      if (open == nullptr)
        {
          g_autofree char *tmpl = g_build_filename (cache, "XXXXXX", nullptr);
          if (g_mkdtemp_full (tmpl, 0700) == nullptr)
            {
              g_warning ("no download folder for a drag: %s", g_strerror (errno));
              remove_dirs (d->dirs);
              return false;
            }
          slot = (int) d->dirs->len;
          g_ptr_array_add (d->dirs, g_steal_pointer (&tmpl));
          g_ptr_array_add (wanted, gbk_loc_ref (loc));
        }
      g_array_append_val (d->slot, slot);
    }
  if (wanted->len == 0)
    return true;                       /* all already here */
  d->job = gbk_job_new_fetch (wanted, d->dirs);
  g_object_set_data_full (G_OBJECT (d->job), KEY_DIRS, g_ptr_array_ref (d->dirs),
                          (GDestroyNotify) g_ptr_array_unref);
  gbk_connect (G_OBJECT (d->job), d->job, "question", G_CALLBACK (on_fetch_question), nullptr);
  gbk_job_start (d->job);
  return true;
}

GbkDragOut *
gbk_drag_out_new (const GPtrArray *locs, bool fetchable)
{
  GbkDragOut *d = g_new0 (GbkDragOut, 1);
  d->locs = gbk_loc_array_new ();
  for (guint i = 0; i < locs->len; i++)
    g_ptr_array_add (d->locs, gbk_loc_ref (g_ptr_array_index (locs, i)));
  d->paths = gbk_loc_array_as_local (d->locs);
  if (d->paths == nullptr && fetchable && d->locs->len > 0)
    d->copies = fetch (d);
  return d;
}

GtkTargetList *
gbk_drag_out_targets (const GbkDragOut *d)
{
  GtkTargetList *t = gtk_target_list_new (nullptr, 0);
  gtk_target_list_add (t, gdk_atom_intern_static_string (LOCS_TARGET), 0, GBK_DND_LOCS);
  if (d->paths != nullptr || d->copies)
    gtk_target_list_add (t, gdk_atom_intern_static_string ("text/uri-list"), 0, GBK_DND_URIS);
  gtk_target_list_add_text_targets (t, GBK_DND_TEXT);
  return t;
}

/* The downloaded copy in folder `slot`, or nullptr. */
static const GbkFetched *
fetched_in (const GbkDragOut *d, int slot)
{
  const GPtrArray *got = gbk_job_fetched (d->job);
  for (guint i = 0; got != nullptr && i < got->len; i++)
    {
      const GbkFetched *f = g_ptr_array_index (got, i);
      if ((int) f->index == slot)
        return f;
    }
  return nullptr;
}

/* The copies, in drag order, once every one is here; nullptr until then (or if one
 * didn't come). */
static GPtrArray *
copies (const GbkDragOut *d)
{
  if (!d->copies || (d->job != nullptr && gbk_job_running (d->job)))
    return nullptr;
  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < d->locs->len; i++)
    {
      const char *open = g_ptr_array_index (d->open, i);
      int slot = g_array_index (d->slot, int, i);
      const GbkFetched *f = slot >= 0 ? fetched_in (d, slot) : nullptr;
      if (open != nullptr)
        g_ptr_array_add (paths, g_strdup (open));
      else if (f != nullptr)
        g_ptr_array_add (paths,
                         g_build_filename (g_ptr_array_index (d->dirs, slot), f->name, nullptr));
      else
        return nullptr;                  /* this one didn't come */
    }
  return g_steal_pointer (&paths);
}

static char *
uri_list (GbkDragOut *d)
{
  g_autoptr (GPtrArray) fetched = d->paths == nullptr ? copies (d) : nullptr;
  GPtrArray *paths = d->paths != nullptr ? d->paths : fetched;
  if (paths == nullptr)
    {
      gbk_edits_say (d->job != nullptr && gbk_job_running (d->job)
                         ? "Still downloading what was dragged: drop it again in a moment"
                         : "What was dragged couldn't be downloaded");
      return nullptr;
    }
  d->taken = d->taken || fetched != nullptr;
  return gbk_clip_format_uri_list (paths);
}

/* Paths here; elsewhere host:/path, never a bare path that would name a file here. */
static char *
text (const GbkDragOut *d)
{
  if (d->paths != nullptr)
    return gbk_clip_format_text (d->paths);
  GString *s = g_string_new ("");
  for (guint i = 0; i < d->locs->len; i++)
    {
      g_autofree char *shown = gbk_loc_display (g_ptr_array_index (d->locs, i));
      g_string_append_printf (s, "%s%s", i > 0 ? "\n" : "", shown);
    }
  return g_string_free (s, false);
}

char *
gbk_drag_out_data (GbkDragOut *d, guint info)
{
  switch (info)
    {
    case GBK_DND_LOCS: return gbk_clip_format_locs (d->locs, false);
    case GBK_DND_URIS: return uri_list (d);
    case GBK_DND_TEXT: return text (d);
    default: return nullptr;            /* not one we offered */
    }
}

void
gbk_drag_out_get (GbkDragOut *d, GtkSelectionData *sd, guint info)
{
  g_autofree char *data = gbk_drag_out_data (d, info);
  if (data == nullptr)
    return;
  if (info == GBK_DND_TEXT)
    gtk_selection_data_set_text (sd, data, -1);
  else
    gtk_selection_data_set (sd, gtk_selection_data_get_target (sd), 8, (const guchar *) data,
                            (int) strlen (data));
}

void
gbk_drag_out_dropped (GbkDragOut *d)
{
  if (!d->taken || d->job == nullptr)
    return;                            /* nothing new went to an app */
  d->taken = false;                    /* once */
  guint watched = 0, n = 0;
  for (guint i = 0; i < d->locs->len; i++)
    {
      int slot = g_array_index (d->slot, int, i);
      const GbkFetched *f = slot >= 0 ? fetched_in (d, slot) : nullptr;
      if (f == nullptr)
        continue;
      n++;
      const char *dir = g_ptr_array_index (d->dirs, slot);
      uint64_t id;
      if (gbk_fetch_watch (g_ptr_array_index (d->locs, i), f, dir, "", nullptr, true, &id))
        watched++;
      else
        {
          g_autofree char *copy = g_build_filename (dir, f->name, nullptr);
          gbk_fetch_read_only (copy);
        }
    }
  /* The copies are the app's now: no longer the drag's to remove. */
  g_ptr_array_set_size (d->dirs, 0);
  if (watched == 0)
    return;
  const GbkLoc *first = g_ptr_array_index (d->locs, 0);
  g_autofree char *name = gbk_loc_display_name (first);
  g_autofree char *said = n == 1 ? g_strdup_printf ("Dropped %s from %s: saves there go back", name,
                                                    gbk_loc_host (first))
                                 : g_strdup_printf ("Dropped %u files from %s: saves there go back",
                                                    n, gbk_loc_host (first));
  gbk_edits_say (said);
}

void
gbk_drag_out_free (GbkDragOut *d)
{
  if (d == nullptr)
    return;
  if (d->job != nullptr && !d->taken && d->dirs->len > 0)
    {
      if (gbk_job_running (d->job))
        {
          gbk_connect (G_OBJECT (d->job), d->job, "finished", G_CALLBACK (on_unwanted_fetched),
                       nullptr);
          gbk_job_cancel (d->job);
        }
      else
        remove_dirs (d->dirs);
    }
  g_clear_object (&d->job);       /* one still running keeps itself alive */
  g_clear_pointer (&d->dirs, g_ptr_array_unref);
  g_clear_pointer (&d->open, g_ptr_array_unref);
  g_clear_pointer (&d->slot, g_array_unref);
  g_clear_pointer (&d->paths, g_ptr_array_unref);
  g_ptr_array_unref (d->locs);
  g_free (d);
}

bool
gbk_drag_out_ready (const GbkDragOut *d)
{
  g_autoptr (GPtrArray) got = d->paths == nullptr ? copies (d) : nullptr;
  return d->paths != nullptr || got != nullptr;
}
