/* Browser window: navigation, history, hidden toggle, vanished directories (under Xvfb). */
#include "lifetime.h"
#include <gdk/gdkx.h>
#include <X11/extensions/XTest.h>
#include "cliptext.h"
#include "job.h"
#include "props.h"
#include "window.h"

/* Windows and tabs on local folders, by path. */
static const char *
wpath (GbkWindow *w)
{
  const char *p;
  g_assert_true (gbk_loc_as_local (gbk_window_loc (w), &p));
  return p;
}

[[maybe_unused]] static const char *
tab_path (GbkWindow *w, int i)
{
  const char *p;
  g_assert_true (gbk_loc_as_local (gbk_window_tab_loc (w, i), &p));
  return p;
}

static GbkWindow *
window_at (const GtkApplication *app, GbkContext *ctx, const char *path)
{
  g_autoptr (GbkLoc) loc = gbk_loc_new_local (path);
  return gbk_window_new (app, ctx, loc);
}

static void
open_at (GbkWindow *w, const char *path)
{
  g_autoptr (GbkLoc) loc = gbk_loc_new_local (path);
  gbk_window_open (w, loc);
}

[[maybe_unused]] static void
new_tab_at (GbkWindow *w, const char *path)
{
  g_autoptr (GbkLoc) loc = gbk_loc_new_local (path);
  gbk_window_new_tab (w, loc);
}

static GtkApplication *app;
static GbkContext *ctx;

typedef bool (*Pred) (void *data);

