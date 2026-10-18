/* Browsing another machine in a real window, with tests/fake-ssh as ssh (the "other
 * machine" is this one, under a private home). Phase 4 of docs/remote-plan.md: look, never
 * change; survive the connection dying. */
#include "browser.h"
#include "edits.h"
#include "jobui.h"
#include "conn.h"
#include "job.h"
#include "lifetime.h"
#include "props.h"
#include "window.h"

static GtkApplication *app;
static GbkContext *ctx;
static char *home;
static char *pid_path;

static bool
wait_until (bool (*pred) (void *), void *data)
{
  int64_t deadline = g_get_monotonic_time () + INT64_C (20) * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < deadline)
    {
      while (gtk_events_pending ())
        gtk_main_iteration ();
      if (pred (data))
        return true;
      g_usleep (1000);
    }
  return pred (data);
}

static GbkDirModel *
model_of (GbkWindow *w)
{
  return gbk_view_get_model (gbk_window_view (w));
}

/* The active tab lists a name. */
typedef struct
{
  GbkWindow *w;
  const char *name;
} Named;

static bool
has_name (void *data)
{
  const Named *n = data;
  GbkDirModel *m = model_of (n->w);
  return m != nullptr && gbk_dir_model_find (m, n->name) != GBK_HANDLE_NONE;
}

static GbkDirState
state_of (GbkWindow *w)
{
  const GbkDirModel *m = model_of (w);
  return m != nullptr ? gbk_dir_model_state (m) : GBK_DIR_ERROR;
}

typedef struct
{
  GbkWindow *w;
  const char *host;
  const char *path;
} At;

static bool
remote_ready (void *data)
{
  const At *a = data;
  const GbkLoc *loc = gbk_window_loc (a->w);
  return g_strcmp0 (gbk_loc_host (loc), a->host) == 0 && g_str_equal (gbk_loc_path (loc), a->path)
         && state_of (a->w) == GBK_DIR_READY;
}

static bool
disconnected (void *w)
{
  return state_of (w) == GBK_DIR_DISCONNECTED;
}

static bool
ready (void *w)
{
  return state_of (w) == GBK_DIR_READY;
}

static bool
has_new_there (void *w)
{
  return gbk_dir_model_find (model_of (w), "new-there") != GBK_HANDLE_NONE;
}

static void
put (const char *rel, const char *content)
{
  g_autofree char *p = g_build_filename (home, rel, nullptr);
  g_autofree char *d = g_path_get_dirname (p);
  g_mkdir_with_parents (d, 0755);
  g_assert_true (g_file_set_contents (p, content, -1, nullptr));
}

static bool
exists (const char *rel)
{
  g_autofree char *p = g_build_filename (home, rel, nullptr);
  return g_file_test (p, G_FILE_TEST_EXISTS);
}

/* A window at /tmp that then opens ws:data (relative: in the other machine's home). */
static GbkWindow *
window_on_remote_data (void)
{
  g_autoptr (GbkLoc) start = gbk_loc_new_local ("/tmp");
  GbkWindow *w = gbk_window_new (app, ctx, start);
  gtk_widget_show_all (GTK_WIDGET (w));
  GbkAddress a;
  g_assert_true (gbk_address_parse ("ws:data", &a));
  gbk_window_open_address (w, &a);
  gbk_address_clear (&a);
  g_autofree char *data = g_build_filename (home, "data", nullptr);
  At at = { w, "ws", data };
  if (!wait_until (remote_ready, &at))
    gbk_fatal ("never got to ws:data (status: %s)", gbk_window_status (w));
  return w;
}

