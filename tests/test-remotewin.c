/* Browsing another machine in a real window, with tests/fake-ssh as ssh (the "other
 * machine" is this one, under a private home). Phase 4 of docs/remote-plan.md: look, never
 * change; survive the connection dying. */
#include "appchoose.h"
#include "browser.h"
#include "edits.h"
#include "jobui.h"
#include "notify.h"
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

static GPtrArray *notices;   /* char*: "summary|kind" of desktop notifications sent */

static void
on_notify (const char *key G_GNUC_UNUSED, GbkNotifyKind kind, const char *summary,
           const char *body G_GNUC_UNUSED)
{
  g_ptr_array_add (notices,
                   g_strdup_printf ("%s|%s", summary, kind == GBK_NOTIFY_LOW ? "low" : "normal"));
}

static bool
notified (void *want)
{
  for (guint i = 0; i < notices->len; i++)
    if (g_str_has_prefix (g_ptr_array_index (notices, i), want))
      return true;
  return false;
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
  notices = g_ptr_array_new_with_free_func (g_free);
  gbk_notify_set_for_test (on_notify);
  gbk_edits_set_in_front_for_test (0);   /* Quoin isn't the window in use: notify too */
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

  g_assert_true (g_str_has_prefix (gbk_window_activity (w), "Opened notes.txt from ws"));
  save_copy (copy, "edited");
  Content edited = { "data/notes.txt", "edited" };
  g_assert_true (wait_until (has_content, &edited));
  /* Said in the status bar, and (Quoin not in front) on the desktop, briefly. */
  char saved_note[] = "Saved notes.txt to ws|low";
  g_assert_true (wait_until (notified, saved_note));
  g_assert_true (g_str_has_prefix (gbk_window_activity (w), "Saved notes.txt to ws at "));

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
  char asked_note[] = "Saving “notes.txt” to ws needs you|normal";
  g_assert_true (notified (asked_note));
  gtk_dialog_response (GTK_DIALOG (asked.found), GBK_JOBUI_KEEP_BOTH);
  Content kept = { "data/notes (1).txt", "mine" };
  g_assert_true (wait_until (has_content, &kept));
  g_assert_true (wait_until (no_jobs, nullptr));
  Content theirs = { "data/notes.txt", "theirs" };
  g_assert_true (has_content (&theirs));
  g_assert_true (
      g_str_has_prefix (gbk_window_activity (w), "Saved notes.txt to ws as “notes (1).txt” at "));
  /* Quoin in front: the status bar says it; nothing on the desktop. */
  gbk_edits_set_in_front_for_test (1);
  guint before = notices->len;
  save_copy (copy, "third");
  Content third = { "data/notes (1).txt", "third" };
  g_assert_true (wait_until (has_content, &third));
  g_assert_true (wait_until (no_jobs, nullptr));
  g_assert_cmpuint (notices->len, ==, before);
  gbk_edits_set_in_front_for_test (-1);
  gbk_notify_set_for_test (nullptr);
  g_clear_pointer (&notices, g_ptr_array_unref);

  gtk_widget_destroy (GTK_WIDGET (w));
  Wanted notice = { .starts = "Quoin keeps running to upload your saves" };
  g_assert_true (wait_until (dialog_shown, &notice));
  g_autofree char *list = nullptr;
  g_object_get (notice.found, "secondary-text", &list, nullptr);
  g_assert_nonnull (strstr (list, "notes.txt (Opener) from ws"));
  g_assert_true (gtk_window_get_application (GTK_WINDOW (notice.found)) == app);   /* holds it */
  g_assert_cmpuint (gbk_edits_count (), ==, 1);    /* the app is still open */

  /* The app closes: watching ends, and the notice, no longer true, goes with it (left up,
   * it once outlived the process: the exit's lifetime check aborted). */
  unlink (hold);
  g_assert_true (wait_until (no_edits, nullptr));
  Wanted still = { .starts = "Quoin keeps running to upload your saves" };
  g_assert_false (dialog_shown (&still));
}

/* The ☰ list's labels. */
static char *
edits_listed (void)
{
  GMenuModel *m = gbk_edits_menu ();
  GString *s = g_string_new ("");
  for (int i = 0; i < g_menu_model_get_n_items (m); i++)
    {
      g_autofree char *label = nullptr;
      g_menu_model_get_item_attribute (m, i, G_MENU_ATTRIBUTE_LABEL, "s", &label);
      g_string_append_printf (s, "%s%s", i > 0 ? "|" : "", label);
    }
  return g_string_free (s, false);
}