static bool
wait_until (Pred pred, void *data)
{
  int64_t deadline = g_get_monotonic_time () + INT64_C (10) * G_USEC_PER_SEC;
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

static bool
ready (void *data)
{
  GbkDirModel *m = model_of (data);
  return m != nullptr && gbk_dir_model_state (m) == GBK_DIR_READY;
}

typedef struct
{
  GbkWindow *w;
  const char *path;
} PathCtx;

static bool
at_path_and_ready (void *data)
{
  PathCtx *c = data;
  return g_str_equal (wpath (c->w), c->path) && ready (c->w);
}

static void
wait_at (GbkWindow *w, const char *path)
{
  PathCtx c = { w, path };
  if (!wait_until (at_path_and_ready, &c))
    gbk_fatal ("expected to reach %s, at %s (state %d, error %d, live %d)", path, wpath (w),
               model_of (w) ? (int) gbk_dir_model_state (model_of (w)) : -1,
               model_of (w) ? gbk_dir_model_error (model_of (w)) : -1,
               model_of (w) ? (int) gbk_dir_model_live (model_of (w)) : -1);
}

static void
mkdirs (const char *root, const char *const *rel)
{
  for (size_t i = 0; rel[i] != nullptr; i++)
    {
      g_autofree char *p = g_build_filename (root, rel[i], nullptr);
      if (g_str_has_suffix (rel[i], "/"))
        g_mkdir_with_parents (p, 0755);
      else
        g_file_set_contents (p, "hello", 5, nullptr);
    }
}

static void
rm_rf (const char *path)
{
  g_autoptr (GDir) d = g_dir_open (path, 0, nullptr);
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    {
      g_autofree char *child = g_build_filename (path, name, nullptr);
      if (g_file_test (child, G_FILE_TEST_IS_DIR) && !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
        rm_rf (child);
      else
        unlink (child);
    }
  rmdir (path);
}

static const char *
cursor_name (GbkWindow *w)
{
  const GbkEntry *e = gbk_dir_model_get (model_of (w), gbk_view_cursor (gbk_window_view (w)));
  return e != nullptr ? gbk_entry_name (e) : "(none)";
}

static void
activate (GbkWindow *w, const char *action)
{
  g_action_group_activate_action (G_ACTION_GROUP (w), action, nullptr);
}

/* A real mouse button press and release on `win`, as the main loop delivers them. */
static void
mouse_button (GdkWindow *win, guint button)
{
  GdkEventType types[] = { GDK_BUTTON_PRESS, GDK_BUTTON_RELEASE };
  for (size_t i = 0; i < G_N_ELEMENTS (types); i++)
    {
      GdkEvent *ev = gdk_event_new (types[i]);
      ev->button.window = g_object_ref (win);
      ev->button.button = button;
      ev->button.x = ev->button.y = 3;
      ev->button.time = GDK_CURRENT_TIME;
      gdk_event_set_device (
          ev, gdk_seat_get_pointer (gdk_display_get_default_seat (gdk_window_get_display (win))));
      gtk_main_do_event (ev);
      gdk_event_free (ev);
    }
}

static void
test_navigation (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root,
          (const char *const[]) { "a/", "b/", "sub/", "sub/x", "sub/y", ".hid", "f.txt", nullptr });
  g_autofree char *sub = g_build_filename (root, "sub", nullptr);

  GbkWindow *w = window_at (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  g_assert_nonnull (strstr (gbk_window_status (w), "3 folders · 1 file   "));
  g_assert_true (g_str_has_suffix (gbk_window_status (w), " free"));

  /* Enter "sub" by activating it, like a double-click. */
  GbkView *v = gbk_window_view (w);
  gbk_view_select_only (v, gbk_dir_model_find (model_of (w), "sub"));
  g_assert_true (g_str_has_prefix (gbk_window_status (w), "sub   "));   /* name, free space */
  gbk_view_select_only (v, gbk_dir_model_find (model_of (w), "f.txt"));
  g_assert_true (g_str_has_prefix (gbk_window_status (w), "f.txt  5 bytes   "));
  gbk_view_select_all (v);
  g_assert_true (g_str_has_prefix (gbk_window_status (w), "4 selected  5 bytes   "));
  gbk_view_select_only (v, gbk_dir_model_find (model_of (w), "sub"));
  g_signal_emit_by_name (v, "activate");
  wait_at (w, sub);
  g_assert_nonnull (strstr (gbk_window_status (w), "2 files"));

  /* Up: back in root with "sub" focused. */
  g_assert_cmpstr (gtk_window_get_title (GTK_WINDOW (w)), ==, sub);
  activate (w, "up");
  wait_at (w, root);
  g_assert_cmpstr (cursor_name (w), ==, "sub");
  g_assert_cmpstr (gtk_window_get_title (GTK_WINDOW (w)), ==, root);

  activate (w, "back");
  wait_at (w, sub);
  activate (w, "forward");
  wait_at (w, root);
  activate (w, "forward");                      /* nothing forward: no-op */
  wait_at (w, root);

  /* Alt+Home: the home folder. */
  activate (w, "home");
  wait_at (w, g_get_home_dir ());
  activate (w, "back");
  wait_at (w, root);

  /* The mouse's side buttons (8 back, 9 forward), over the files and over a path bar
   * button, which would otherwise take the press itself. */
  mouse_button (gtk_widget_get_window (GTK_WIDGET (gbk_window_view (w))), 8);
  wait_at (w, sub);
  mouse_button (gtk_widget_get_window (GTK_WIDGET (gbk_window_view (w))), 9);
  wait_at (w, root);
  GtkWidget *crumb = gbk_pathbar_button (gbk_window_pathbar (w), root);
  mouse_button (gtk_button_get_event_window (GTK_BUTTON (crumb)), 8);
  wait_at (w, sub);
  crumb = gbk_pathbar_button (gbk_window_pathbar (w), sub);
  mouse_button (gtk_button_get_event_window (GTK_BUTTON (crumb)), 9);
  wait_at (w, root);

  /* The title is the full folder path, nothing else. */
  g_assert_cmpstr (gtk_window_get_title (GTK_WINDOW (w)), ==, root);

  /* Ctrl+H, reflected by the status bar's ".*" toggle; clicking the toggle works too. */
  GtkToggleButton *hidden = gbk_window_hidden_button (w);
  g_assert_false (gtk_toggle_button_get_active (hidden));
  size_t before = gbk_dir_model_n_visible (model_of (w));
  activate (w, "show-hidden");
  g_assert_cmpuint (gbk_dir_model_n_visible (model_of (w)), ==, before + 1);
  g_assert_true (gtk_toggle_button_get_active (hidden));
  gtk_button_clicked (GTK_BUTTON (hidden));
  g_assert_cmpuint (gbk_dir_model_n_visible (model_of (w)), ==, before);
  g_assert_false (gtk_toggle_button_get_active (hidden));
  gtk_button_clicked (GTK_BUTTON (hidden));
  g_assert_cmpuint (gbk_dir_model_n_visible (model_of (w)), ==, before + 1);
  activate (w, "show-hidden");
  g_assert_false (gtk_toggle_button_get_active (hidden));

  /* The sort button shows the sort and cycles name -> modified -> size -> name. */
  GtkButton *sort = gbk_window_sort_button (w);
  g_assert_cmpstr (gtk_button_get_label (sort), ==, "↑Name");
  gtk_button_clicked (sort);
  g_assert_cmpstr (gtk_button_get_label (sort), ==, "↓Modified");
  g_assert_cmpint (gbk_dir_model_sort (model_of (w)), ==, GBK_SORT_MTIME);
  gtk_button_clicked (sort);
  g_assert_cmpstr (gtk_button_get_label (sort), ==, "↓Size");
  g_assert_cmpint (gbk_dir_model_sort (model_of (w)), ==, GBK_SORT_SIZE);
  gtk_button_clicked (sort);
  g_assert_cmpstr (gtk_button_get_label (sort), ==, "↑Name");
  g_assert_cmpint (gbk_dir_model_sort (model_of (w)), ==, GBK_SORT_NAME);
  /* The action (menus) updates the button; unknown values are ignored. */
  g_action_group_activate_action (G_ACTION_GROUP (w), "sort", g_variant_new_string ("size"));
  g_assert_cmpstr (gtk_button_get_label (sort), ==, "↓Size");
  g_action_group_activate_action (G_ACTION_GROUP (w), "sort", g_variant_new_string ("bogus"));
  g_assert_cmpint (gbk_dir_model_sort (model_of (w)), ==, GBK_SORT_SIZE);
  g_action_group_activate_action (G_ACTION_GROUP (w), "sort", g_variant_new_string ("name"));

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

static void
test_vanishing_directory (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "keep/", "doomed/", "doomed/deeper/", "doomed/deeper/f",
                                        nullptr });
  g_autofree char *deeper = g_build_filename (root, "doomed", "deeper", nullptr);
  g_autofree char *doomed = g_build_filename (root, "doomed", nullptr);

  GbkWindow *w = window_at (app, ctx, deeper);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, deeper);

  /* The directory we're in, and its parent, disappear: walk up to the nearest survivor. */
  rm_rf (doomed);
  g_assert_false (g_file_test (doomed, G_FILE_TEST_EXISTS));
  wait_at (w, root);

  /* Opening something that doesn't exist also lands on the nearest ancestor. */
  g_autofree char *nowhere = g_build_filename (root, "no", "such", "dir", nullptr);
  open_at (w, nowhere);
  wait_at (w, root);

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

static void
test_terminal_missing (void)
{
  /* The default terminal isn't installed in the test image: F4 must report, not crash. */
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  GbkWindow *w = window_at (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  activate (w, "terminal");
  g_assert_true (g_str_has_prefix (gbk_window_status (w), "Terminal:"));
  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

/* ---- context menu ---- */

static GtkMenu *
open_context_menu (GbkWindow *w)
{
  /* A real Menu key press, dispatched the way the main loop does it. */
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
  if (g_list_length (menus) != 1)
    gbk_fatal ("expected one context menu, got %u", g_list_length (menus));
  return GTK_MENU (menus->data);
}

static char *
menu_labels (GtkMenu *menu)
{
  GString *s = g_string_new ("");
  g_autoptr (GList) children = gtk_container_get_children (GTK_CONTAINER (menu));
  for (GList *l = children; l != nullptr; l = l->next)
    {
      if (s->len > 0)
        g_string_append_c (s, '|');
      const char *label = GTK_IS_SEPARATOR_MENU_ITEM (l->data)
                              ? "-"
                              : gtk_menu_item_get_label (GTK_MENU_ITEM (l->data));
      g_string_append (s, label);
    }
  return g_string_free (s, false);
}

static void
choose (GtkMenu *menu, const char *label)
{
  g_autoptr (GList) children = gtk_container_get_children (GTK_CONTAINER (menu));
  for (GList *l = children; l != nullptr; l = l->next)
    if (!GTK_IS_SEPARATOR_MENU_ITEM (l->data)
        && g_strcmp0 (gtk_menu_item_get_label (GTK_MENU_ITEM (l->data)), label) == 0)
      {
        gtk_menu_item_activate (GTK_MENU_ITEM (l->data));
        gtk_menu_shell_deactivate (GTK_MENU_SHELL (menu));
        while (gtk_events_pending ())
          gtk_main_iteration ();
        return;
      }
  gbk_fatal ("no menu item \"%s\"", label);
}

/* A real right-button press on the path bar button for `path`, as the main loop delivers it.
 * Returns the menu it opened. */
static GtkMenu *
right_click_bar (GbkWindow *w, const char *path)
{
  GtkWidget *button = gbk_pathbar_button (gbk_window_pathbar (w), path);
  GdkEvent *ev = gdk_event_new (GDK_BUTTON_PRESS);
  ev->button.window = g_object_ref (gtk_button_get_event_window (GTK_BUTTON (button)));
  ev->button.button = GDK_BUTTON_SECONDARY;
  ev->button.x = ev->button.y = 2;
  ev->button.time = GDK_CURRENT_TIME;
  gdk_event_set_device (
      ev, gdk_seat_get_pointer (gdk_display_get_default_seat (gtk_widget_get_display (button))));
  gtk_main_do_event (ev);
  gdk_event_free (ev);
  GList *menus = gtk_menu_get_for_attach_widget (GTK_WIDGET (gbk_window_pathbar (w)));
  if (g_list_length (menus) != 1)
    gbk_fatal ("expected one path bar menu, got %u", g_list_length (menus));
  return menus->data;
}

static bool
bar_menus_gone (void *data)
{
  return gtk_menu_get_for_attach_widget (data) == nullptr;
}

static bool
menus_gone (void *data)
{
  return gtk_menu_get_for_attach_widget (GTK_WIDGET (gbk_window_view (data))) == nullptr;
}

static bool
file_exists (void *data)
{
  return g_file_test (data, G_FILE_TEST_EXISTS);
}

/* The CLIPBOARD's text, read the way the app must (asynchronously). */
static void
got_text (GtkSelectionData *sd, void *data)
{
  g_autofree guchar *t = gtk_selection_data_get_text (sd);
  *(char **) data = g_strdup (t != nullptr ? (const char *) t : "");
}

static bool
string_set (void *data)
{
  return *(char **) data != nullptr;
}

static char *
clipboard_text (GbkWindow *w)
{
  char *text = nullptr;
  gbk_clipboard_request (G_OBJECT (w),
                         gtk_widget_get_clipboard (GTK_WIDGET (w), GDK_SELECTION_CLIPBOARD),
                         gdk_atom_intern_static_string ("UTF8_STRING"), got_text, &text, nullptr);
  g_assert_true (wait_until (string_set, &text));
  return text;
}

static bool
has_marker (void *data)
{
  return gbk_dir_model_find (model_of (data), "ran") != GBK_HANDLE_NONE;
}

static void
test_context_menu (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  g_autofree char *marker = g_build_filename (root, "ran", nullptr);
  g_autofree char *script = g_strdup_printf ("#!/bin/sh\ntouch '%s'\n", marker);
  g_autofree char *prog = g_build_filename (root, "prog", nullptr);
  g_assert_true (g_file_set_contents (prog, script, -1, nullptr));
  chmod (prog, 0755);
  /* "old" is older than "new": sorting by modified must flip them. */
  mkdirs (root, (const char *const[]) { "a-old", "b-new", nullptr });
  g_autofree char *old = g_build_filename (root, "a-old", nullptr);
  struct timespec ts[2] = { { .tv_sec = 1000 }, { .tv_sec = 1000 } };
  utimensat (AT_FDCWD, old, ts, 0);

  GbkWindow *w = window_at (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  GbkDirModel *m = model_of (w);

  /* Background menu: the directory itself, plus view options. */
  GtkMenu *menu = open_context_menu (w);
  g_autofree char *labels = menu_labels (menu);
  g_assert_cmpstr (
      labels, ==,
      "Create Folder…|Create File…|-|Paste|Copy Full Path|-|Open Terminal Here|-|"
      "Sort by Name|Sort by Date Modified|Sort by Size|-|Show Hidden Files|-|Properties…");
  const char *first = gbk_entry_name (gbk_dir_model_get (m, gbk_dir_model_visible_handle (m, 0)));
  g_assert_cmpstr (first, ==, "a-old");
  choose (menu, "Sort by Date Modified");
  g_assert_cmpint (gbk_dir_model_sort (m), ==, GBK_SORT_MTIME);
  g_assert_cmpstr (gbk_entry_name (gbk_dir_model_get (m, gbk_dir_model_visible_handle (m, 2))), ==,
                   "a-old");                     /* oldest last */
  g_assert_true (wait_until (menus_gone, w));       /* destroyed after closing */

  /* Copy Full Path on empty space: the folder's own path. */
  choose (open_context_menu (w), "Copy Full Path");
  g_autofree char *here_text = clipboard_text (w);
  g_assert_cmpstr (here_text, ==, root);
  g_assert_cmpstr (gbk_window_status (w), ==, "Path copied");
  g_assert_true (wait_until (menus_gone, w));

  /* Item menu on an extensionless executable: Execute runs it. */
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (m, "prog"));
  menu = open_context_menu (w);
  g_autofree char *labels2 = menu_labels (menu);
  g_assert_nonnull (strstr (labels2, "Open|"));
  g_assert_nonnull (strstr (labels2, "Execute|Execute in Terminal"));
  choose (menu, "Execute");
  g_assert_true (wait_until (file_exists, marker));
  g_assert_true (wait_until (menus_gone, w));

  /* Copy Full Path on items: one path per line, no trailing newline (a terminal would run
   * it on paste). */
  choose (open_context_menu (w), "Copy Full Path");
  g_autofree char *one = clipboard_text (w);
  g_assert_cmpstr (one, ==, prog);
  g_assert_true (wait_until (menus_gone, w));
  /* The marker Execute created must be in the folder's model before selecting all. */
  g_assert_true (wait_until (has_marker, w));
  gbk_view_select_all (gbk_window_view (w));
  choose (open_context_menu (w), "Copy Full Path");
  g_autofree char *all = clipboard_text (w);
  /* Visible order: newest first since the earlier menu choice ("ran" is Execute's marker). */
  g_autofree char *expect
      = g_strdup_printf ("%s\n%s/b-new\n%s\n%s/a-old", marker, root, prog, root);
  g_assert_cmpstr (all, ==, expect);
  g_assert_cmpstr (gbk_window_status (w), ==, "4 paths copied");
  g_assert_true (wait_until (menus_gone, w));

  /* Double-click (activate) on it runs it too. */
  unlink (marker);
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (m, "prog"));
  g_signal_emit_by_name (gbk_window_view (w), "activate");
  g_assert_true (wait_until (file_exists, marker));

  /* Close the window with a menu still open: no callbacks into a dead window. */
  menu = open_context_menu (w);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (menu)));
  gtk_widget_destroy (GTK_WIDGET (w));
  while (gtk_events_pending ())
    gtk_main_iteration ();
  rm_rf (root);
}

