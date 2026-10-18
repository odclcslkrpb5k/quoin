/* Browsing another machine in a real window, with tests/fake-ssh as ssh (the "other
 * machine" is this one, under a private home). Phase 4 of docs/remote-plan.md: look, never
 * change; survive the connection dying. */
#include "browser.h"
#include "fetch.h"
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

/* Nothing that changes anything there: each says so, starts no job, changes nothing. */
static void
test_refusals (void)
{
  put ("data/a.txt", "aaa");
  GbkWindow *w = window_on_remote_data ();
  GbkDirModel *m = model_of (w);
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (m, "a.txt"));
  const char *actions[] = { "cut", "paste", "trash", "delete", "rename" };
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

/* Copy there, paste here: the file arrives. A cut from there is refused (moving comes later). */
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

  /* Cut there, paste here: refused, nothing moves. */
  gbk_window_tab_loc (w, 0);
  gtk_notebook_set_current_page (
      GTK_NOTEBOOK (gtk_widget_get_ancestor (GTK_WIDGET (gbk_window_view (w)), GTK_TYPE_NOTEBOOK)),
      0);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "a.txt"));
  g_action_group_activate_action (G_ACTION_GROUP (w), "cut", nullptr);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_assert_cmpstr (gbk_window_status (w), ==, "Not possible on another machine yet");
  g_assert_true (exists ("data/a.txt"));
  gtk_widget_destroy (GTK_WIDGET (w));
}

/* Opening a file there opens a read-only copy here, in the cache. */
static void
test_open_copy (void)
{
  put ("data/notes.txt", "notes");
  GbkWindow *w = window_on_remote_data ();
  GbkDirModel *m = model_of (w);
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (m, "notes.txt"));
  g_signal_emit_by_name (gbk_window_view (w), "activate");
  /* The stand-in default application leaves "<file>.opened" beside what it opened. */
  g_autofree char *cache = g_build_filename (home, ".cache", "quoin", "remote", nullptr);
  g_autofree char *opened = nullptr;
  int64_t deadline = g_get_monotonic_time () + INT64_C (20) * G_USEC_PER_SEC;
  while (opened == nullptr && g_get_monotonic_time () < deadline)
    {
      while (gtk_events_pending ())
        gtk_main_iteration ();
      g_autoptr (GDir) d = g_dir_open (cache, 0, nullptr);
      const char *name;
      while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
        {
          g_autofree char *marker = g_build_filename (cache, name, "notes.txt.opened", nullptr);
          if (g_file_test (marker, G_FILE_TEST_EXISTS))
            opened = g_build_filename (cache, name, "notes.txt", nullptr);
        }
      g_usleep (5000);
    }
  g_assert_nonnull (opened);
  g_autofree char *content = nullptr;
  g_assert_true (g_file_get_contents (opened, &content, nullptr, nullptr));
  g_assert_cmpstr (content, ==, "notes");
  struct stat st;
  g_assert_cmpint (stat (opened, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0222, ==, 0);      /* read-only: saves can't go anywhere yet */
  g_autofree char *dir = g_path_get_dirname (opened);
  g_assert_cmpint (stat (dir, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0700);   /* private */
  gtk_widget_destroy (GTK_WIDGET (w));
}

static bool
gone (void *path)
{
  return !g_file_test (path, G_FILE_TEST_EXISTS);
}

/* Cache folders older than the limit go (read-only files in them too); newer ones stay. */
static void
test_cache_cleanup (void)
{
  g_autofree char *cache = gbk_fetch_cache_dir ();
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
  gbk_fetch_cleanup_start (GBK_FETCH_KEEP_DAYS);
  g_assert_true (wait_until (gone, old));
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
  g_assert_true (g_file_set_contents (opener, "#!/bin/sh\nfor f; do touch \"$f.opened\"; done\n",
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

  g_test_add_func ("/remotewin/browse", test_browse);
  g_test_add_func ("/remotewin/refusals", test_refusals);
  g_test_add_func ("/remotewin/properties", test_properties);
  g_test_add_func ("/remotewin/disconnect-and-back", test_disconnect_and_back);
  g_test_add_func ("/remotewin/network-action", test_network_action);
  g_test_add_func ("/remotewin/copy-paste-here", test_copy_paste_here);
  g_test_add_func ("/remotewin/open-copy", test_open_copy);
  g_test_add_func ("/remotewin/cache-cleanup", test_cache_cleanup);
  g_test_add_func ("/remotewin/open-names-app", test_open_names_app);
  int status = g_test_run ();

  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_object_unref (app);
  gbk_context_unref (ctx);
  wait_until (no_conns, nullptr);
  if (gbk_lifetime_live () != 0)
    gbk_fatal ("%d registrations leaked", gbk_lifetime_live ());
  return status;
}