static bool
marked (void *data)
{
  Named *n = data;
  GbkDirModel *m = model_of (n->w);
  GbkHandle h = m != nullptr ? gbk_dir_model_find (m, n->name) : GBK_HANDLE_NONE;
  return h != GBK_HANDLE_NONE && gbk_view_is_marked (gbk_window_view (n->w), h);
}

/* Let time pass with the main loop running (a plain sleep would stop Quoin looking). */
static void
run_for (int ms)
{
  int64_t until = g_get_monotonic_time () + (int64_t) ms * 1000;
  while (g_get_monotonic_time () < until)
    {
      while (gtk_events_pending ())
        gtk_main_iteration ();
      g_usleep (10000);
    }
}

static void
touch_home (const char *name)
{
  g_autofree char *p = g_build_filename (home, name, nullptr);
  g_assert_true (g_file_set_contents (p, "", -1, nullptr));
}

static void
unlink_home (const char *name)
{
  g_autofree char *p = g_build_filename (home, name, nullptr);
  unlink (p);
}

/* An app that keeps running after closing the file (one process for many files, as Mousepad
 * and Ristretto are): the watch ends when the file is no longer open, not when the app
 * exits. While open, the file is marked in its folder and listed in the ☰ menu. The last
 * window closing then ends Quoin quietly: no notice, nothing to wait for. */
static void
test_closed_file_ends_watch (void)
{
  put ("data/held.txt", "held");
  touch_home ("hold-file");
  touch_home ("hold-app");
  GbkWindow *w = window_on_remote_data ();
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "held.txt"));
  g_signal_emit_by_name (gbk_window_view (w), "activate");
  g_autofree char *copy = opened_copy ("held.txt");
  g_assert_nonnull (copy);
  Named held = { w, "held.txt" };
  g_assert_true (wait_until (marked, &held));
  g_autofree char *listed = edits_listed ();
  g_assert_cmpstr (listed, ==, "held.txt on ws (Opener)");
  run_for (1500);                            /* several looks: still open, still watched */
  g_assert_cmpuint (gbk_edits_count (), ==, 1);

  unlink_home ("hold-file");                 /* closed; the app goes on running */
  run_for (400);                             /* (the stand-in takes up to 0.1 s to close it) */
  gtk_widget_destroy (GTK_WIDGET (w));       /* the last window: a fresh look, no notice */
  g_assert_true (wait_until (no_edits, nullptr));
  g_autoptr (GPtrArray) dialogs = message_dialogs ();
  g_assert_cmpuint (dialogs->len, ==, 0);
  g_autofree char *none = edits_listed ();
  g_assert_cmpstr (none, ==, "None open");
  unlink_home ("hold-app");
}

/* An app that only watches the file for changes (as GTK editors do, through its folder)
 * keeps it watched, whatever its process is called; when it stops, the watch ends. */
static void
test_watched_file_keeps_watch (void)
{
  put ("data/watched.txt", "w");
  GbkWindow *w = window_on_remote_data ();
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "watched.txt"));
  g_signal_emit_by_name (gbk_window_view (w), "activate");
  g_autofree char *copy = opened_copy ("watched.txt");
  g_assert_nonnull (copy);
  g_autofree char *dir = g_path_get_dirname (copy);
  /* A separate program watching the copy's folder, as a GTK editor does with a file open
   * (its GFileMonitor watches the folder). It stops when ~/hold-watch goes. */
  touch_home ("hold-watch");
  g_autofree char *hold = g_build_filename (home, "hold-watch", nullptr);
  char script[] = "import ctypes, os, sys, time\n"
                  "libc = ctypes.CDLL(None, use_errno=True)\n"
                  "fd = libc.inotify_init1(0)\n"
                  "assert fd >= 0 and libc.inotify_add_watch(fd, sys.argv[1].encode(), 0x2) >= 0\n"
                  "while os.path.exists(sys.argv[2]): time.sleep(0.05)\n";
  char *argv[] = { (char[]) { "python3" }, (char[]) { "-c" }, script, dir, hold, nullptr };
  g_autoptr (GError) error = nullptr;
  g_assert_true (g_spawn_async (nullptr, argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                                nullptr, &error));
  g_assert_no_error (error);
  run_for (1500);                            /* the opener has long gone; the watcher hasn't */
  g_assert_cmpuint (gbk_edits_count (), ==, 1);
  unlink (hold);
  g_assert_true (wait_until (no_edits, nullptr));
  gtk_widget_destroy (GTK_WIDGET (w));
}