static void
test_open_folder_elsewhere (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "x/", "y/", "f.txt", nullptr });
  g_autofree char *x = g_build_filename (root, "x", nullptr);
  g_autofree char *y = g_build_filename (root, "y", nullptr);
  GbkWindow *w = window_at (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  GbkDirModel *m = model_of (w);

  /* A folder's menu offers a new tab or window; a file's doesn't. */
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (m, "x"));
  GtkMenu *menu = open_context_menu (w);
  g_autofree char *labels = menu_labels (menu);
  g_assert_true (g_str_has_prefix (labels, "Open|Open in New Tab|Open in New Window|"));
  choose (menu, "Open in New Tab");
  g_assert_cmpint (gbk_window_n_tabs (w), ==, 2);
  wait_at (w, x);                                  /* the new tab is shown */
  g_assert_true (wait_until (menus_gone, w));

  activate (w, "close-tab");
  wait_at (w, root);
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "f.txt"));
  menu = open_context_menu (w);
  g_autofree char *file_labels = menu_labels (menu);
  g_assert_null (strstr (file_labels, "New Tab"));
  g_assert_null (strstr (file_labels, "New Window"));
  gtk_menu_shell_deactivate (GTK_MENU_SHELL (menu));
  g_assert_true (wait_until (menus_gone, w));

  /* Two folders in new windows: one window each. */
  GbkHandle both[]
      = { gbk_dir_model_find (model_of (w), "x"), gbk_dir_model_find (model_of (w), "y") };
  gbk_view_set_selection (gbk_window_view (w), both, 2);
  guint windows = g_list_length (gtk_application_get_windows (app));
  choose (open_context_menu (w), "Open in New Window");
  g_assert_cmpuint (g_list_length (gtk_application_get_windows (app)), ==, windows + 2);
  g_assert_true (wait_until (menus_gone, w));
  /* Newest first in the application's list. */
  GList *wins = gtk_application_get_windows (app);
  GbkWindow *wy = GBK_WINDOW (wins->data), *wx = GBK_WINDOW (wins->next->data);
  wait_at (wx, x);
  wait_at (wy, y);
  gtk_widget_destroy (GTK_WIDGET (wx));
  gtk_widget_destroy (GTK_WIDGET (wy));

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

/* Let GTK finish (re)allocating: scrollbars appearing take more than one pass. */
static void
settle (void)
{
  for (int i = 0; i < 30; i++)
    {
      while (gtk_events_pending ())
        gtk_main_iteration ();
      g_usleep (2000);
    }
}

static void
test_scroll_extent (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  GRand *r = g_rand_new_with_seed (7);
  for (int i = 0; i < 600; i++)
    {
      GString *n = g_string_new ("");
      g_string_append_printf (n, "%03d-", i);
      for (int k = g_rand_int_range (r, 3, 40); k > 0; k--)
        g_string_append_c (n, (char) ('a' + g_rand_int_range (r, 0, 26)));
      g_autofree char *p = g_build_filename (root, n->str, nullptr);
      g_assert_true (g_file_set_contents (p, "x", 1, nullptr));
      g_string_free (n, true);
    }
  g_rand_free (r);

  GbkWindow *w = window_at (app, ctx, root);
  gtk_window_resize (GTK_WINDOW (w), 640, 300);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  settle ();
  GbkView *v = gbk_window_view (w);
  GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW (gtk_widget_get_parent (GTK_WIDGET (v)));
  GtkAdjustment *adj = gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (v));

  /* The scrollbar has its own space: it doesn't cover the bottom row. */
  GtkAllocation va, sa, ha;
  gtk_widget_get_allocation (GTK_WIDGET (v), &va);
  gtk_widget_get_allocation (GTK_WIDGET (sw), &sa);
  gtk_widget_get_allocation (gtk_scrolled_window_get_hscrollbar (sw), &ha);
  g_assert_true (gtk_widget_get_visible (gtk_scrolled_window_get_hscrollbar (sw)));
  g_assert_cmpint (va.height + ha.height, <=, sa.height);

  /* Dragged to the far end, it stays at the very end (GTK's passing layouts without the
   * scrollbar used to clamp it short), and still after the window changes size. */
  gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj));
  settle ();
  g_assert_cmpfloat (gtk_adjustment_get_value (adj), ==,
                     gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));
  gtk_window_resize (GTK_WINDOW (w), 600, 360);
  settle ();
  g_assert_cmpfloat (gtk_adjustment_get_value (adj), ==,
                     gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj));

  /* In the middle, the same files stay at the left edge when the height changes. */
  gtk_adjustment_set_value (adj, gtk_adjustment_get_upper (adj) / 2);
  settle ();
  GbkHandle left = gbk_view_item_at (v, 1, 1);
  g_assert_cmpuint (left, !=, GBK_HANDLE_NONE);
  gtk_window_resize (GTK_WINDOW (w), 600, 260);
  settle ();
  g_assert_cmpuint (gbk_view_rows_per_column (v), >, 0);
  size_t rows = gbk_view_rows_per_column (v);
  size_t want = gbk_dir_model_visible_index (model_of (w), left) / rows * rows;
  g_assert_cmpuint (gbk_view_item_at (v, 1, 1), ==,
                    gbk_dir_model_visible_handle (model_of (w), want));

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

static GtkMenu *
right_click_status_bar (GbkWindow *w)
{
  GtkWidget *bar = gbk_window_status_bar (w);
  GdkEvent *ev = gdk_event_new (GDK_BUTTON_PRESS);
  ev->button.window = g_object_ref (gtk_widget_get_window (bar));
  ev->button.button = GDK_BUTTON_SECONDARY;
  ev->button.x = ev->button.y = 3;
  ev->button.time = GDK_CURRENT_TIME;
  gdk_event_set_device (
      ev, gdk_seat_get_pointer (gdk_display_get_default_seat (gtk_widget_get_display (bar))));
  gtk_main_do_event (ev);
  gdk_event_free (ev);
  GList *menus = gtk_menu_get_for_attach_widget (bar);
  if (g_list_length (menus) != 1)
    gbk_fatal ("expected one status bar menu, got %u", g_list_length (menus));
  return menus->data;
}

static void
test_status_bar_menu (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "sub/", "f.txt", nullptr });
  g_autofree char *sub = g_build_filename (root, "sub", nullptr);
  GbkWindow *w = window_at (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);

  /* The same menu as empty space, for the active tab's folder, even with a selection. */
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "f.txt"));
  GtkMenu *menu = right_click_status_bar (w);
  g_autofree char *labels = menu_labels (menu);
  g_assert_cmpstr (
      labels, ==,
      "Create Folder…|Create File…|-|Paste|Copy Full Path|-|Open Terminal Here|-|"
      "Sort by Name|Sort by Date Modified|Sort by Size|-|Show Hidden Files|-|Properties…");
  choose (menu, "Sort by Size");
  g_assert_cmpint (gbk_dir_model_sort (model_of (w)), ==, GBK_SORT_SIZE);
  GtkWidget *bar = gbk_window_status_bar (w);
  g_assert_true (wait_until (bar_menus_gone, bar));

  /* In a second tab, it's that tab's folder. */
  new_tab_at (w, sub);
  wait_at (w, sub);
  choose (right_click_status_bar (w), "Copy Full Path");
  g_autofree char *copied = clipboard_text (w);
  g_assert_cmpstr (copied, ==, sub);
  g_assert_true (wait_until (bar_menus_gone, bar));

  /* The menu belongs to its tab: closing the tab takes the menu with it (the status bar
   * stays, so being attached there isn't enough). */
  right_click_status_bar (w);
  activate (w, "close-tab");
  g_assert_true (wait_until (bar_menus_gone, bar));

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