static void
test_browse (void)
{
  put ("data/a.txt", "aaa");
  put ("data/sub/inner", "");
  GbkWindow *w = window_on_remote_data ();
  GbkDirModel *m = model_of (w);
  g_assert_cmpuint (gbk_dir_model_n_entries (m), ==, 2);
  g_assert_cmpuint (gbk_dir_model_find (m, "a.txt"), !=, GBK_HANDLE_NONE);

  /* The title names the machine; so does the path bar's first button. */
  g_autofree char *title_want = g_strdup_printf ("ws:%s/data", home);
  g_assert_cmpstr (gtk_window_get_title (GTK_WINDOW (w)), ==, title_want);
  g_autofree char *bar = gbk_pathbar_describe (gbk_window_pathbar (w));
  g_autofree char *home_name = g_path_get_basename (home);
  g_autofree char *bar_want = g_strdup_printf ("ws: %s|[data]", home_name);
  g_assert_cmpstr (bar, ==, bar_want);

  /* Up, then Home: both stay on that machine. */
  g_action_group_activate_action (G_ACTION_GROUP (w), "up", nullptr);
  At up = { w, "ws", home };
  g_assert_true (wait_until (remote_ready, &up));
  g_action_group_activate_action (G_ACTION_GROUP (w), "home", nullptr);
  g_assert_true (wait_until (remote_ready, &up));

  /* Live: a file made there shows up. */
  put ("new-there", "n");
  g_assert_true (wait_until (has_new_there, w));
  gtk_widget_destroy (GTK_WIDGET (w));
}

/* Nothing that changes anything there, apart from copies and moves: each says so, starts
 * no job, changes nothing. */
static void
test_refusals (void)
{
  put ("data/a.txt", "aaa");
  GbkWindow *w = window_on_remote_data ();
  GbkDirModel *m = model_of (w);
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (m, "a.txt"));
  const char *actions[] = { "trash", "delete", "rename" };
  for (size_t i = 0; i < G_N_ELEMENTS (actions); i++)
    {
      g_action_group_activate_action (G_ACTION_GROUP (w), actions[i], nullptr);
      while (gtk_events_pending ())
        gtk_main_iteration ();
      g_test_message ("%s: %s", actions[i], gbk_window_status (w));
      g_assert_cmpstr (gbk_window_status (w), ==, "Not possible on another machine yet");
      g_assert_cmpint (gbk_job_live (), ==, 0);
    }
  g_assert_true (exists ("data/a.txt"));
  gtk_widget_destroy (GTK_WIDGET (w));
}

static bool
file_there (void *path)
{
  return g_file_test (path, G_FILE_TEST_EXISTS);
}

static bool
no_jobs (void *data G_GNUC_UNUSED)
{
  return gbk_job_live () == 0;
}

/* Copy there, paste here: the file arrives. Cut there, paste here: it moves. */
static void
test_copy_paste_here (void)
{
  put ("data/a.txt", "aaa");
  put ("here/keep", "");
  GbkWindow *w = window_on_remote_data ();
  GbkDirModel *m = model_of (w);
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (m, "a.txt"));
  g_action_group_activate_action (G_ACTION_GROUP (w), "copy", nullptr);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_assert_cmpstr (gbk_window_status (w), ==, "1 item copied");

  g_autofree char *here = g_build_filename (home, "here", nullptr);
  g_autoptr (GbkLoc) here_loc = gbk_loc_new_local (here);
  gbk_window_new_tab (w, here_loc);
  g_action_group_activate_action (G_ACTION_GROUP (w), "paste", nullptr);
  g_autofree char *arrived = g_build_filename (here, "a.txt", nullptr);
  g_assert_true (wait_until (file_there, arrived));
  g_assert_true (wait_until (no_jobs, nullptr));
  g_autofree char *content = nullptr;
  g_assert_true (g_file_get_contents (arrived, &content, nullptr, nullptr));
  g_assert_cmpstr (content, ==, "aaa");

  /* Cut there, paste into another folder here: it moves. */
  put ("data/b.txt", "bbb");
  GtkNotebook *tabs = GTK_NOTEBOOK (
      gtk_widget_get_ancestor (GTK_WIDGET (gbk_window_view (w)), GTK_TYPE_NOTEBOOK));
  gtk_notebook_set_current_page (tabs, 0);
  Named b_txt = { w, "b.txt" };
  g_assert_true (wait_until (has_name, &b_txt));
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "b.txt"));
  g_action_group_activate_action (G_ACTION_GROUP (w), "cut", nullptr);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_assert_cmpstr (gbk_window_status (w), ==, "1 item cut");
  gtk_notebook_set_current_page (tabs, 1);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_action_group_activate_action (G_ACTION_GROUP (w), "paste", nullptr);
  g_autofree char *moved = g_build_filename (here, "b.txt", nullptr);
  g_assert_true (wait_until (file_there, moved));
  g_assert_true (wait_until (no_jobs, nullptr));
  g_assert_false (exists ("data/b.txt"));
  gtk_widget_destroy (GTK_WIDGET (w));
}