/* The kernel's description of inotify watches, as /proc shows it. */
static void
test_fdinfo (void)
{
  const char *info = "pos:\t0\nflags:\t02000000\nmnt_id:\t15\nino:\t1057\n"
                     "inotify wd:2 ino:1a2b sdev:800003 mask:fce ignored_mask:0 "
                     "fhandle-bytes:8 fhandle-type:1 f_handle:2b1a0000\n"
                     "inotify wd:1 ino:ff sdev:fd00001 mask:2 ignored_mask:0\n";
  g_assert_true (gbk_edits_fdinfo_watches (info, makedev (8, 3), 0x1a2b));
  g_assert_true (gbk_edits_fdinfo_watches (info, makedev (253, 1), 0xff));
  g_assert_false (gbk_edits_fdinfo_watches (info, makedev (8, 3), 0xff));
  g_assert_false (gbk_edits_fdinfo_watches (info, makedev (8, 4), 0x1a2b));
  g_assert_false (gbk_edits_fdinfo_watches ("inotify wd:1 ino:zz sdev:1\n", makedev (0, 1), 0));
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

/* The application chooser on screen (borrowed), or nullptr. */
static GtkDialog *
app_chooser (void)
{
  g_autoptr (GList) tops = gtk_window_list_toplevels ();
  for (GList *l = tops; l != nullptr; l = l->next)
    if (GTK_IS_APP_CHOOSER_DIALOG (l->data) && gtk_widget_get_visible (l->data))
      return l->data;
  return nullptr;
}

static bool
chooser_up (void *unused G_GNUC_UNUSED)
{
  return app_chooser () != nullptr;
}

static bool
chooser_gone (void *unused G_GNUC_UNUSED)
{
  return app_chooser () == nullptr;
}

/* A descendant of `w` of type `type` (borrowed), or nullptr. */
static GtkWidget *
find_widget (GtkWidget *w, GType type)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE (w, type))
    return w;
  if (!GTK_IS_CONTAINER (w))
    return nullptr;
  g_autoptr (GList) children = gtk_container_get_children (GTK_CONTAINER (w));
  for (GList *l = children; l != nullptr; l = l->next)
    {
      GtkWidget *found = find_widget (l->data, type);
      if (found != nullptr)
        return found;
    }
  return nullptr;
}

/* Choose Open With ▸ Other Application… for the selection. */
static void
open_with_other (GbkWindow *w)
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
  GtkMenuItem *other = nullptr;
  g_autoptr (GList) items = gtk_container_get_children (GTK_CONTAINER (menu));
  for (GList *l = items; l != nullptr && other == nullptr; l = l->next)
    {
      GtkWidget *sub = GTK_IS_MENU_ITEM (l->data) ? gtk_menu_item_get_submenu (l->data) : nullptr;
      if (sub == nullptr || g_strcmp0 (gtk_menu_item_get_label (l->data), "Open With") != 0)
        continue;
      g_autoptr (GList) apps = gtk_container_get_children (GTK_CONTAINER (sub));
      GList *last = g_list_last (apps);
      g_assert_cmpstr (gtk_menu_item_get_label (last->data), ==, "Other Application…");
      other = last->data;
    }
  g_assert_nonnull (other);
  gtk_menu_item_activate (other);
  gtk_menu_shell_deactivate (GTK_MENU_SHELL (menu));
  while (gtk_events_pending ())
    gtk_main_iteration ();
}

/* A file no application opens asks which should (GTK's chooser), here and there; so does
 * Open With ▸ Other Application…. Cancel opens nothing; a choice opens the files with it,
 * and with the tick box becomes the type's default. */