/* ---- create, executable, properties ---- */

/* The visible toplevel of `type` (with `title`, if given), or nullptr. */
static GtkWindow *
find_toplevel (GType type, const char *title)
{
  g_autoptr (GList) all = gtk_window_list_toplevels ();
  for (GList *l = all; l != nullptr; l = l->next)
    if (G_TYPE_CHECK_INSTANCE_TYPE (l->data, type) && gtk_widget_get_visible (l->data)
        && (title == nullptr || g_strcmp0 (gtk_window_get_title (l->data), title) == 0))
      return l->data;
  return nullptr;
}

static bool
dialog_open (void *title_ptr)
{
  return find_toplevel (GTK_TYPE_DIALOG, *(const char **) title_ptr) != nullptr;
}

/* The entry of the name dialog titled `title`. */
static GtkEntry *
name_entry (const char *title)
{
  g_assert_true (wait_until (dialog_open, &title));
  GtkWindow *d = find_toplevel (GTK_TYPE_DIALOG, title);
  return GTK_ENTRY (g_object_get_data (G_OBJECT (d), "gbk-entry"));
}

static GtkMenuItem *
menu_item_named (GtkMenu *menu, const char *label)
{
  g_autoptr (GList) children = gtk_container_get_children (GTK_CONTAINER (menu));
  for (GList *l = children; l != nullptr; l = l->next)
    if (GTK_IS_MENU_ITEM (l->data) && !GTK_IS_SEPARATOR_MENU_ITEM (l->data)
        && g_strcmp0 (gtk_menu_item_get_label (l->data), label) == 0)
      return l->data;
  gbk_fatal ("no menu item \"%s\"", label);
}

typedef struct
{
  GbkWindow *w;
  const char *name;
} NameCtx;

static bool
cursor_is (void *data)
{
  NameCtx *c = data;
  const char *name = cursor_name (c->w);
  return name != nullptr && g_str_equal (name, c->name);
}

static bool
is_executable (void *path)
{
  return g_file_test (path, G_FILE_TEST_IS_EXECUTABLE);
}

static bool
not_executable (void *path)
{
  return !g_file_test (path, G_FILE_TEST_IS_EXECUTABLE);
}

static bool
props_loaded (void *p)
{
  return gbk_props_loaded (p);
}

static bool
error_shown (void *data G_GNUC_UNUSED)
{
  return find_toplevel (GTK_TYPE_MESSAGE_DIALOG, nullptr) != nullptr;
}

static void
test_create_executable_properties (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "New Folder/", nullptr });
  g_autofree char *notes = g_build_filename (root, "notes.txt", nullptr);
  GbkWindow *w = window_at (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);

  /* Create Folder: "New Folder" is taken, so it suggests "New Folder (1)"; the new folder
   * ends up selected. */
  choose (open_context_menu (w), "Create Folder…");
  GtkEntry *entry = name_entry ("Create Folder");
  g_assert_cmpstr (gtk_entry_get_text (entry), ==, "New Folder (1)");
  gtk_dialog_response (GTK_DIALOG (find_toplevel (GTK_TYPE_DIALOG, "Create Folder")),
                       GTK_RESPONSE_ACCEPT);
  NameCtx c1 = { w, "New Folder (1)" };
  g_assert_true (wait_until (cursor_is, &c1));
  g_autofree char *nf1 = g_build_filename (root, "New Folder (1)", nullptr);
  g_assert_true (g_file_test (nf1, G_FILE_TEST_IS_DIR));

  /* Create File with a typed name. */
  gbk_view_unselect_all (gbk_window_view (w));
  choose (open_context_menu (w), "Create File…");
  entry = name_entry ("Create File");
  g_assert_cmpstr (gtk_entry_get_text (entry), ==, "New File");
  gtk_entry_set_text (entry, "notes.txt");
  gtk_dialog_response (GTK_DIALOG (find_toplevel (GTK_TYPE_DIALOG, "Create File")),
                       GTK_RESPONSE_ACCEPT);
  NameCtx c2 = { w, "notes.txt" };
  g_assert_true (wait_until (cursor_is, &c2));
  g_assert_true (g_file_test (notes, G_FILE_TEST_IS_REGULAR));

  /* A name that exists: an error, and nothing is replaced. */
  g_assert_true (g_file_set_contents (notes, "keep", -1, nullptr));
  gbk_view_unselect_all (gbk_window_view (w));
  choose (open_context_menu (w), "Create File…");
  entry = name_entry ("Create File");
  gtk_entry_set_text (entry, "notes.txt");
  gtk_dialog_response (GTK_DIALOG (find_toplevel (GTK_TYPE_DIALOG, "Create File")),
                       GTK_RESPONSE_ACCEPT);
  g_assert_true (wait_until (error_shown, nullptr));
  gtk_widget_destroy (GTK_WIDGET (find_toplevel (GTK_TYPE_MESSAGE_DIALOG, nullptr)));
  g_autofree char *kept = nullptr;
  g_assert_true (g_file_get_contents (notes, &kept, nullptr, nullptr));
  g_assert_cmpstr (kept, ==, "keep");

  /* Executable: a check item showing the state; choosing it flips it. */
  GbkDirModel *m = model_of (w);
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (m, "notes.txt"));
  GtkMenu *menu = open_context_menu (w);
  g_assert_false (
      gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (menu_item_named (menu, "Executable"))));
  choose (menu, "Executable");
  g_assert_true (wait_until (is_executable, notes));
  g_assert_true (wait_until (menus_gone, w));
  /* The model hears about the change (inotify) before the menu shows it as on. */
  for (int i = 0; i < 100; i++)
    {
      const GbkEntry *e = gbk_dir_model_get (m, gbk_dir_model_find (m, "notes.txt"));
      if (e != nullptr && (e->flags & GBK_ENTRY_EXECUTABLE))
        break;
      while (gtk_events_pending ())
        gtk_main_iteration ();
      g_usleep (10000);
    }
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (m, "notes.txt"));
  menu = open_context_menu (w);
  g_assert_true (
      gtk_check_menu_item_get_active (GTK_CHECK_MENU_ITEM (menu_item_named (menu, "Executable"))));
  choose (menu, "Executable");
  g_assert_true (wait_until (not_executable, notes));
  g_assert_true (wait_until (menus_gone, w));

  /* Properties: details, then permissions changed with Apply. */
  g_assert_cmpint (chmod (notes, 0640), ==, 0);
  choose (open_context_menu (w), "Properties…");
  g_assert_true (wait_until (menus_gone, w));
  GbkProps *p = GBK_PROPS (find_toplevel (GBK_TYPE_PROPS, "notes.txt Properties"));
  g_assert_nonnull (p);
  g_assert_true (wait_until (props_loaded, p));
  g_assert_cmpstr (gbk_props_value (p, "Name"), ==, "notes.txt");
  g_assert_cmpstr (gbk_props_value (p, "Location"), ==, root);
  g_assert_cmpstr (gbk_props_value (p, "Size"), ==, "4 bytes (4 bytes)");
  g_assert_nonnull (gbk_props_value (p, "Modified"));
  g_assert_nonnull (gbk_props_value (p, "Owner"));
  g_assert_cmpstr (gbk_props_octal (p), ==, "640");
  g_assert_false (gtk_widget_get_sensitive (GTK_WIDGET (gbk_props_apply_button (p))));
  gtk_toggle_button_set_active (gbk_props_permission (p, 0, 2), true);     /* owner execute */
  gtk_toggle_button_set_active (gbk_props_permission (p, 2, 0), true);     /* others read */
  g_assert_cmpstr (gbk_props_octal (p), ==, "744");
  g_assert_true (gtk_widget_get_sensitive (GTK_WIDGET (gbk_props_apply_button (p))));
  g_assert_true (not_executable (notes));               /* nothing changes before Apply */
  gtk_button_clicked (gbk_props_apply_button (p));
  g_assert_true (wait_until (is_executable, notes));
  g_assert_true (wait_until (props_loaded, p));         /* re-read after applying */
  struct stat st;
  g_assert_cmpint (stat (notes, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 0777, ==, 0744);
  g_assert_false (gtk_widget_get_sensitive (GTK_WIDGET (gbk_props_apply_button (p))));
  const char *art = g_getenv ("GBK_ARTIFACTS");
  if (art != nullptr)
    {
      for (int i = 0; i < 10; i++)          /* past the theme's check-box animations */
        settle ();
      gtk_container_check_resize (GTK_CONTAINER (p));
      int pw = gtk_widget_get_allocated_width (GTK_WIDGET (p));
      int ph = gtk_widget_get_allocated_height (GTK_WIDGET (p));
      cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, pw, ph);
      cairo_t *cr = cairo_create (surf);
      gtk_widget_draw (GTK_WIDGET (p), cr);
      cairo_destroy (cr);
      g_autofree char *png = g_build_filename (art, "props.png", nullptr);
      cairo_surface_write_to_png (surf, png);
      cairo_surface_destroy (surf);
    }
  /* Escape closes it. */
  GtkWidget *alive = GTK_WIDGET (p);
  g_object_add_weak_pointer (G_OBJECT (p), (void **) &alive);
  GdkEvent *ev = gdk_event_new (GDK_KEY_PRESS);
  ev->key.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (p)));
  ev->key.keyval = GDK_KEY_Escape;
  gdk_event_set_device (ev, gdk_seat_get_keyboard (gdk_display_get_default_seat (
                                gtk_widget_get_display (GTK_WIDGET (p)))));
  gtk_main_do_event (ev);
  gdk_event_free (ev);
  g_assert_null (alive);

  /* Alt+Enter with nothing selected: the folder itself (no size). */
  gbk_view_unselect_all (gbk_window_view (w));
  activate (w, "properties");
  g_autofree char *base = g_path_get_basename (root);
  g_autofree char *title = g_strdup_printf ("%s Properties", base);
  p = GBK_PROPS (find_toplevel (GBK_TYPE_PROPS, title));
  g_assert_nonnull (p);
  g_assert_true (wait_until (props_loaded, p));
  g_assert_cmpstr (gbk_props_value (p, "Type"), ==, "Folder");
  g_assert_null (gbk_props_value (p, "Size"));

  /* It closes with its window. */
  alive = GTK_WIDGET (p);
  g_object_add_weak_pointer (G_OBJECT (p), (void **) &alive);
  gtk_widget_destroy (GTK_WIDGET (w));
  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_assert_null (alive);
  rm_rf (root);
}