/* Copy here, paste there: the file arrives there. Cut here, paste there: it moves. */
static void
test_paste_there (void)
{
  put ("data/keep", "");
  put ("local/up.txt", "up");
  put ("local/mv.txt", "mv");
  g_autofree char *local = g_build_filename (home, "local", nullptr);
  g_autoptr (GbkLoc) local_loc = gbk_loc_new_local (local);
  GbkWindow *w = window_on_remote_data ();
  gbk_window_new_tab (w, local_loc);
  Named up_txt = { w, "up.txt" };
  g_assert_true (wait_until (has_name, &up_txt));
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "up.txt"));
  g_action_group_activate_action (G_ACTION_GROUP (w), "copy", nullptr);
  GtkNotebook *tabs = GTK_NOTEBOOK (
      gtk_widget_get_ancestor (GTK_WIDGET (gbk_window_view (w)), GTK_TYPE_NOTEBOOK));
  gtk_notebook_set_current_page (tabs, 0);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_action_group_activate_action (G_ACTION_GROUP (w), "paste", nullptr);
  g_autofree char *arrived = g_build_filename (home, "data", "up.txt", nullptr);
  g_assert_true (wait_until (file_there, arrived));
  g_assert_true (wait_until (no_jobs, nullptr));
  g_autofree char *content = nullptr;
  g_assert_true (g_file_get_contents (arrived, &content, nullptr, nullptr));
  g_assert_cmpstr (content, ==, "up");
  g_assert_true (exists ("local/up.txt"));

  gtk_notebook_set_current_page (tabs, 1);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "mv.txt"));
  g_action_group_activate_action (G_ACTION_GROUP (w), "cut", nullptr);
  gtk_notebook_set_current_page (tabs, 0);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_action_group_activate_action (G_ACTION_GROUP (w), "paste", nullptr);
  g_autofree char *moved = g_build_filename (home, "data", "mv.txt", nullptr);
  g_assert_true (wait_until (file_there, moved));
  g_assert_true (wait_until (no_jobs, nullptr));
  g_assert_false (exists ("local/mv.txt"));
  gtk_widget_destroy (GTK_WIDGET (w));
}

/* The copy here of `name` that the stand-in app was asked to open ("<file>.opened" appears
 * beside it). g_free. */
static char *
opened_copy (const char *name)
{
  g_autofree char *cache = g_build_filename (home, ".cache", "quoin", "remote", nullptr);
  g_autofree char *marker_name = g_strconcat (name, ".opened", nullptr);
  int64_t deadline = g_get_monotonic_time () + INT64_C (20) * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < deadline)
    {
      while (gtk_events_pending ())
        gtk_main_iteration ();
      g_autoptr (GDir) d = g_dir_open (cache, 0, nullptr);
      const char *entry;
      while (d != nullptr && (entry = g_dir_read_name (d)) != nullptr)
        {
          g_autofree char *marker = g_build_filename (cache, entry, marker_name, nullptr);
          if (g_file_test (marker, G_FILE_TEST_EXISTS))
            {
              unlink (marker);
              return g_build_filename (cache, entry, name, nullptr);
            }
        }
      g_usleep (5000);
    }
  return nullptr;
}

/* Message dialogs on screen now (borrowed). */
static GPtrArray *
message_dialogs (void)
{
  GPtrArray *out = g_ptr_array_new ();
  GList *tops = gtk_window_list_toplevels ();
  for (GList *l = tops; l != nullptr; l = l->next)
    if (GTK_IS_MESSAGE_DIALOG (l->data) && gtk_widget_get_visible (l->data))
      g_ptr_array_add (out, l->data);
  g_list_free (tops);
  return out;
}

typedef struct
{
  const char *starts;     /* the start of its main text */
  GtkMessageDialog *found;
} Wanted;

static bool
dialog_shown (void *data)
{
  Wanted *want = data;
  g_autoptr (GPtrArray) all = message_dialogs ();
  for (guint i = 0; i < all->len; i++)
    {
      g_autofree char *text = nullptr;
      g_object_get (g_ptr_array_index (all, i), "text", &text, nullptr);
      if (text != nullptr && g_str_has_prefix (text, want->starts))
        {
          want->found = g_ptr_array_index (all, i);
          return true;
        }
    }
  return false;
}