static void
test_choose_app (void)
{
  put ("local/odd.gbk-nothing-opens-this", "o");
  g_autofree char *local = g_build_filename (home, "local", nullptr);
  g_autoptr (GbkLoc) loc = gbk_loc_new_local (local);
  GbkWindow *w = gbk_window_new (app, ctx, loc);
  gtk_widget_show_all (GTK_WIDGET (w));
  g_assert_true (wait_until (local_ready, w));
  g_autoptr (GAppInfo) opener = nullptr;
  GList *all = g_app_info_get_all ();
  for (GList *l = all; l != nullptr; l = l->next)
    if (g_strcmp0 (g_app_info_get_id (l->data), "gbk-opener.desktop") == 0)
      opener = g_object_ref (l->data);
  g_list_free_full (all, g_object_unref);
  g_assert_nonnull (opener);
  gbk_appchoose_set_pick_for_test (opener);
  const char *mime = "application/octet-stream";
  g_autoptr (GAppInfo) before = g_app_info_get_default_for_type (mime, false);
  g_assert_null (before);

  /* Open: asked instead of an error. Cancel: nothing opens. */
  select_names (w, (const char *const[]) { "odd.gbk-nothing-opens-this", nullptr });
  g_signal_emit_by_name (gbk_window_view (w), "activate");
  g_assert_true (wait_until (chooser_up, nullptr));
  gtk_dialog_response (app_chooser (), GTK_RESPONSE_CANCEL);
  g_assert_true (wait_until (chooser_gone, nullptr));
  run_for (300);
  g_assert_false (exists ("local/odd.gbk-nothing-opens-this.opened"));

  /* Chosen, with the tick box: it opens, and opens that type from now on. */
  g_signal_emit_by_name (gbk_window_view (w), "activate");
  g_assert_true (wait_until (chooser_up, nullptr));
  GtkWidget *always = find_widget (GTK_WIDGET (app_chooser ()), GTK_TYPE_CHECK_BUTTON);
  g_assert_nonnull (always);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (always), true);
  gtk_dialog_response (app_chooser (), GTK_RESPONSE_OK);
  g_autofree char *marker = g_build_filename (local, "odd.gbk-nothing-opens-this.opened", nullptr);
  g_assert_true (wait_until (file_there, marker));
  g_autoptr (GAppInfo) after = g_app_info_get_default_for_type (mime, false);
  g_assert_nonnull (after);
  g_assert_cmpstr (g_app_info_get_id (after), ==, "gbk-opener.desktop");
  g_autofree char *label = first_menu_label (w);
  g_assert_cmpstr (label, ==, "Open With Opener");
  g_app_info_reset_type_associations (mime);
  gtk_widget_destroy (GTK_WIDGET (w));

  /* There: Other Application… opens a copy with the choice. The window going while the
   * chooser is up takes the chooser with it. */
  put ("data/other.txt", "other");
  GbkWindow *rw = window_on_remote_data ();
  select_names (rw, (const char *const[]) { "other.txt", nullptr });
  open_with_other (rw);
  g_assert_true (wait_until (chooser_up, nullptr));
  gtk_dialog_response (app_chooser (), GTK_RESPONSE_OK);
  g_autofree char *copy = opened_copy ("other.txt");
  g_assert_nonnull (copy);
  open_with_other (rw);
  g_assert_true (wait_until (chooser_up, nullptr));
  gtk_widget_destroy (GTK_WIDGET (rw));
  g_assert_true (wait_until (chooser_gone, nullptr));
  gbk_appchoose_set_pick_for_test (nullptr);
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
      "#!/bin/sh\nfor f; do touch \"$f.opened\"; done\n"
      /* With ~/hold-file it keeps the file open first, as an editor with it loaded. */
      "[ -e \"$HOME/hold-file\" ] && exec 3<\"$1\"\nn=0\n"
      "while [ -e \"$HOME/hold-file\" ] && [ $n -lt 300 ]; do sleep 0.1; n=$((n+1)); done\n"
      "exec 3<&-\nn=0\n"
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

  g_application_set_default (G_APPLICATION (app));
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
  g_test_add_func ("/remotewin/closed-file-ends-watch", test_closed_file_ends_watch);
  g_test_add_func ("/remotewin/watched-file-keeps-watch", test_watched_file_keeps_watch);
  g_test_add_func ("/remotewin/fdinfo", test_fdinfo);
  g_test_add_func ("/remotewin/cache-cleanup", test_cache_cleanup);
  g_test_add_func ("/remotewin/open-names-app", test_open_names_app);
  g_test_add_func ("/remotewin/choose-app", test_choose_app);
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