/* ---- drag and drop ---- */

/* A button press on `widget`'s window, standing in for the event GTK has during a drop. */
static GdkEvent *
fake_trigger (GtkWidget *widget, double x, double y)
{
  GdkEvent *ev = gdk_event_new (GDK_BUTTON_PRESS);
  ev->button.window = g_object_ref (gtk_widget_get_window (widget));
  ev->button.button = GDK_BUTTON_PRIMARY;
  ev->button.x = x;
  ev->button.y = y;
  ev->button.time = GDK_CURRENT_TIME;
  gdk_event_set_device (
      ev, gdk_seat_get_pointer (gdk_display_get_default_seat (gtk_widget_get_display (widget))));
  return ev;
}

static GtkMenu *
only_menu (GtkWidget *attach)
{
  GList *menus = gtk_menu_get_for_attach_widget (attach);
  if (g_list_length (menus) != 1)
    gbk_fatal ("expected one menu, got %u", g_list_length (menus));
  return menus->data;
}

static char *
uris_of (const char *dir, const char *const *names)
{
  g_autoptr (GPtrArray) paths = g_ptr_array_new_with_free_func (g_free);
  for (size_t i = 0; names[i] != nullptr; i++)
    g_ptr_array_add (paths, g_build_filename (dir, names[i], nullptr));
  return gbk_clip_format_uri_list (paths);
}

static bool
both_exist (void *dir)
{
  g_autofree char *a = g_build_filename (dir, "a.txt", nullptr);
  g_autofree char *b = g_build_filename (dir, "b.txt", nullptr);
  return g_file_test (a, G_FILE_TEST_EXISTS) && g_file_test (b, G_FILE_TEST_EXISTS);
}

static bool
neither_exists (void *dir)
{
  g_autofree char *a = g_build_filename (dir, "a.txt", nullptr);
  g_autofree char *b = g_build_filename (dir, "b.txt", nullptr);
  return !g_file_test (a, G_FILE_TEST_EXISTS) && !g_file_test (b, G_FILE_TEST_EXISTS);
}

/* No drag icon or menu left on screen. (GTK keeps empty 1x1 popups off-screen at
 * -99,-99 for drag-and-drop messaging, and reuses them: those don't count.) */
static bool
no_popups_mapped (void *data G_GNUC_UNUSED)
{
  g_autoptr (GList) all = gtk_window_list_toplevels ();
  for (GList *l = all; l != nullptr; l = l->next)
    if (gtk_window_get_window_type (l->data) == GTK_WINDOW_POPUP && gtk_widget_get_mapped (l->data)
        && gtk_bin_get_child (l->data) != nullptr)
      return false;
  return true;
}

static void
test_drag_and_drop (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "src/", "src/a.txt", "src/b.txt", "dest/sub/", nullptr });
  g_autofree char *src = g_build_filename (root, "src", nullptr);
  g_autofree char *dest = g_build_filename (root, "dest", nullptr);
  g_autofree char *sub = g_build_filename (dest, "sub", nullptr);
  const char *const ab[] = { "a.txt", "b.txt", nullptr };
  g_autofree char *uris = uris_of (src, ab);

  GbkWindow *w = window_at (app, ctx, dest);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, dest);
  GbkView *v = gbk_window_view (w);
  GtkWidget *vw = GTK_WIDGET (v);
  double far_x = gtk_widget_get_allocated_width (vw) - 5;   /* empty space */

  /* Onto the folder "sub" (first item): asked, and Copy Here copies into it. */
  g_autoptr (GdkEvent) t1 = fake_trigger (vw, 5, 2);
  g_assert_true (gbk_view_drop_for_test (v, uris, 5, 2, t1));
  GtkMenu *menu = only_menu (vw);
  g_autofree char *labels = menu_labels (menu);
  g_assert_cmpstr (labels, ==, "Move Here|Copy Here|-|Cancel");
  g_assert_false (both_exist (sub));                 /* nothing happens before choosing */
  choose (menu, "Copy Here");
  g_assert_true (wait_until (both_exist, sub));
  g_assert_true (both_exist (src));                   /* a copy: the originals stay */
  g_assert_true (wait_until (menus_gone, w));

  /* Cancel does nothing. */
  g_autoptr (GdkEvent) t2 = fake_trigger (vw, far_x, 2);
  g_assert_true (gbk_view_drop_for_test (v, uris, far_x, 2, t2));
  choose (only_menu (vw), "Cancel");
  g_assert_true (wait_until (menus_gone, w));
  g_assert_true (neither_exists (dest));

  /* Onto empty space: the folder itself. Move Here moves, and selects what arrived. */
  g_autoptr (GdkEvent) t3 = fake_trigger (vw, far_x, 2);
  g_assert_true (gbk_view_drop_for_test (v, uris, far_x, 2, t3));
  choose (only_menu (vw), "Move Here");
  g_assert_true (wait_until (both_exist, dest));
  g_assert_true (wait_until (neither_exists, src));
  NameCtx moved = { w, "a.txt" };
  g_assert_true (wait_until (cursor_is, &moved));
  g_assert_cmpuint (gbk_view_n_selected (v), ==, 2);
  g_assert_true (wait_until (menus_gone, w));

  /* Files dropped back into the folder they're in: no menu, nothing to do. */
  g_autofree char *here = uris_of (dest, ab);
  g_autoptr (GdkEvent) t4 = fake_trigger (vw, far_x, 2);
  g_assert_true (gbk_view_drop_for_test (v, here, far_x, 2, t4));
  g_assert_null (gtk_menu_get_for_attach_widget (vw));
  g_assert_cmpstr (gbk_window_status (w), ==, "Already in this folder");

  /* Onto a path bar button (the parent folder): asked the same way. */
  GtkWidget *bar = GTK_WIDGET (gbk_window_pathbar (w));
  g_autoptr (GdkEvent) t5 = fake_trigger (bar, 2, 2);
  gbk_pathbar_drop_for_test (gbk_window_pathbar (w), root, here, t5);
  menu = only_menu (bar);
  g_autofree char *bar_labels = menu_labels (menu);
  g_assert_cmpstr (bar_labels, ==, "Move Here|Copy Here|-|Cancel");
  choose (menu, "Copy Here");
  g_assert_true (wait_until (both_exist, root));
  g_assert_true (both_exist (dest));
  g_assert_true (wait_until (bar_menus_gone, bar));

  /* Dragging out carries the selection as a uri-list. */
  g_autofree char *dragged = gbk_view_drag_uris (v);
  g_assert_cmpstr (dragged, ==, here);

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

/* Real input, through the X server (XTest): during a drag GTK holds a pointer grab and
 * only listens to the server, so events injected into GTK itself never reach it. */