static bool
no_edits (void *data G_GNUC_UNUSED)
{
  return gbk_edits_count () == 0;
}

typedef struct
{
  const char *rel;
  const char *content;
} Content;

static bool
has_content (void *data)
{
  const Content *c = data;
  g_autofree char *p = g_build_filename (home, c->rel, nullptr);
  g_autofree char *text = nullptr;
  return g_file_get_contents (p, &text, nullptr, nullptr) && g_str_equal (text, c->content);
}

static void
save_copy (const char *copy, const char *content)
{
  g_usleep (20000);                      /* a later mtime, on any filesystem */
  g_assert_true (g_file_set_contents (copy, content, -1, nullptr));
}

/* Opening a file there opens a private, writable copy here; its saves go back there. A
 * change made there meanwhile is asked about (Keep Both here), and later saves go to the
 * kept copy. When the last window closes, a notice names the file and its app, and Quoin
 * keeps watching until that app has closed. */
static void
test_edit_upload (void)
{
  put ("data/notes.txt", "notes");
  g_autofree char *hold = g_build_filename (home, "hold-app", nullptr);
  g_assert_true (g_file_set_contents (hold, "", -1, nullptr));   /* the app stays open */
  GbkWindow *w = window_on_remote_data ();
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "notes.txt"));
  g_signal_emit_by_name (gbk_window_view (w), "activate");
  g_autofree char *copy = opened_copy ("notes.txt");
  g_assert_nonnull (copy);
  g_autofree char *content = nullptr;
  g_assert_true (g_file_get_contents (copy, &content, nullptr, nullptr));
  g_assert_cmpstr (content, ==, "notes");
  struct stat st;
  g_assert_cmpint (stat (copy, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0600);   /* the app can save it; nobody else */
  g_autofree char *dir = g_path_get_dirname (copy);
  g_assert_cmpint (stat (dir, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);
  g_assert_cmpuint (gbk_edits_count (), ==, 1);

  save_copy (copy, "edited");
  Content edited = { "data/notes.txt", "edited" };
  g_assert_true (wait_until (has_content, &edited));

  /* Opened again: the same copy, no second download. */
  g_signal_emit_by_name (gbk_window_view (w), "activate");
  g_autofree char *again = opened_copy ("notes.txt");
  g_assert_cmpstr (again, ==, copy);
  g_assert_cmpuint (gbk_edits_count (), ==, 1);

  g_assert_true (wait_until (no_jobs, nullptr));
  g_usleep (20000);
  put ("data/notes.txt", "theirs");
  save_copy (copy, "mine");
  Wanted asked = { .starts = "“notes.txt” changed on the other machine" };
  g_assert_true (wait_until (dialog_shown, &asked));
  gtk_dialog_response (GTK_DIALOG (asked.found), GBK_JOBUI_KEEP_BOTH);
  Content kept = { "data/notes (1).txt", "mine" };
  g_assert_true (wait_until (has_content, &kept));
  g_assert_true (wait_until (no_jobs, nullptr));
  Content theirs = { "data/notes.txt", "theirs" };
  g_assert_true (has_content (&theirs));
  save_copy (copy, "third");
  Content third = { "data/notes (1).txt", "third" };
  g_assert_true (wait_until (has_content, &third));

  gtk_widget_destroy (GTK_WIDGET (w));
  Wanted notice = { .starts = "Quoin keeps running to upload your saves" };
  g_assert_true (wait_until (dialog_shown, &notice));
  g_autofree char *list = nullptr;
  g_object_get (notice.found, "secondary-text", &list, nullptr);
  g_assert_nonnull (strstr (list, "notes.txt (Opener) from ws"));
  gtk_widget_destroy (GTK_WIDGET (notice.found));
  g_assert_cmpuint (gbk_edits_count (), ==, 1);    /* the app is still open */

  unlink (hold);                                   /* the app closes: watching ends */
  g_assert_true (wait_until (no_edits, nullptr));
}

static guint unsent_offered;
static GbkEditsAnswer unsent_reply;

static GbkEditsAnswer
answer_unsent (guint n)
{
  unsent_offered += n;
  return unsent_reply;
}

/* A copy as an earlier Quoin would have left it: downloaded from data/`name`, then changed
 * here (`edited`) and never uploaded. `machine_id` as that machine answered then. */
static void
leave_unsent (const char *id, const char *name, const char *machine_id, const char *edited)
{
  g_autofree char *cache = gbk_edits_cache ();
  g_autofree char *remote = g_build_filename (home, "data", name, nullptr);
  g_autofree char *dir = g_build_filename (cache, id, nullptr);
  g_autofree char *copy = g_build_filename (dir, name, nullptr);
  g_mkdir_with_parents (dir, 0700);
  struct stat st;
  g_assert_cmpint (stat (remote, &st), ==, 0);
  g_autoptr (GbkEditRecord) rec = g_new0 (GbkEditRecord, 1);
  rec->id = g_strdup (id);
  rec->name = g_strdup (name);
  rec->host = g_strdup ("ws");
  rec->machine_id = g_strdup (machine_id);
  rec->uid = (uint32_t) getuid ();
  rec->remote = g_strdup (remote);
  rec->app = g_strdup ("Opener");
  rec->base = gbk_file_token_of (&st);
  g_assert_true (g_file_set_contents (copy, "as downloaded", -1, nullptr));
  g_assert_cmpint (stat (copy, &st), ==, 0);
  rec->synced = gbk_file_token_of (&st);
  g_autoptr (GError) error = nullptr;
  g_assert_true (gbk_edit_record_save (cache, rec, &error));
  save_copy (copy, edited);
}

static bool
offered (void *want)
{
  return unsent_offered >= GPOINTER_TO_UINT (want);
}

/* Copies an earlier Quoin left changed and unsent are offered at the next start: Upload
 * sends them (once), Discard settles them; and one that came from a machine that now
 * answers differently is never sent there. */
static void
test_unsent_offered (void)
{
  g_autofree char *mine = gbk_conn_local_machine_id ();
  gbk_edits_set_unsent_answer_for_test (answer_unsent);
  unsent_offered = 0;

  put ("data/left.txt", "there");
  leave_unsent ("unsentA", "left.txt", mine, "changed offline");
  unsent_reply = GBK_EDITS_UPLOAD;
  gbk_edits_scan ();
  Content sent = { "data/left.txt", "changed offline" };
  g_assert_true (wait_until (has_content, &sent));
  g_assert_true (wait_until (no_edits, nullptr));
  g_assert_cmpuint (unsent_offered, ==, 1);

  put ("data/other.txt", "there");
  leave_unsent ("unsentB", "other.txt", mine, "never mind");
  unsent_reply = GBK_EDITS_DISCARD;
  gbk_edits_scan ();
  g_assert_true (wait_until (offered, GUINT_TO_POINTER (2)));
  Content untouched = { "data/other.txt", "there" };
  g_assert_true (has_content (&untouched));
  gbk_edits_scan ();                     /* settled now: neither is offered again */
  put ("data/elsewhere.txt", "there");
  leave_unsent ("unsentC", "elsewhere.txt", "0123456789abcdef", "for another machine");
  unsent_reply = GBK_EDITS_UPLOAD;
  gbk_edits_scan ();
  g_assert_true (wait_until (offered, GUINT_TO_POINTER (3)));
  Wanted refused = { .starts = "Not uploaded" };
  g_assert_true (wait_until (dialog_shown, &refused));
  gtk_widget_destroy (GTK_WIDGET (refused.found));
  g_assert_true (wait_until (no_edits, nullptr));
  Content kept_there = { "data/elsewhere.txt", "there" };
  g_assert_true (has_content (&kept_there));
  g_assert_cmpuint (unsent_offered, ==, 3);
  gbk_edits_set_unsent_answer_for_test (nullptr);
}

static bool
gone (void *path)
{
  return !g_file_test (path, G_FILE_TEST_EXISTS);
}

/* Old copies go: folders without a record (read-only copies from before records), and
 * finished records with their copies; newer ones stay. */
static void
test_cache_cleanup (void)
{
  g_autofree char *cache = gbk_edits_cache ();
  g_autofree char *old = g_build_filename (cache, "oldone", nullptr);
  g_autofree char *young = g_build_filename (cache, "young", nullptr);
  g_autofree char *old_file = g_build_filename (old, "f.txt", nullptr);
  g_mkdir_with_parents (old, 0700);
  g_mkdir_with_parents (young, 0700);
  g_assert_true (g_file_set_contents (old_file, "x", -1, nullptr));
  g_assert_cmpint (chmod (old_file, 0444), ==, 0);
  const time_t then = time (nullptr) - (time_t) 10 * 24 * 3600;
  const struct timespec ten_days_ago[2] = { { then, 0 }, { then, 0 } };
  g_assert_cmpint (utimensat (AT_FDCWD, old, ten_days_ago, 0), ==, 0);

  put ("data/done.txt", "x");
  leave_unsent ("finished", "done.txt", "", "x");
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkEditRecord) rec = gbk_edit_record_load (cache, "finished", &error);
  g_assert_no_error (error);
  g_autofree char *copy = gbk_edit_record_copy (cache, rec);
  struct stat st;
  g_assert_cmpint (stat (copy, &st), ==, 0);
  rec->synced = gbk_file_token_of (&st);         /* uploaded: settled */
  g_assert_true (gbk_edit_record_save (cache, rec, &error));
  g_autofree char *ini = g_build_filename (cache, "finished.ini", nullptr);
  g_assert_cmpint (utimensat (AT_FDCWD, ini, ten_days_ago, 0), ==, 0);
  g_autofree char *finished_dir = g_build_filename (cache, "finished", nullptr);

  gbk_edits_scan ();
  g_assert_true (wait_until (gone, old));
  g_assert_true (wait_until (gone, ini));
  g_assert_true (wait_until (gone, finished_dir));
  g_assert_true (g_file_test (young, G_FILE_TEST_IS_DIR));
}