static Display *
xdisplay (GtkWidget *w)
{
  return gdk_x11_display_get_xdisplay (gtk_widget_get_display (w));
}

static void
x_move (GtkWidget *view, double x, double y)
{
  int rx, ry;
  gdk_window_get_root_coords (gtk_widget_get_window (view), (int) x, (int) y, &rx, &ry);
  XTestFakeMotionEvent (xdisplay (view), -1, rx, ry, CurrentTime);
  XFlush (xdisplay (view));
  settle ();
}

static void
x_button (GtkWidget *view, bool press)
{
  XTestFakeButtonEvent (xdisplay (view), 1, press, CurrentTime);
  XFlush (xdisplay (view));
  settle ();
}

/* Where item `name` is drawn, in view coordinates. */
static bool
item_xy (GbkWindow *w, const char *name, double *x, double *y)
{
  GbkView *v = gbk_window_view (w);
  GbkHandle h = gbk_dir_model_find (model_of (w), name);
  int width = gtk_widget_get_allocated_width (GTK_WIDGET (v));
  int height = gtk_widget_get_allocated_height (GTK_WIDGET (v));
  for (int py = 2; py < height; py += 4)
    for (int px = 2; px < width; px += 4)
      if (gbk_view_item_at (v, px, py) == h)
        {
          *x = px + 2;
          *y = py + 2;
          return true;
        }
  return false;
}

static bool
view_has_menu (void *v)
{
  return gtk_menu_get_for_attach_widget (v) != nullptr;
}

static bool
moved_into_sub (void *root)
{
  g_autofree char *sub = g_build_filename (root, "sub", nullptr);
  return both_exist (sub) && neither_exists (root);
}

/* The whole thing through GTK's real drag-and-drop, driven by real X input (pointer grab,
 * X messages, selection transfer): press on one of two selected files, drag onto a folder
 * in the same view, get asked, move both. */
static void
test_real_drag_and_drop (void)
{
  g_assert_true (wait_until (no_popups_mapped, nullptr));
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "sub/", "a.txt", "b.txt", nullptr });
  GbkWindow *w = window_at (app, ctx, root);
  gtk_window_move (GTK_WINDOW (w), 0, 0);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  settle ();
  GtkWidget *vw = GTK_WIDGET (gbk_window_view (w));
  double ax, ay, sx, sy;
  g_assert_true (item_xy (w, "a.txt", &ax, &ay));
  g_assert_true (item_xy (w, "sub", &sx, &sy));
  GbkHandle both[]
      = { gbk_dir_model_find (model_of (w), "a.txt"), gbk_dir_model_find (model_of (w), "b.txt") };
  gbk_view_set_selection (gbk_window_view (w), both, 2);

  x_move (vw, ax, ay);
  x_button (vw, true);
  for (int i = 1; i <= 10; i++)
    x_move (vw, ax + (sx - ax) * i / 10, ay + (sy - ay) * i / 10);
  const char *art = g_getenv ("GBK_ARTIFACTS");
  if (art != nullptr)                 /* the folder being hovered is outlined */
    {
      int ww = gtk_widget_get_allocated_width (vw), wh = gtk_widget_get_allocated_height (vw);
      cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, ww, wh);
      cairo_t *cr = cairo_create (surf);
      gtk_widget_draw (vw, cr);
      cairo_destroy (cr);
      g_autofree char *png = g_build_filename (art, "drop-hover.png", nullptr);
      cairo_surface_write_to_png (surf, png);
      cairo_surface_destroy (surf);
    }
  x_button (vw, false);
  g_assert_true (wait_until (view_has_menu, vw));
  g_assert_cmpuint (gbk_view_n_selected (gbk_window_view (w)), ==, 2);   /* dragged both */
  /* Nothing has moved: we're asked first. */
  g_autofree char *a = g_build_filename (root, "a.txt", nullptr);
  g_assert_true (g_file_test (a, G_FILE_TEST_EXISTS));
  GtkMenu *menu = gtk_menu_get_for_attach_widget (vw)->data;
  g_autofree char *labels = menu_labels (menu);
  g_assert_cmpstr (labels, ==, "Move Here|Copy Here|-|Cancel");
  choose (menu, "Move Here");
  g_assert_true (wait_until (moved_into_sub, root));
  g_assert_true (wait_until (no_popups_mapped, nullptr));      /* drag icon, menu: gone */

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

/* ---- file operations through the window ---- */

static bool
no_jobs (void *data G_GNUC_UNUSED)
{
  return gbk_job_live () == 0;
}

static bool
path_exists (void *data)
{
  return g_file_test (data, G_FILE_TEST_EXISTS);
}

static bool
path_gone (void *data)
{
  return !g_file_test (data, G_FILE_TEST_EXISTS);
}

typedef struct
{
  GbkWindow *w;
  const char *name;
} SelCtx;

static bool
only_selected (void *data)
{
  SelCtx *c = data;
  GbkView *v = gbk_window_view (c->w);
  GbkHandle h = gbk_dir_model_find (model_of (c->w), c->name);
  return h != GBK_HANDLE_NONE && gbk_view_n_selected (v) == 1 && gbk_view_is_selected (v, h);
}

/* The topmost visible dialog other than our windows (async dialogs: find, then respond). */
static GtkDialog *
find_dialog (void)
{
  for (int tries = 0; tries < 5000; tries++)
    {
      g_autoptr (GList) tops = gtk_window_list_toplevels ();
      for (GList *l = tops; l != nullptr; l = l->next)
        if (GTK_IS_DIALOG (l->data) && gtk_widget_get_visible (l->data))
          return l->data;
      while (gtk_events_pending ())
        gtk_main_iteration ();
      g_usleep (1000);
    }
  gbk_fatal ("no dialog appeared");
}

static void
select_name (GbkWindow *w, const char *name)
{
  GbkHandle h = gbk_dir_model_find (model_of (w), name);
  if (h == GBK_HANDLE_NONE)
    gbk_fatal ("no %s", name);
  gbk_view_select_only (gbk_window_view (w), h);
}

static void
test_file_operations (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root,
          (const char *const[]) { "src/", "dst/", "src/a.txt", "src/b.txt", "dst/b.txt", nullptr });
  g_autofree char *src = g_build_filename (root, "src", nullptr);
  g_autofree char *dst = g_build_filename (root, "dst", nullptr);
  GbkWindow *w = window_at (app, ctx, src);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, src);

  /* Copy a.txt, go to dst, paste: it's copied and selected there. */
  select_name (w, "a.txt");
  activate (w, "copy");
  open_at (w, dst);
  wait_at (w, dst);
  activate (w, "paste");
  g_autofree char *dst_a = g_build_filename (dst, "a.txt", nullptr);
  g_assert_true (wait_until (path_exists, dst_a));
  SelCtx sel_a = { w, "a.txt" };
  g_assert_true (wait_until (only_selected, &sel_a));
  g_assert_true (wait_until (no_jobs, nullptr));

  /* Cut b.txt from src, paste into dst where b.txt exists: answer Keep Both. */
  activate (w, "back");
  wait_at (w, src);
  select_name (w, "b.txt");
  activate (w, "cut");
  open_at (w, dst);
  wait_at (w, dst);
  activate (w, "paste");
  GtkDialog *conflict = find_dialog ();
  gtk_dialog_response (conflict, 3 /* RESP_KEEP_BOTH */);
  g_autofree char *kept = g_build_filename (dst, "b (1).txt", nullptr);
  g_autofree char *src_b = g_build_filename (src, "b.txt", nullptr);
  g_assert_true (wait_until (path_exists, kept));
  g_assert_true (wait_until (path_gone, src_b));      /* it was a cut: moved */
  g_assert_true (wait_until (no_jobs, nullptr));

  /* F2 rename via the dialog. */
  select_name (w, "a.txt");
  activate (w, "rename");
  GtkDialog *rename = find_dialog ();
  GtkEntry *entry = g_object_get_data (G_OBJECT (rename), "gbk-entry");
  gtk_entry_set_text (entry, "renamed.txt");
  gtk_dialog_response (rename, GTK_RESPONSE_ACCEPT);
  g_autofree char *renamed = g_build_filename (dst, "renamed.txt", nullptr);
  g_assert_true (wait_until (path_exists, renamed));
  SelCtx sel_r = { w, "renamed.txt" };
  g_assert_true (wait_until (only_selected, &sel_r));

  /* Delete key: to the trash (no confirmation). */
  g_assert_true (wait_until (no_jobs, nullptr));
  activate (w, "trash");
  g_assert_true (wait_until (path_gone, renamed));
  g_assert_true (wait_until (no_jobs, nullptr));

  /* Shift+Delete: confirmation; Cancel keeps the file, Delete removes it. */
  select_name (w, "b.txt");
  activate (w, "delete");
  gtk_dialog_response (find_dialog (), GTK_RESPONSE_CANCEL);
  g_autofree char *dst_b = g_build_filename (dst, "b.txt", nullptr);
  g_assert_true (g_file_test (dst_b, G_FILE_TEST_EXISTS));
  activate (w, "delete");
  gtk_dialog_response (find_dialog (), GTK_RESPONSE_ACCEPT);
  g_assert_true (wait_until (path_gone, dst_b));
  g_assert_true (wait_until (no_jobs, nullptr));

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