/* The context menu for the selection, as the Menu key opens it; its first entry's label. */
static char *
first_menu_label (GbkWindow *w)
{
  GbkView *v = gbk_window_view (w);
  gtk_widget_grab_focus (GTK_WIDGET (v));
  GdkEvent *ev = gdk_event_new (GDK_KEY_PRESS);
  ev->key.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (w)));
  ev->key.keyval = GDK_KEY_Menu;
  ev->key.time = GDK_CURRENT_TIME;
  gdk_event_set_device (ev, gdk_seat_get_keyboard (gdk_display_get_default_seat (
                                gtk_widget_get_display (GTK_WIDGET (w)))));
  gtk_main_do_event (ev);
  gdk_event_free (ev);
  GList *menus = gtk_menu_get_for_attach_widget (GTK_WIDGET (v));
  g_assert_cmpuint (g_list_length (menus), ==, 1);
  GtkMenu *menu = menus->data;
  g_autoptr (GList) items = gtk_container_get_children (GTK_CONTAINER (menu));
  char *label = g_strdup (gtk_menu_item_get_label (GTK_MENU_ITEM (items->data)));
  gtk_menu_shell_deactivate (GTK_MENU_SHELL (menu));
  while (gtk_events_pending ())
    gtk_main_iteration ();
  return label;
}

static void
select_names (GbkWindow *w, const char *const *names)
{
  GbkDirModel *m = model_of (w);
  GbkHandle h[8] = { 0 };
  size_t n = 0;
  for (; n < G_N_ELEMENTS (h) && names[n] != nullptr; n++)
    h[n] = gbk_dir_model_find (m, names[n]);
  gbk_view_set_selection (gbk_window_view (w), h, n);
}

static bool
local_ready (void *w)
{
  return gbk_loc_is_local (gbk_window_loc (w)) && ready (w);
}

/* Open names the application it will use, here and there, when there is one. */
static void
test_open_names_app (void)
{
  put ("local/notes.txt", "n");
  put ("local/data.gbk-nothing-opens-this", "d");
  put ("local/folder/x", "");
  g_autofree char *local = g_build_filename (home, "local", nullptr);
  g_autoptr (GbkLoc) loc = gbk_loc_new_local (local);
  GbkWindow *w = gbk_window_new (app, ctx, loc);
  gtk_widget_show_all (GTK_WIDGET (w));
  g_assert_true (wait_until (local_ready, w));

  select_names (w, (const char *const[]) { "notes.txt", nullptr });
  g_autofree char *one = first_menu_label (w);
  g_assert_cmpstr (one, ==, "Open With Opener");
  /* Different applications (one has none): plain Open. */
  select_names (w, (const char *const[]) { "notes.txt", "data.gbk-nothing-opens-this", nullptr });
  g_autofree char *mixed = first_menu_label (w);
  g_assert_cmpstr (mixed, ==, "Open");
  select_names (w, (const char *const[]) { "folder", nullptr });
  g_autofree char *folder = first_menu_label (w);
  g_assert_cmpstr (folder, ==, "Open");
  gtk_widget_destroy (GTK_WIDGET (w));

  /* On another machine: the same, for the copy it opens. */
  put ("data/notes.txt", "notes");
  GbkWindow *rw = window_on_remote_data ();
  select_names (rw, (const char *const[]) { "notes.txt", nullptr });
  g_autofree char *remote = first_menu_label (rw);
  g_assert_cmpstr (remote, ==, "Open With Opener");
  gtk_widget_destroy (GTK_WIDGET (rw));
}