static void
test_job_outlives_window (void)
{
  /* Close the window while its paste is still asking a question: the job carries on with
   * its own UI, and cancelling it there cleans up. Nothing may call into the dead window. */
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "f.txt", nullptr });
  GbkWindow *w = window_at (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  select_name (w, "f.txt");
  activate (w, "copy");
  /* Paste where it already is: a copy... but pasting twice makes a conflict-free copy, so
   * create the conflict explicitly by pasting into a folder holding f.txt. */
  mkdirs (root, (const char *const[]) { "sub/", "sub/f.txt", nullptr });
  g_autofree char *sub = g_build_filename (root, "sub", nullptr);
  open_at (w, sub);
  wait_at (w, sub);
  activate (w, "paste");
  GtkDialog *conflict = find_dialog ();
  gtk_widget_destroy (GTK_WIDGET (w));
  while (gtk_events_pending ())
    gtk_main_iteration ();
  gtk_dialog_response (conflict, GTK_RESPONSE_CANCEL);
  g_assert_true (wait_until (no_jobs, nullptr));
  rm_rf (root);
}

/* ---- path bar, tabs, key routing ---- */

static void
send_key_to_window (GbkWindow *w, guint keyval, GdkModifierType state)
{
  GdkEvent *ev = gdk_event_new (GDK_KEY_PRESS);
  ev->key.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (w)));
  ev->key.keyval = keyval;
  ev->key.state = state;
  ev->key.time = GDK_CURRENT_TIME;
  /* Real events carry a hardware keycode, and GTK's key bindings (e.g. GtkEntry's Delete)
   * are looked up by it: without one the event looks unhandled. */
  GdkKeymap *keymap = gdk_keymap_get_for_display (gtk_widget_get_display (GTK_WIDGET (w)));
  g_autofree GdkKeymapKey *keys = nullptr;
  int n_keys = 0;
  if (gdk_keymap_get_entries_for_keyval (keymap, keyval, &keys, &n_keys) && n_keys > 0)
    {
      /* A keyval can sit on several keys (Delete is also KP_Delete at another level):
       * pick the one that produces it unshifted, as a plain keypress would. */
      int pick = 0;
      for (int i = 0; i < n_keys; i++)
        if (keys[i].level == 0 && keys[i].group == 0)
          {
            pick = i;
            break;
          }
      ev->key.hardware_keycode = (guint16) keys[pick].keycode;
      ev->key.group = (guint8) keys[pick].group;
    }
  gdk_event_set_device (ev, gdk_seat_get_keyboard (gdk_display_get_default_seat (
                                gtk_widget_get_display (GTK_WIDGET (w)))));
  gtk_main_do_event (ev);
  gdk_event_free (ev);
  while (gtk_events_pending ())
    gtk_main_iteration ();
}

#define assert_bar(w, expected) \
  do \
    { \
      g_autofree char *d_ = gbk_pathbar_describe (gbk_window_pathbar (w)); \
      g_assert_cmpstr (d_, ==, expected); \
    } \
  while (0)

static void
test_pathbar (void)
{
  const char *home = g_get_home_dir ();
  g_autofree char *home_label = g_path_get_basename (home);
  g_autofree char *b = g_build_filename (home, "a", "b", nullptr);
  g_autofree char *a = g_build_filename (home, "a", nullptr);
  g_mkdir_with_parents (b, 0755);

  GbkWindow *w = window_at (app, ctx, b);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, b);
  g_autofree char *e1 = g_strdup_printf ("%s|a|[b]", home_label);
  assert_bar (w, e1);

  /* Up via a button: the deeper button stays, for going back down. */
  gbk_pathbar_click (gbk_window_pathbar (w), a);
  wait_at (w, a);
  g_autofree char *e2 = g_strdup_printf ("%s|[a]|b", home_label);
  assert_bar (w, e2);
  gbk_pathbar_click (gbk_window_pathbar (w), b);
  wait_at (w, b);
  assert_bar (w, e1);

  /* Right-click on a button: that folder's menu, without anything that would move or delete
   * a folder the tab is inside. */
  GtkWidget *bar_widget = GTK_WIDGET (gbk_window_pathbar (w));
  GtkMenu *menu = right_click_bar (w, a);
  g_autofree char *labels = menu_labels (menu);
  g_assert_cmpstr (labels, ==,
                   "Open|Open in New Tab|Open in New Window|Open Terminal Here|-|"
                   "Copy|Paste Into Folder|Copy Full Path|-|Properties…");
  choose (menu, "Copy Full Path");
  g_autofree char *copied = clipboard_text (w);
  g_assert_cmpstr (copied, ==, a);
  g_assert_true (wait_until (bar_menus_gone, bar_widget));

  choose (right_click_bar (w, a), "Open in New Tab");
  g_assert_cmpint (gbk_window_n_tabs (w), ==, 2);
  wait_at (w, a);
  activate (w, "close-tab");
  wait_at (w, b);

  choose (right_click_bar (w, a), "Open");
  wait_at (w, a);
  g_assert_cmpint (gbk_window_n_tabs (w), ==, 1);
  gbk_pathbar_click (gbk_window_pathbar (w), b);
  wait_at (w, b);

  /* Outside home: from the filesystem root. */
  open_at (w, "/usr");
  wait_at (w, "/usr");
  assert_bar (w, "File System|[usr]");

  /* Typed location, with ~ and relative to nothing in particular. */
  GbkPathbar *bar = gbk_window_pathbar (w);
  gbk_pathbar_edit (bar);
  g_assert_true (gbk_pathbar_editing (bar));
  gtk_entry_set_text (gbk_pathbar_entry (bar), "~/a");
  gtk_widget_activate (GTK_WIDGET (gbk_pathbar_entry (bar)));
  wait_at (w, a);
  g_assert_false (gbk_pathbar_editing (bar));

  /* Escape leaves the entry without navigating. */
  activate (w, "location");
  g_assert_true (gbk_pathbar_editing (bar));
  send_key_to_window (w, GDK_KEY_Escape, 0);
  g_assert_false (gbk_pathbar_editing (bar));
  g_assert_cmpstr (wpath (w), ==, a);

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (a);
}

static void
test_keys_go_to_focused_entry (void)
{
  /* With the location entry focused, Delete / Ctrl+V / F2 must edit text, never act on
   * files. (GTK runs window shortcuts first unless we route keys to the entry.) */
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "precious.txt", nullptr });
  g_autofree char *precious = g_build_filename (root, "precious.txt", nullptr);
  GbkWindow *w = window_at (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  select_name (w, "precious.txt");
  activate (w, "copy");                       /* something on the clipboard to paste */

  GbkPathbar *bar = gbk_window_pathbar (w);
  gbk_pathbar_edit (bar);                     /* entry focused, whole path selected */
  GtkEntry *entry = gbk_pathbar_entry (bar);
  g_assert_true (gtk_window_get_focus (GTK_WINDOW (w)) == GTK_WIDGET (entry));
  send_key_to_window (w, GDK_KEY_Delete, 0);
  g_assert_cmpstr (gtk_entry_get_text (entry), ==, "");          /* text deleted */
  send_key_to_window (w, GDK_KEY_v, GDK_CONTROL_MASK);
  send_key_to_window (w, GDK_KEY_F2, 0);
  send_key_to_window (w, GDK_KEY_Delete, GDK_SHIFT_MASK);
  for (int i = 0; i < 200; i++)               /* give any wrongly started job time to run */
    {
      while (gtk_events_pending ())
        gtk_main_iteration ();
      g_usleep (1000);
    }
  g_assert_true (g_file_test (precious, G_FILE_TEST_EXISTS));
  g_assert_cmpint (gbk_job_live (), ==, 0);
  g_autofree char *copy = g_build_filename (root, "precious (1).txt", nullptr);
  g_assert_false (g_file_test (copy, G_FILE_TEST_EXISTS));
  g_autoptr (GList) tops = gtk_window_list_toplevels ();
  for (GList *l = tops; l != nullptr; l = l->next)
    g_assert_false (GTK_IS_DIALOG (l->data) && gtk_widget_get_visible (l->data));

  /* Control: with the view focused, the same Delete key does trash the file. */
  send_key_to_window (w, GDK_KEY_Escape, 0);
  gtk_widget_grab_focus (GTK_WIDGET (gbk_window_view (w)));
  select_name (w, "precious.txt");
  send_key_to_window (w, GDK_KEY_Delete, 0);
  g_assert_true (wait_until (path_gone, precious));
  g_assert_true (wait_until (no_jobs, nullptr));

  gtk_widget_destroy (GTK_WIDGET (w));
  rm_rf (root);
}