static bool
props_loaded (void *p)
{
  return gbk_props_loaded (p);
}

static void
test_properties (void)
{
  put ("data/a.txt", "aaa");
  GbkWindow *w = window_on_remote_data ();
  g_autofree char *path = g_build_filename (home, "data", "a.txt", nullptr);
  g_autoptr (GbkLoc) loc = gbk_loc_new_remote ("ws", path);
  GbkProps *p = gbk_props_new (GTK_WINDOW (w), loc, "text/plain");
  gtk_widget_show (GTK_WIDGET (p));
  g_assert_true (wait_until (props_loaded, p));
  g_assert_cmpstr (gbk_props_value (p, "Name"), ==, "a.txt");
  g_assert_cmpstr (gbk_props_value (p, "Size"), ==, "3 bytes (3 bytes)");
  /* Read there; changing them there comes later. */
  g_assert_false (gtk_widget_is_sensitive (GTK_WIDGET (gbk_props_permission (p, 0, 0))));
  gtk_widget_destroy (GTK_WIDGET (p));
  gtk_widget_destroy (GTK_WIDGET (w));
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

/* The connection dies: the listing stays (dimmed, the bar says why), and comes back on
 * retry with the selection where it was. */
static void
test_disconnect_and_back (void)
{
  put ("data/a.txt", "aaa");
  put ("data/b.txt", "bbb");
  GbkWindow *w = window_on_remote_data ();
  GbkDirModel *m = model_of (w);
  GbkHandle a = gbk_dir_model_find (m, "a.txt");
  gbk_view_select_only (gbk_window_view (w), a);
  size_t before = gbk_dir_model_n_entries (m);

  g_assert_cmpint (kill (server_pid (), SIGKILL), ==, 0);
  g_assert_true (wait_until (disconnected, w));
  g_assert_true (model_of (w) == m);              /* the same folder, not walked away from */
  g_assert_cmpuint (gbk_dir_model_n_entries (m), ==, before);   /* the last listing, kept */
  g_assert_nonnull (strstr (gbk_window_status (w), "Disconnected from ws"));

  GtkWidget *browser = gtk_widget_get_ancestor (GTK_WIDGET (gbk_window_view (w)), GBK_TYPE_BROWSER);
  gbk_browser_reconnect (GBK_BROWSER (browser));
  g_assert_true (wait_until (ready, w));
  size_t n;
  g_autofree GbkHandle *sel = gbk_view_selected (gbk_window_view (w), &n);
  g_assert_cmpuint (n, ==, 1);
  g_assert_cmpuint (sel[0], ==, a);               /* same entry, same handle */
  gtk_widget_destroy (GTK_WIDGET (w));
}

/* The ☰ menu's Network items open a machine's home; a name ssh would misread does nothing. */
static void
test_network_action (void)
{
  g_autoptr (GbkLoc) start = gbk_loc_new_local ("/tmp");
  GbkWindow *w = gbk_window_new (app, ctx, start);
  gtk_widget_show_all (GTK_WIDGET (w));
  g_action_group_activate_action (G_ACTION_GROUP (w), "network",
                                  g_variant_new_string ("-oProxyCommand=x"));
  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_assert_null (gbk_loc_host (gbk_window_loc (w)));
  g_action_group_activate_action (G_ACTION_GROUP (w), "network", g_variant_new_string ("ws"));
  At at = { w, "ws", home };
  g_assert_true (wait_until (remote_ready, &at));
  gtk_widget_destroy (GTK_WIDGET (w));
}

static bool
no_conns (void *data G_GNUC_UNUSED)
{
  return gbk_conn_live () == 0;
}

int
main (int argc, char **argv)
{
  home = g_dir_make_tmp ("gbk-rwin-XXXXXX", nullptr);
  g_setenv ("HOME", home, true);
  g_autofree char *data_home = g_build_filename (home, "xdg-data", nullptr);
  g_setenv ("XDG_DATA_HOME", data_home, true);
  /* A stand-in default application for text/plain: it marks what it was asked to open. */
  g_autofree char *config_home = g_build_filename (home, "xdg-config", nullptr);
  g_autofree char *apps = g_build_filename (data_home, "applications", nullptr);
  g_mkdir_with_parents (apps, 0755);
  g_mkdir_with_parents (config_home, 0755);
  g_autofree char *opener = g_build_filename (home, "opener", nullptr);
  /* It stays open while ~/hold-app exists (at most 30 s). */
  g_assert_true (g_file_set_contents (
      opener,
      "#!/bin/sh\nfor f; do touch \"$f.opened\"; done\nn=0\n"
      "while [ -e \"$HOME/hold-app\" ] && [ $n -lt 300 ]; do sleep 0.1; n=$((n+1)); done\n",
      -1, nullptr));
  g_assert_cmpint (chmod (opener, 0755), ==, 0);
  g_autofree char *desktop = g_strdup_printf (
      "[Desktop Entry]\nType=Application\nName=Opener\nExec=%s %%F\nMimeType=text/plain;\n",
      opener);
  g_autofree char *desktop_path = g_build_filename (apps, "gbk-opener.desktop", nullptr);
  g_assert_true (g_file_set_contents (desktop_path, desktop, -1, nullptr));
  g_autofree char *mimeapps = g_build_filename (config_home, "mimeapps.list", nullptr);
  g_assert_true (g_file_set_contents (
      mimeapps, "[Default Applications]\ntext/plain=gbk-opener.desktop\n", -1, nullptr));
  g_setenv ("XDG_CONFIG_HOME", config_home, true);
  pid_path = g_build_filename (home, "ssh.pid", nullptr);
  g_setenv ("FAKE_SSH_PIDFILE", pid_path, true);
  g_autofree char *fake
      = g_build_filename (g_getenv ("GBK_SOURCE_ROOT"), "tests", "fake-ssh", nullptr);
  g_setenv ("QUOIN_SSH", fake, true);
  gtk_test_init (&argc, &argv, nullptr);
  ctx = gbk_context_new ("/nonexistent/gbk-test.conf");
  app = gtk_application_new ("net.gbk.QuoinRemoteTest", G_APPLICATION_NON_UNIQUE);
  g_autoptr (GError) error = nullptr;
  if (!g_application_register (G_APPLICATION (app), nullptr, &error))
    gbk_fatal ("register: %s", error->message);

  gbk_edits_set_timing_for_test (50, 400, 400);
  gbk_edits_init (app, ctx);
  g_test_add_func ("/remotewin/browse", test_browse);
  g_test_add_func ("/remotewin/refusals", test_refusals);
  g_test_add_func ("/remotewin/properties", test_properties);
  g_test_add_func ("/remotewin/disconnect-and-back", test_disconnect_and_back);
  g_test_add_func ("/remotewin/network-action", test_network_action);
  g_test_add_func ("/remotewin/copy-paste-here", test_copy_paste_here);
  g_test_add_func ("/remotewin/paste-there", test_paste_there);
  g_test_add_func ("/remotewin/edit-upload", test_edit_upload);
  g_test_add_func ("/remotewin/unsent-offered", test_unsent_offered);
  g_test_add_func ("/remotewin/cache-cleanup", test_cache_cleanup);
  g_test_add_func ("/remotewin/open-names-app", test_open_names_app);
  int status = g_test_run ();

  while (gtk_events_pending ())
    gtk_main_iteration ();
  gbk_edits_shutdown ();
  g_object_unref (app);
  gbk_context_unref (ctx);
  wait_until (no_conns, nullptr);
  if (gbk_lifetime_live () != 0)
    gbk_fatal ("%d registrations leaked", gbk_lifetime_live ());
  return status;
}