static void
test_tabs (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "one/", "two/", "three/", ".hidden", nullptr });
  g_autofree char *one = g_build_filename (root, "one", nullptr);
  g_autofree char *two = g_build_filename (root, "two", nullptr);
  GbkWindow *w = window_at (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  g_assert_cmpint (gbk_window_n_tabs (w), ==, 1);

  /* Ctrl+T: a second tab on the same folder; navigating it leaves the first alone. */
  send_key_to_window (w, GDK_KEY_t, GDK_CONTROL_MASK);
  g_assert_cmpint (gbk_window_n_tabs (w), ==, 2);
  wait_at (w, root);
  open_at (w, one);
  wait_at (w, one);
  send_key_to_window (w, GDK_KEY_Page_Up, GDK_CONTROL_MASK);
  wait_at (w, root);                          /* first tab, untouched */
  g_autofree char *bar = gbk_pathbar_describe (gbk_window_pathbar (w));
  g_assert_null (strstr (bar, "one"));         /* path bar follows the active tab */
  send_key_to_window (w, GDK_KEY_Page_Down, GDK_CONTROL_MASK);
  wait_at (w, one);

  /* Layout: the tab strip sits above the path bar, which belongs to the tab's page.
   * Layout happens on GTK's next frame, not when the tab switches: let it run first. */
  settle ();
  GtkWidget *bar_widget = GTK_WIDGET (gbk_window_pathbar (w));
  g_assert_cmpint (gtk_widget_get_allocated_height (bar_widget), >, 1);
  GtkWidget *notebook = gtk_widget_get_ancestor (bar_widget, GTK_TYPE_NOTEBOOK);
  g_assert_nonnull (notebook);
  int bar_y, page_y;
  g_assert_true (
      gtk_widget_translate_coordinates (bar_widget, GTK_WIDGET (w), 0, 0, nullptr, &bar_y));
  GtkWidget *page = gtk_notebook_get_nth_page (GTK_NOTEBOOK (notebook), 0);
  GtkWidget *tab_label = gtk_notebook_get_tab_label (GTK_NOTEBOOK (notebook), page);
  g_assert_true (
      gtk_widget_translate_coordinates (tab_label, GTK_WIDGET (w), 0, 0, nullptr, &page_y));
  g_assert_cmpint (page_y, <, bar_y);

  /* Show hidden is window-wide: every tab. */
  send_key_to_window (w, GDK_KEY_Page_Up, GDK_CONTROL_MASK);
  wait_at (w, root);
  size_t before = gbk_dir_model_n_visible (model_of (w));
  activate (w, "show-hidden");
  g_assert_cmpuint (gbk_dir_model_n_visible (model_of (w)), ==, before + 1);
  activate (w, "show-hidden");

  /* Activating several folders opens each in a new tab. */
  GbkView *v = gbk_window_view (w);
  GbkHandle hs[2]
      = { gbk_dir_model_find (model_of (w), "two"), gbk_dir_model_find (model_of (w), "three") };
  gbk_view_set_selection (v, hs, 2);
  g_signal_emit_by_name (v, "activate");
  g_assert_cmpint (gbk_window_n_tabs (w), ==, 4);
  wait_at (w, root);                          /* active tab unchanged */
  gbk_view_set_selection (v, hs, 1);
  const char *art = g_getenv ("GBK_ARTIFACTS");
  if (art != nullptr)
    {
      gtk_window_resize (GTK_WINDOW (w), 640, 300);
      for (int i = 0; i < 50; i++)
        {
          while (gtk_events_pending ())
            gtk_main_iteration ();
          g_usleep (2000);
        }
      gtk_container_check_resize (GTK_CONTAINER (w));
      int ww = gtk_widget_get_allocated_width (GTK_WIDGET (w));
      int wh = gtk_widget_get_allocated_height (GTK_WIDGET (w));
      cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, ww, wh);
      cairo_t *cr = cairo_create (surf);
      gtk_widget_draw (GTK_WIDGET (w), cr);
      cairo_destroy (cr);
      g_autofree char *png = g_build_filename (art, "window.png", nullptr);
      cairo_surface_write_to_png (surf, png);
      cairo_surface_destroy (surf);
    }

  /* Ctrl+W closes tabs; the last one closes the window. */
  send_key_to_window (w, GDK_KEY_w, GDK_CONTROL_MASK);
  g_assert_cmpint (gbk_window_n_tabs (w), ==, 3);
  GtkWidget *alive = GTK_WIDGET (w);
  g_object_add_weak_pointer (G_OBJECT (w), (void **) &alive);
  for (int i = 0; i < 3 && alive != nullptr; i++)
    send_key_to_window (w, GDK_KEY_w, GDK_CONTROL_MASK);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_assert_null (alive);
  (void) two;
  rm_rf (root);
}

static bool
no_workers (void *data G_GNUC_UNUSED)
{
  return gbk_scanner_live_workers () == 0;
}

/* Closing a window with several tabs, each with a selection: tearing a tab down clears its
 * selection, and nothing may then ask the half-gone window for its current tab (it used to
 * abort: "window without tabs"). */
static void
test_close_with_selections (void)
{
  g_autofree char *root = g_dir_make_tmp ("gbk-win-XXXXXX", nullptr);
  mkdirs (root, (const char *const[]) { "a.txt", "sub/", "sub/b.txt", nullptr });
  GbkWindow *w = window_at (app, ctx, root);
  gtk_widget_show_all (GTK_WIDGET (w));
  wait_at (w, root);
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "a.txt"));
  g_autofree char *sub = g_build_filename (root, "sub", nullptr);
  new_tab_at (w, sub);
  wait_at (w, sub);
  gbk_view_select_only (gbk_window_view (w), gbk_dir_model_find (model_of (w), "b.txt"));
  /* Back on the first tab: tearing down then takes the current tab away first. */
  GtkNotebook *tabs = GTK_NOTEBOOK (
      gtk_widget_get_ancestor (GTK_WIDGET (gbk_window_view (w)), GTK_TYPE_NOTEBOOK));
  gtk_notebook_set_current_page (tabs, 0);
  while (gtk_events_pending ())
    gtk_main_iteration ();
  gtk_widget_destroy (GTK_WIDGET (w));
  while (gtk_events_pending ())
    gtk_main_iteration ();
  rm_rf (root);
}

int
main (int argc, char **argv)
{
  /* Private data dir: the trash goes here, never into anyone's real one. */
  g_autofree char *data_home = g_dir_make_tmp ("gbk-win-data-XXXXXX", nullptr);
  g_setenv ("XDG_DATA_HOME", data_home, true);
  /* A private home for the path bar's "start at home" behaviour. */
  g_autofree char *home = g_dir_make_tmp ("gbk-win-home-XXXXXX", nullptr);
  g_setenv ("HOME", home, true);
  gtk_test_init (&argc, &argv, nullptr);
  ctx = gbk_context_new ("/nonexistent/gbk-test.conf");
  app = gtk_application_new ("net.gbk.QuoinTest", G_APPLICATION_NON_UNIQUE);
  g_autoptr (GError) error = nullptr;
  if (!g_application_register (G_APPLICATION (app), nullptr, &error))
    gbk_fatal ("register: %s", error->message);
  gbk_window_install_accels (app);   /* the real bindings: tests press real keys */

  g_test_add_func ("/window/navigation", test_navigation);
  g_test_add_func ("/window/vanishing-directory", test_vanishing_directory);
  g_test_add_func ("/window/terminal-missing", test_terminal_missing);
  g_test_add_func ("/window/context-menu", test_context_menu);
  g_test_add_func ("/window/open-folder-elsewhere", test_open_folder_elsewhere);
  g_test_add_func ("/window/scroll-extent", test_scroll_extent);
  g_test_add_func ("/window/status-bar-menu", test_status_bar_menu);
  g_test_add_func ("/window/drag-and-drop", test_drag_and_drop);
  g_test_add_func ("/window/real-drag-and-drop", test_real_drag_and_drop);
  g_test_add_func ("/window/create-executable-properties", test_create_executable_properties);
  g_test_add_func ("/window/file-operations", test_file_operations);
  g_test_add_func ("/window/job-outlives-window", test_job_outlives_window);
  g_test_add_func ("/window/pathbar", test_pathbar);
  g_test_add_func ("/window/keys-go-to-focused-entry", test_keys_go_to_focused_entry);
  g_test_add_func ("/window/tabs", test_tabs);
  g_test_add_func ("/window/close-with-selections", test_close_with_selections);
  int status = g_test_run ();

  while (gtk_events_pending ())
    gtk_main_iteration ();
  g_object_unref (app);
  gbk_context_unref (ctx);
  wait_until (no_workers, nullptr);
  if (gbk_lifetime_live () != 0)
    gbk_fatal ("%d registrations leaked", gbk_lifetime_live ());
  return status;
}
