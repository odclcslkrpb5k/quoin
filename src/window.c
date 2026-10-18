#include "window.h"
#include "clipboard.h"
#include "devices.h"
#include "cliptext.h"
#include "jobui.h"
#include "lifetime.h"
#include "menu.h"
#include "open.h"
#include "props.h"

struct _GbkWindow
{
  GtkApplicationWindow parent_instance;

  GbkContext *ctx;
  GtkNotebook *tabs;
  GtkLabel *status;
  GtkWidget *status_bar;  /* event box: right-click gives the folder's (empty-space) menu */
  GtkButton *sort_button; /* status bar, right: shows the sort, click cycles it */
  GtkToggleButton *hidden_button;   /* status bar, right: ".*", bound to win.show-hidden */

  bool show_hidden;       /* window-wide: applies to every tab */
  GbkSortMode sort;
  GbkLoc *paste_dest;     /* destination of the paste request in flight */
  GtkGesture *mouse_back;     /* mouse buttons 8/9, caught before any child sees them */
  GtkGesture *mouse_forward;
  GbkDevices *devices;    /* the ☰ menu's Devices submenu, and its operations */
};

G_DEFINE_FINAL_TYPE (GbkWindow, gbk_window, GTK_TYPE_APPLICATION_WINDOW)

/* ---- tabs ---- */

static GbkBrowser *
active (GbkWindow *w)
{
  int i = gtk_notebook_get_current_page (w->tabs);
  if (i < 0)
    gbk_fatal ("window without tabs");
  return GBK_BROWSER (gtk_notebook_get_nth_page (w->tabs, i));
}

static GbkBrowser *
browser_of (GbkView *v)
{
  GtkWidget *b = gtk_widget_get_ancestor (GTK_WIDGET (v), GBK_TYPE_BROWSER);
  if (b == nullptr)
    gbk_fatal ("view outside a browser");
  return GBK_BROWSER (b);
}

static void
update_status (GbkWindow *w)
{
  g_autofree char *text = gbk_browser_status (active (w));
  gtk_label_set_text (w->status, text);
}

/* The title is the full location of the open folder, and nothing else. */
static void
set_title_loc (GbkWindow *w, const GbkLoc *loc)
{
  g_autofree char *shown = gbk_loc_display (loc);
  gtk_window_set_title (GTK_WINDOW (w), shown);
}

/* Sort modes in the order the status bar button cycles through them. The arrow says which
 * way the list runs: names A to Z, dates newest first, sizes largest first. */
static const struct
{
  GbkSortMode mode;
  const char *state;     /* win.sort state */
  const char *label;
  const char *tooltip;
} sorts[] = {
  { GBK_SORT_NAME, "name", "↑Name", "Sorted by name. Click to sort by date modified." },
  { GBK_SORT_MTIME, "modified", "↓Modified",
    "Sorted by date modified, newest first. Click to sort by size." },
  { GBK_SORT_SIZE, "size", "↓Size", "Sorted by size, largest first. Click to sort by name." },
};

static size_t
sort_index (GbkSortMode mode)
{
  for (size_t i = 0; i < G_N_ELEMENTS (sorts); i++)
    if (sorts[i].mode == mode)
      return i;
  gbk_fatal ("sort mode %d missing from the cycle", (int) mode);
}

static void
update_sort_button (GbkWindow *w)
{
  size_t i = sort_index (w->sort);
  gtk_button_set_label (w->sort_button, sorts[i].label);
  gtk_widget_set_tooltip_text (GTK_WIDGET (w->sort_button), sorts[i].tooltip);
}

static void
update_chrome (GbkWindow *w)
{
  const GbkBrowser *b = active (w);
  set_title_loc (w, gbk_browser_loc (b));
  update_status (w);
}

static void
update_tab_label (GbkBrowser *b)
{
  GtkLabel *label = g_object_get_data (G_OBJECT (b), "gbk-tab-label");
  g_autofree char *shown = gbk_loc_display_name (gbk_browser_loc (b));
  gtk_label_set_text (label, shown);
  /* An ellipsizing label asks only for "…": give it room for the name, up to a cap. */
  gtk_label_set_width_chars (label, (int) MIN (g_utf8_strlen (shown, -1) + 2, 32));
  g_autofree char *full = gbk_loc_display (gbk_browser_loc (b));
  gtk_widget_set_tooltip_text (GTK_WIDGET (label), full);
}

static void
on_browser_path (GbkBrowser *b, GbkWindow *w)
{
  update_tab_label (b);
  if (b == active (w))
    update_chrome (w);
}

static void
on_browser_status (GbkBrowser *b, GbkWindow *w)
{
  if (b == active (w))
    update_status (w);
}

static void
close_tab (GbkWindow *w, GbkBrowser *b)
{
  if (gtk_notebook_get_n_pages (w->tabs) <= 1)
    {
      gtk_window_close (GTK_WINDOW (w));
      return;
    }
  gtk_widget_destroy (GTK_WIDGET (b));
}

static void
on_tab_close_clicked (GtkButton *button G_GNUC_UNUSED, GbkBrowser *b)
{
  GbkWindow *w = GBK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (b)));
  close_tab (w, b);
}

static void
update_tabs_visible (GbkWindow *w)
{
  /* Like Thunar: no tab bar until there's a second tab. */
  gtk_notebook_set_show_tabs (w->tabs, gtk_notebook_get_n_pages (w->tabs) > 1);
}

static void on_activate (GbkView *v, GbkWindow *w);
static GtkWidget *app_menu_button (GbkWindow *w);
static void on_context_menu (GbkView *v, GdkEvent *event, GbkWindow *w);
static void on_pathbar_menu (GbkPathbar *bar, GbkLoc *loc, GdkEvent *event, GbkWindow *w);
static void on_view_drop (GbkView *v, GPtrArray *locs, guint64 folder, GdkEvent *trigger,
                          GbkWindow *w);
static void on_pathbar_drop (GbkPathbar *bar, GbkLoc *loc, GPtrArray *locs, GdkEvent *trigger,
                             GbkWindow *w);

static GbkBrowser *
add_tab (GbkWindow *w, GbkLoc *loc, bool switch_to)
{
  GbkBrowser *b = gbk_browser_new (w->ctx, w->show_hidden, w->sort);

  GtkWidget *label_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *label = gtk_label_new ("");
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 30);
  GtkWidget *close = gtk_button_new_from_icon_name ("window-close-symbolic", GTK_ICON_SIZE_MENU);
  gtk_button_set_relief (GTK_BUTTON (close), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click (close, false);
  gtk_box_pack_start (GTK_BOX (label_box), label, true, true, 0);
  gtk_box_pack_start (GTK_BOX (label_box), close, false, false, 0);
  gtk_widget_show_all (label_box);
  g_object_set_data (G_OBJECT (b), "gbk-tab-label", label);

  /* The window's menu button leads each tab's toolbar row (so it's there with one tab and
   * the tab strip hidden). Its actions are window actions, shared by every tab. */
  GtkWidget *menu_button = app_menu_button (w);
  gtk_box_pack_start (gbk_browser_toolbar (b), menu_button, false, false, 0);
  gtk_box_reorder_child (gbk_browser_toolbar (b), menu_button, 0);

  /* Open before joining the notebook: inserting the first page emits "switch-page", which
   * reads the tab's location. */
  gbk_browser_open (b, loc);

  GbkView *v = gbk_browser_view (b);
  gbk_connect (G_OBJECT (b), close, "clicked", G_CALLBACK (on_tab_close_clicked), b);
  gbk_connect (G_OBJECT (w), b, "path-changed", G_CALLBACK (on_browser_path), w);
  gbk_connect (G_OBJECT (w), b, "status-changed", G_CALLBACK (on_browser_status), w);
  gbk_connect (G_OBJECT (w), v, "activate", G_CALLBACK (on_activate), w);
  gbk_connect (G_OBJECT (w), v, "context-menu", G_CALLBACK (on_context_menu), w);
  gbk_connect (G_OBJECT (w), gbk_browser_pathbar (b), "context-menu", G_CALLBACK (on_pathbar_menu),
               w);
  gbk_connect (G_OBJECT (w), v, "drop", G_CALLBACK (on_view_drop), w);
  gbk_connect (G_OBJECT (w), gbk_browser_pathbar (b), "drop", G_CALLBACK (on_pathbar_drop), w);

  gtk_widget_show_all (GTK_WIDGET (b));
  int at = gtk_notebook_get_current_page (w->tabs) + 1;
  gtk_notebook_insert_page (w->tabs, GTK_WIDGET (b), label_box, at);
  gtk_notebook_set_tab_reorderable (w->tabs, GTK_WIDGET (b), true);
  update_tab_label (b);
  update_tabs_visible (w);
  if (switch_to)
    {
      gtk_notebook_set_current_page (w->tabs, gtk_notebook_page_num (w->tabs, GTK_WIDGET (b)));
      gtk_widget_grab_focus (GTK_WIDGET (v));
    }
  return b;
}

static void
on_switch_page (GtkNotebook *nb G_GNUC_UNUSED, GtkWidget *page, guint n G_GNUC_UNUSED, GbkWindow *w)
{
  /* "switch-page" runs before the current page changes: use the page we're given. */
  GbkBrowser *b = GBK_BROWSER (page);
  set_title_loc (w, gbk_browser_loc (b));
  g_autofree char *text = gbk_browser_status (b);
  gtk_label_set_text (w->status, text);
  gtk_widget_grab_focus (GTK_WIDGET (gbk_browser_view (b)));
}

static void
on_page_removed (GtkNotebook *nb G_GNUC_UNUSED, GtkWidget *child G_GNUC_UNUSED,
                 guint n G_GNUC_UNUSED, GbkWindow *w)
{
  update_tabs_visible (w);
}

static void
set_show_hidden (GbkWindow *w, bool show)
{
  w->show_hidden = show;
  for (int i = 0; i < gtk_notebook_get_n_pages (w->tabs); i++)
    gbk_browser_set_show_hidden (GBK_BROWSER (gtk_notebook_get_nth_page (w->tabs, i)), show);
  GAction *a = g_action_map_lookup_action (G_ACTION_MAP (w), "show-hidden");
  /* Also updates the status bar's ".*" toggle, which is bound to this action. */
  g_simple_action_set_state (G_SIMPLE_ACTION (a), g_variant_new_boolean (show));
}

static void
set_sort (GbkWindow *w, GbkSortMode sort)
{
  w->sort = sort;
  for (int i = 0; i < gtk_notebook_get_n_pages (w->tabs); i++)
    gbk_browser_set_sort (GBK_BROWSER (gtk_notebook_get_nth_page (w->tabs, i)), sort);
  GAction *a = g_action_map_lookup_action (G_ACTION_MAP (w), "sort");
  g_simple_action_set_state (G_SIMPLE_ACTION (a),
                             g_variant_new_string (sorts[sort_index (sort)].state));
  update_sort_button (w);
}

static void
on_sort_button_clicked (GtkButton *button G_GNUC_UNUSED, GbkWindow *w)
{
  set_sort (w, sorts[(sort_index (w->sort) + 1) % G_N_ELEMENTS (sorts)].mode);
}

/* ---- what works on another machine ---- */

/* Shortcuts ask what the menu asks (gbk_menu_works_remote): on another machine, browsing
 * only for now. True, with a message, if `kind` isn't possible in tab `b`. */
static bool
refused_remote (GbkBrowser *b, GbkMenuKind kind, bool on_file)
{
  if (gbk_loc_is_local (gbk_browser_loc (b)) || gbk_menu_works_remote (kind, on_file))
    return false;
  gbk_browser_set_message (b, "Not possible on another machine yet");
  return true;
}

/* ---- opening things ---- */

/* The selection as owned GbkItems: safe to keep while the model changes underneath. */
static GPtrArray *
snapshot_selection (const GbkWindow *w, const GbkBrowser *b)
{
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  GbkDirModel *m = gbk_browser_model (b);
  if (m == nullptr)
    return items;
  size_t n;
  g_autofree GbkHandle *sel = gbk_view_selected (gbk_browser_view (b), &n);
  for (size_t i = 0; i < n; i++)
    {
      const GbkEntry *e = gbk_dir_model_get (m, sel[i]);
      if (e != nullptr)
        g_ptr_array_add (items,
                         gbk_item_new (w->ctx->config, w->ctx->types, gbk_browser_loc (b), e));
    }
  return items;
}

static void
report (GbkBrowser *b, const char *what, const GError *error)
{
  g_autofree char *msg = g_strdup_printf ("%s: %s", what, error->message);
  gbk_browser_set_message (b, msg);
}

static GAppLaunchContext *
launch_context (GbkWindow *w)
{
  GdkAppLaunchContext *lc
      = gdk_display_get_app_launch_context (gtk_widget_get_display (GTK_WIDGET (w)));
  gdk_app_launch_context_set_timestamp (lc, gtk_get_current_event_time ());
  return G_APP_LAUNCH_CONTEXT (lc);
}

/* Double-click / Enter. A single folder is entered; several folders open in new tabs;
 * files are run or opened per gbk_open_plan. */
static void
open_items (GbkWindow *w, GbkBrowser *b, GPtrArray *items)
{
  if (items->len == 1 && ((GbkItem *) g_ptr_array_index (items, 0))->is_dir)
    {
      gbk_browser_open (b, ((GbkItem *) g_ptr_array_index (items, 0))->loc);
      return;
    }
  for (guint i = 0; i < items->len; i++)
    {
      const GbkItem *it = g_ptr_array_index (items, i);
      if (!it->is_dir && refused_remote (b, GBK_MENU_OPEN, true))
        return;
    }
  g_autoptr (GPtrArray) by_default = g_ptr_array_new ();
  for (guint i = 0; i < items->len; i++)
    {
      GbkItem *it = g_ptr_array_index (items, i);
      g_autoptr (GError) error = nullptr;
      switch (gbk_open_plan (it))
        {
        case GBK_OPEN_ENTER: add_tab (w, it->loc, false); break;
        case GBK_OPEN_RUN:
          if (!gbk_open_execute (w->ctx->config, it, false, gbk_browser_loc (b), &error))
            report (b, it->display, error);
          break;
        case GBK_OPEN_RUN_TERMINAL:
          if (!gbk_open_execute (w->ctx->config, it, true, gbk_browser_loc (b), &error))
            report (b, it->display, error);
          break;
        case GBK_OPEN_DEFAULT: g_ptr_array_add (by_default, it); break;
        default: gbk_fatal ("bad open plan");
        }
    }
  if (by_default->len > 0)
    {
      g_autoptr (GAppLaunchContext) lc = launch_context (w);
      g_autoptr (GError) error = nullptr;
      if (!gbk_open_default (by_default, lc, &error))
        report (b, "Open", error);
    }
}

/* ---- view events ---- */

static void
on_activate (GbkView *v, GbkWindow *w)
{
  GbkBrowser *b = browser_of (v);
  g_autoptr (GPtrArray) items = snapshot_selection (w, b);
  open_items (w, b, items);
}

/* ---- file operations ---- */

static GPtrArray *
locs_of (GPtrArray *items)
{
  GPtrArray *locs = gbk_loc_array_new ();
  for (guint i = 0; i < items->len; i++)
    g_ptr_array_add (locs, gbk_loc_ref (((GbkItem *) g_ptr_array_index (items, i))->loc));
  return locs;
}

static void
on_job_finished (GbkJob *job, GError *error, GbkWindow *w)
{
  if (error == nullptr && g_object_get_data (G_OBJECT (job), "gbk-from-cut") != nullptr)
    gbk_clipboard_clear_if_ours (GTK_WIDGET (w));
  /* Select what was just created in every tab looking at where it went. */
  const GbkLoc *dest = gbk_job_dest (job);
  if (dest == nullptr)
    return;
  for (int i = 0; i < gtk_notebook_get_n_pages (w->tabs); i++)
    {
      GbkBrowser *b = GBK_BROWSER (gtk_notebook_get_nth_page (w->tabs, i));
      if (gbk_loc_equal (gbk_browser_loc (b), dest))
        gbk_browser_select_names (b, gbk_job_created (job));
    }
}

static void
start_job (GbkWindow *w, GbkJob *job)
{
  gbk_connect (G_OBJECT (w), job, "finished", G_CALLBACK (on_job_finished), w);
  gbk_jobui_start (job, GTK_WINDOW (w));
}

static void
copy_items (GbkWindow *w, GbkBrowser *b, GPtrArray *items, bool cut)
{
  if (items->len == 0)
    return;
  g_autoptr (GPtrArray) locs = locs_of (items);
  if (!gbk_clipboard_set_files (GTK_WIDGET (w), locs, cut))
    {
      gbk_browser_set_message (b, "Files on another machine can't be copied yet");
      return;
    }
  g_autofree char *msg = g_strdup_printf (
      "%u %s %s", items->len, items->len == 1 ? "item" : "items", cut ? "cut" : "copied");
  gbk_browser_set_message (b, msg);
}

static void
on_paste_files (GPtrArray *locs, bool cut, void *data)
{
  GbkWindow *w = data;
  g_autoptr (GbkLoc) dest = g_steal_pointer (&w->paste_dest);
  if (locs->len == 0 || dest == nullptr)
    {
      gbk_browser_set_message (active (w), "Nothing to paste");
      return;
    }
  g_autoptr (GbkJob) job = gbk_job_new (cut ? GBK_OP_MOVE : GBK_OP_COPY, locs, dest);
  if (cut)
    g_object_set_data (G_OBJECT (job), "gbk-from-cut", GINT_TO_POINTER (1));
  start_job (w, job);
}

static void
paste_into (GbkWindow *w, GbkLoc *dest)
{
  g_clear_pointer (&w->paste_dest, gbk_loc_unref);
  w->paste_dest = gbk_loc_ref (dest);
  gbk_clipboard_get_files (G_OBJECT (w), GTK_WIDGET (w), on_paste_files, w);
}

static void
trash_items (GbkWindow *w, GPtrArray *items)
{
  if (items->len == 0)
    return;
  g_autoptr (GPtrArray) locs = locs_of (items);
  g_autoptr (GbkJob) job = gbk_job_new (GBK_OP_TRASH, locs, nullptr);
  start_job (w, job);
}

static void
on_delete_response (GtkDialog *d, int response, GbkWindow *w)
{
  if (response == GTK_RESPONSE_ACCEPT)
    {
      const GPtrArray *locs = g_object_get_data (G_OBJECT (d), "gbk-locs");
      g_autoptr (GbkJob) job = gbk_job_new (GBK_OP_DELETE, locs, nullptr);
      start_job (w, job);
    }
  gtk_widget_destroy (GTK_WIDGET (d));
}

static void
delete_items (GbkWindow *w, GPtrArray *items)
{
  if (items->len == 0)
    return;
  const GbkItem *first = g_ptr_array_index (items, 0);
  GtkWidget *d = items->len == 1
                     ? gtk_message_dialog_new (GTK_WINDOW (w),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                               "Permanently delete “%s”?", first->display)
                     : gtk_message_dialog_new (GTK_WINDOW (w),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                               "Permanently delete %u items?", items->len);
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (d),
                                            "This can't be undone. They won't go to the trash.");
  gtk_dialog_add_buttons (GTK_DIALOG (d), "_Cancel", GTK_RESPONSE_CANCEL, "_Delete",
                          GTK_RESPONSE_ACCEPT, nullptr);
  gtk_dialog_set_default_response (GTK_DIALOG (d), GTK_RESPONSE_CANCEL);
  g_object_set_data_full (G_OBJECT (d), "gbk-locs", locs_of (items),
                          (GDestroyNotify) g_ptr_array_unref);
  gbk_connect (G_OBJECT (w), d, "response", G_CALLBACK (on_delete_response), w);
  gtk_widget_show_all (d);
}

/* ---- name dialogs: rename, create ---- */

typedef enum
{
  NAME_RENAME,          /* `loc` is the item */
  NAME_CREATE_FILE,     /* `loc` is the directory */
  NAME_CREATE_FOLDER,
} NameAction;

static void
on_name_response (GtkDialog *d, int response, GbkWindow *w)
{
  if (response == GTK_RESPONSE_ACCEPT)
    {
      NameAction act
          = (NameAction) GPOINTER_TO_INT (g_object_get_data (G_OBJECT (d), "gbk-action"));
      GbkLoc *loc = g_object_get_data (G_OBJECT (d), "gbk-loc");
      GtkEntry *entry = g_object_get_data (G_OBJECT (d), "gbk-entry");
      const char *what = act == NAME_RENAME ? "Can't rename" : "Can't create";
      /* The entry holds UTF-8; filenames are bytes in the filesystem encoding. */
      g_autoptr (GError) error = nullptr;
      g_autofree char *name
          = g_filename_from_utf8 (gtk_entry_get_text (entry), -1, nullptr, nullptr, &error);
      if (name == nullptr || !gbk_ops_valid_name (name, &error))
        gbk_jobui_error (GTK_WINDOW (w), what, error->message);
      else
        {
          g_autoptr (GbkJob) job = act == NAME_RENAME
                                       ? gbk_job_new_rename (loc, name)
                                       : gbk_job_new_create (loc, name, act == NAME_CREATE_FOLDER);
          start_job (w, job);
        }
    }
  gtk_widget_destroy (GTK_WIDGET (d));
}

/* Ask for a name, starting from `initial` (filename bytes). Selects all of it, or only the
 * part before the extension when `select_stem`, so typing replaces the right part. */
static void
name_dialog (GbkWindow *w, NameAction act, GbkLoc *loc, const char *initial, bool select_stem)
{
  static const char *const titles[] = { "Rename", "Create File", "Create Folder" };
  GtkWidget *d = gtk_dialog_new_with_buttons (
      titles[act], GTK_WINDOW (w), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "_Cancel",
      GTK_RESPONSE_CANCEL, act == NAME_RENAME ? "_Rename" : "C_reate", GTK_RESPONSE_ACCEPT,
      nullptr);
  gtk_dialog_set_default_response (GTK_DIALOG (d), GTK_RESPONSE_ACCEPT);
  GtkWidget *entry = gtk_entry_new ();
  g_autofree char *shown = g_filename_display_name (initial);
  gtk_entry_set_text (GTK_ENTRY (entry), shown);
  gtk_entry_set_activates_default (GTK_ENTRY (entry), true);
  gtk_entry_set_width_chars (GTK_ENTRY (entry),
                             (int) MIN (60, MAX (30, g_utf8_strlen (shown, -1) + 5)));
  GtkWidget *area = gtk_dialog_get_content_area (GTK_DIALOG (d));
  gtk_container_set_border_width (GTK_CONTAINER (area), 8);
  gtk_box_pack_start (GTK_BOX (area), entry, true, true, 0);
  g_object_set_data (G_OBJECT (d), "gbk-action", GINT_TO_POINTER ((int) act));
  g_object_set_data_full (G_OBJECT (d), "gbk-loc", gbk_loc_ref (loc),
                          (GDestroyNotify) gbk_loc_unref);
  g_object_set_data (G_OBJECT (d), "gbk-entry", entry);
  gbk_connect (G_OBJECT (w), d, "response", G_CALLBACK (on_name_response), w);
  gtk_widget_show_all (d);
  const char *dot = select_stem ? strrchr (shown, '.') : nullptr;
  int end = (dot != nullptr && dot != shown) ? (int) g_utf8_pointer_to_offset (shown, dot) : -1;
  gtk_editable_select_region (GTK_EDITABLE (entry), 0, end);
}

static void
rename_item (GbkWindow *w, const GbkItem *it)
{
  if (it == nullptr)
    gbk_fatal ("rename without an item");
  g_autofree char *base = gbk_loc_name (it->loc);
  name_dialog (w, NAME_RENAME, it->loc, base, !it->is_dir);
}

/* "New Folder", or "New Folder (n)" if the folder (as last listed) has that name. Only a
 * suggestion: creating never replaces, whatever appeared since. */
static void
create_item (GbkWindow *w, const GbkBrowser *b, GbkLoc *dir, bool folder)
{
  const char *base = folder ? "New Folder" : "New File";
  GbkDirModel *m = gbk_browser_model (b);
  g_autofree char *name = g_strdup (base);
  for (unsigned n = 1; m != nullptr && gbk_dir_model_find (m, name) != GBK_HANDLE_NONE && n < 10000;
       n++)
    {
      g_free (name);
      name = gbk_ops_numbered_name (base, n);
    }
  name_dialog (w, folder ? NAME_CREATE_FOLDER : NAME_CREATE_FILE, dir, name, false);
}

static bool
all_executable (const GPtrArray *items)
{
  for (guint i = 0; i < items->len; i++)
    if (!((const GbkItem *) g_ptr_array_index (items, i))->executable)
      return false;
  return items->len > 0;
}

/* Executable on for all of them, unless all of them already are: then off. */
static void
toggle_executable (GbkWindow *w, GPtrArray *items)
{
  g_autoptr (GPtrArray) locs = locs_of (items);
  g_autoptr (GbkJob) job = gbk_job_new_executable (locs, !all_executable (items));
  start_job (w, job);
}

static void
show_properties (GbkWindow *w, const GbkItem *it)
{
  GbkProps *p = gbk_props_new (GTK_WINDOW (w), it->loc, it->mime);
  gtk_widget_show (GTK_WIDGET (p));
}

/* ---- context menu ---- */

/* Each menu item carries its entry kind, plus an action or app; the menu carries the item
 * snapshot. Activations are connected with the window as owner, so nothing fires after the
 * window is gone; the data is owned by the menu (items), so nothing outlives it. */
#define KEY_ITEMS "gbk-items"
#define KEY_KIND "gbk-kind"
#define KEY_ACTION "gbk-action"
#define KEY_APP "gbk-app"
#define KEY_BROWSER "gbk-browser"

/* The top-level context menu an item belongs to (Open With items sit in a submenu). */
static GtkWidget *
menu_of (GtkWidget *menu_item)
{
  GtkWidget *menu = gtk_widget_get_parent (menu_item);
  while (menu != nullptr && g_object_get_data (G_OBJECT (menu), KEY_ITEMS) == nullptr)
    {
      GtkWidget *attach
          = GTK_IS_MENU (menu) ? gtk_menu_get_attach_widget (GTK_MENU (menu)) : nullptr;
      menu = attach != nullptr ? gtk_widget_get_parent (attach) : nullptr;
    }
  if (menu == nullptr)
    gbk_fatal ("menu item without a snapshot");
  return menu;
}

static GPtrArray *
menu_items_of (GtkWidget *menu_item)
{
  return g_object_get_data (G_OBJECT (menu_of (menu_item)), KEY_ITEMS);
}

/* Paths are shown as display names: exact for UTF-8 names (the norm), with U+FFFD for bytes
 * that can't be text at all. A location on another machine is "host:/path", as scp and
 * rsync take it: never a bare path, which would name a file on this machine. */
static void
copy_paths (GbkWindow *w, GbkBrowser *b, GPtrArray *items)
{
  g_autoptr (GPtrArray) locs = locs_of (items);
  g_autoptr (GPtrArray) paths = gbk_loc_array_as_local (locs);
  g_autofree char *text = nullptr;
  if (paths != nullptr)
    text = gbk_clip_format_text (paths);
  else
    {
      GString *s = g_string_new ("");
      for (guint i = 0; i < locs->len; i++)
        {
          g_autofree char *shown = gbk_loc_display (g_ptr_array_index (locs, i));
          g_string_append_printf (s, "%s%s", i > 0 ? "\n" : "", shown);
        }
      text = g_string_free (s, false);
    }
  gbk_clipboard_set_text (GTK_WIDGET (w), text);
  g_autofree char *msg = items->len == 1 ? g_strdup ("Path copied")
                                         : g_strdup_printf ("%u paths copied", items->len);
  gbk_browser_set_message (b, msg);
}

static void
on_menu_item (GtkMenuItem *mi, GbkWindow *w)
{
  /* Our own reference: what an entry does must not free the snapshot it's working from. */
  g_autoptr (GPtrArray) items = g_ptr_array_ref (menu_items_of (GTK_WIDGET (mi)));
  /* The tab the menu was opened in (it may no longer be the active one). */
  GbkBrowser *b = g_object_get_data (G_OBJECT (menu_of (GTK_WIDGET (mi))), KEY_BROWSER);
  if (b == nullptr)
    return;
  GbkLoc *here = gbk_browser_loc (b);
  GbkMenuKind kind = (GbkMenuKind) GPOINTER_TO_INT (g_object_get_data (G_OBJECT (mi), KEY_KIND));
  /* Invariant: a context menu always has a target (empty space = the folder itself). */
  if (items->len == 0 || g_ptr_array_index (items, 0) == nullptr)
    gbk_fatal ("context menu without a target");
  const GbkItem *first = g_ptr_array_index (items, 0);
  g_autoptr (GError) error = nullptr;
  bool ok = true;

  switch (kind)
    {
    case GBK_MENU_OPEN: open_items (w, b, items); break;
    case GBK_MENU_OPEN_TAB:
      for (guint i = 0; i < items->len; i++)       /* show the first one opened */
        add_tab (w, ((GbkItem *) g_ptr_array_index (items, i))->loc, i == 0);
      break;
    case GBK_MENU_OPEN_WINDOW:
      for (guint i = 0; i < items->len; i++)
        {
          GbkWindow *n = gbk_window_new (gtk_window_get_application (GTK_WINDOW (w)), w->ctx,
                                         ((GbkItem *) g_ptr_array_index (items, i))->loc);
          gtk_widget_show_all (GTK_WIDGET (n));
        }
      break;
    case GBK_MENU_OPEN_WITH:
      {
        GAppInfo *app = g_object_get_data (G_OBJECT (mi), KEY_APP);
        g_autoptr (GAppLaunchContext) lc = launch_context (w);
        ok = gbk_open_with (app, items, lc, &error);
        break;
      }
    case GBK_MENU_EXECUTE:
      ok = gbk_open_execute (w->ctx->config, first, false, here, &error);
      break;
    case GBK_MENU_EXECUTE_TERMINAL:
      ok = gbk_open_execute (w->ctx->config, first, true, here, &error);
      break;
    case GBK_MENU_TERMINAL_HERE: ok = gbk_open_terminal (w->ctx->config, first->loc, &error); break;
    case GBK_MENU_ACTION:
      ok = gbk_open_action (g_object_get_data (G_OBJECT (mi), KEY_ACTION), items, here, &error);
      break;
    case GBK_MENU_SORT_NAME: set_sort (w, GBK_SORT_NAME); break;
    case GBK_MENU_SORT_MTIME: set_sort (w, GBK_SORT_MTIME); break;
    case GBK_MENU_SORT_SIZE: set_sort (w, GBK_SORT_SIZE); break;
    case GBK_MENU_SHOW_HIDDEN: set_show_hidden (w, !w->show_hidden); break;
    case GBK_MENU_CUT: copy_items (w, b, items, true); break;
    case GBK_MENU_COPY: copy_items (w, b, items, false); break;
    case GBK_MENU_COPY_PATH: copy_paths (w, b, items); break;
    case GBK_MENU_PASTE: paste_into (w, here); break;
    case GBK_MENU_PASTE_INTO: paste_into (w, first->loc); break;
    case GBK_MENU_TRASH: trash_items (w, items); break;
    case GBK_MENU_DELETE: delete_items (w, items); break;
    case GBK_MENU_RENAME: rename_item (w, first); break;
    case GBK_MENU_EXECUTABLE: toggle_executable (w, items); break;
    case GBK_MENU_PROPERTIES: show_properties (w, first); break;
    case GBK_MENU_CREATE_FOLDER: create_item (w, b, first->loc, true); break;
    case GBK_MENU_CREATE_FILE: create_item (w, b, first->loc, false); break;
    case GBK_MENU_SEPARATOR:
    default: gbk_fatal ("bad menu entry kind %d", (int) kind);
    }
  if (!ok)
    report (b, gtk_menu_item_get_label (mi), error);
}

static GtkWidget *
menu_item (GbkWindow *w, GtkWidget *menu, const char *label, GbkMenuKind kind)
{
  GtkWidget *mi = gtk_menu_item_new_with_label (label);
  g_object_set_data (G_OBJECT (mi), KEY_KIND, GINT_TO_POINTER ((int) kind));
  gbk_connect (G_OBJECT (w), mi, "activate", G_CALLBACK (on_menu_item), w);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
  return mi;
}

static GtkWidget *
check_item (GbkWindow *w, GtkWidget *menu, const char *label, GbkMenuKind kind, bool active,
            bool radio)
{
  GtkWidget *mi = gtk_check_menu_item_new_with_label (label);
  gtk_check_menu_item_set_draw_as_radio (GTK_CHECK_MENU_ITEM (mi), radio);
  gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (mi), active);   /* before connecting */
  g_object_set_data (G_OBJECT (mi), KEY_KIND, GINT_TO_POINTER ((int) kind));
  gbk_connect (G_OBJECT (w), mi, "activate", G_CALLBACK (on_menu_item), w);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), mi);
  return mi;
}

static void
add_open_with (GbkWindow *w, GtkWidget *menu, const char *mime)
{
  GList *apps = g_app_info_get_recommended_for_type (mime);
  if (apps == nullptr)
    return;
  GtkWidget *sub = gtk_menu_new ();
  for (GList *l = apps; l != nullptr; l = l->next)
    {
      GAppInfo *app = l->data;
      g_autofree char *label = g_strdup (g_app_info_get_display_name (app));
      GtkWidget *mi = menu_item (w, sub, label, GBK_MENU_OPEN_WITH);
      g_object_set_data_full (G_OBJECT (mi), KEY_APP, g_object_ref (app), g_object_unref);
    }
  g_list_free_full (apps, g_object_unref);
  GtkWidget *parent = gtk_menu_item_new_with_label ("Open With");
  gtk_menu_item_set_submenu (GTK_MENU_ITEM (parent), sub);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), parent);
}

static bool
destroy_menu (void *data)
{
  gtk_widget_destroy (GTK_WIDGET (data));
  return false;
}

static void
on_menu_deactivate (GtkMenuShell *menu, void *data G_GNUC_UNUSED)
{
  /* "deactivate" comes before the chosen item's "activate": destroy on the next idle. */
  gbk_idle_add (G_OBJECT (menu), destroy_menu, menu);
}

static void
on_attach_destroyed (GtkWidget *attach G_GNUC_UNUSED, GtkWidget *menu)
{
  gtk_widget_destroy (menu);
}

/* Build and pop up the context menu for `items` in tab `b`. The menu is attached to
 * `attach` and destroyed with it (or after it closes). */
static void
popup_menu (GbkWindow *w, GbkBrowser *b, GPtrArray *items, GbkMenuTarget target, GtkWidget *attach,
            GdkEvent *event)
{
  g_autoptr (GArray) entries = gbk_menu_entries (w->ctx->config, items, target);
  GtkWidget *menu = gtk_menu_new ();
  g_object_set_data_full (G_OBJECT (menu), KEY_ITEMS, g_ptr_array_ref (items),
                          (GDestroyNotify) g_ptr_array_unref);
  /* Borrowed: the menu dies with `attach` (below), which dies with the browser. */
  g_object_set_data (G_OBJECT (menu), KEY_BROWSER, b);

  for (guint i = 0; i < entries->len; i++)
    {
      const GbkMenuEntry *e = &g_array_index (entries, GbkMenuEntry, i);
      switch (e->kind)
        {
        case GBK_MENU_SEPARATOR:
          gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
          break;
        case GBK_MENU_OPEN: menu_item (w, menu, "Open", e->kind); break;
        case GBK_MENU_OPEN_TAB: menu_item (w, menu, "Open in New Tab", e->kind); break;
        case GBK_MENU_OPEN_WINDOW: menu_item (w, menu, "Open in New Window", e->kind); break;
        case GBK_MENU_OPEN_WITH: add_open_with (w, menu, gbk_menu_common_mime (items)); break;
        case GBK_MENU_EXECUTE: menu_item (w, menu, "Execute", e->kind); break;
        case GBK_MENU_EXECUTE_TERMINAL: menu_item (w, menu, "Execute in Terminal", e->kind); break;
        case GBK_MENU_TERMINAL_HERE: menu_item (w, menu, "Open Terminal Here", e->kind); break;
        case GBK_MENU_ACTION:
          {
            GtkWidget *mi = menu_item (w, menu, e->action->name, e->kind);
            g_object_set_data (G_OBJECT (mi), KEY_ACTION, e->action);
            break;
          }
        case GBK_MENU_SORT_NAME:
          check_item (w, menu, "Sort by Name", e->kind, w->sort == GBK_SORT_NAME, true);
          break;
        case GBK_MENU_SORT_MTIME:
          check_item (w, menu, "Sort by Date Modified", e->kind, w->sort == GBK_SORT_MTIME, true);
          break;
        case GBK_MENU_SORT_SIZE:
          check_item (w, menu, "Sort by Size", e->kind, w->sort == GBK_SORT_SIZE, true);
          break;
        case GBK_MENU_SHOW_HIDDEN:
          check_item (w, menu, "Show Hidden Files", e->kind, w->show_hidden, false);
          break;
        case GBK_MENU_CUT: menu_item (w, menu, "Cut", e->kind); break;
        case GBK_MENU_COPY: menu_item (w, menu, "Copy", e->kind); break;
        case GBK_MENU_COPY_PATH: menu_item (w, menu, "Copy Full Path", e->kind); break;
        case GBK_MENU_PASTE: menu_item (w, menu, "Paste", e->kind); break;
        case GBK_MENU_PASTE_INTO: menu_item (w, menu, "Paste Into Folder", e->kind); break;
        case GBK_MENU_TRASH: menu_item (w, menu, "Move to Trash", e->kind); break;
        case GBK_MENU_DELETE: menu_item (w, menu, "Delete", e->kind); break;
        case GBK_MENU_RENAME: menu_item (w, menu, "Rename…", e->kind); break;
        case GBK_MENU_EXECUTABLE:
          check_item (w, menu, "Executable", e->kind, all_executable (items), false);
          break;
        case GBK_MENU_PROPERTIES: menu_item (w, menu, "Properties…", e->kind); break;
        case GBK_MENU_CREATE_FOLDER: menu_item (w, menu, "Create Folder…", e->kind); break;
        case GBK_MENU_CREATE_FILE: menu_item (w, menu, "Create File…", e->kind); break;
        default: gbk_fatal ("bad menu entry kind %d", (int) e->kind);
        }
    }

  gtk_menu_attach_to_widget (GTK_MENU (menu), attach, nullptr);
  gbk_connect (G_OBJECT (menu), menu, "deactivate", G_CALLBACK (on_menu_deactivate), nullptr);
  /* A GtkMenu is its own toplevel: destroying the widget it's attached to doesn't destroy
   * it. Tie it to that widget, or closing the window with a menu open leaks the menu. And to
   * its tab: KEY_BROWSER is borrowed, and `attach` (the status bar) can outlive the tab. */
  gbk_connect (G_OBJECT (menu), attach, "destroy", G_CALLBACK (on_attach_destroyed), menu);
  gbk_connect (G_OBJECT (menu), b, "destroy", G_CALLBACK (on_attach_destroyed), menu);
  gtk_widget_show_all (menu);
  /* Keyboard-triggered menus come without an event; GTK needs one (for the grab), and the
   * key press being handled is it. */
  g_autoptr (GdkEvent) current = event == nullptr ? gtk_get_current_event () : nullptr;
  GdkEvent *trigger = event != nullptr ? event : current;
  if (trigger != nullptr && trigger->type == GDK_BUTTON_PRESS)
    gtk_menu_popup_at_pointer (GTK_MENU (menu), trigger);
  else
    gtk_menu_popup_at_widget (GTK_MENU (menu), attach, GDK_GRAVITY_CENTER, GDK_GRAVITY_NORTH_WEST,
                              trigger);
}

/* The folder's own menu, as for a click on empty space. */
static void
popup_background_menu (GbkWindow *w, GbkBrowser *b, GtkWidget *attach, GdkEvent *event)
{
  g_autoptr (GPtrArray) items = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  g_ptr_array_add (items, gbk_item_new_dir (gbk_browser_loc (b)));
  popup_menu (w, b, items, GBK_MENU_ON_BACKGROUND, attach, event);
}

static void
on_context_menu (GbkView *v, GdkEvent *event, GbkWindow *w)
{
  GbkBrowser *b = browser_of (v);
  g_autoptr (GPtrArray) items = snapshot_selection (w, b);
  if (items->len == 0)
    popup_background_menu (w, b, GTK_WIDGET (v), event);
  else
    popup_menu (w, b, items, GBK_MENU_ON_ITEMS, GTK_WIDGET (v), event);
}

/* Right-click on the status bar: the active tab's folder menu. */
static gboolean
on_status_bar_press (GtkWidget *bar, GdkEventButton *ev, GbkWindow *w)
{
  if (!gdk_event_triggers_context_menu ((GdkEvent *) ev))
    return false;
  popup_background_menu (w, active (w), bar, (GdkEvent *) ev);
  return true;
}

/* ---- drops: always ask ---- */

/* Dropped files are never moved or copied without asking: an accidental drop onto a
 * folder is the classic way files vanish into the wrong place. The menu sits where they
 * were dropped; Escape, clicking elsewhere or Cancel does nothing. */
#define KEY_DROP_LOCS "gbk-drop-locs"
#define KEY_DROP_DEST "gbk-drop-dest"
#define KEY_DROP_MOVE "gbk-drop-move"

static void
on_drop_chosen (GtkMenuItem *mi, GbkWindow *w)
{
  GtkWidget *menu = gtk_widget_get_parent (GTK_WIDGET (mi));
  const GPtrArray *locs = g_object_get_data (G_OBJECT (menu), KEY_DROP_LOCS);
  GbkLoc *dest = g_object_get_data (G_OBJECT (menu), KEY_DROP_DEST);
  bool move = g_object_get_data (G_OBJECT (mi), KEY_DROP_MOVE) != nullptr;
  g_autoptr (GbkJob) job = gbk_job_new (move ? GBK_OP_MOVE : GBK_OP_COPY, locs, dest);
  start_job (w, job);
}

static bool
all_in (const GPtrArray *locs, const GbkLoc *dir)
{
  for (guint i = 0; i < locs->len; i++)
    if (!gbk_loc_is_child_of (g_ptr_array_index (locs, i), dir))
      return false;
  return true;
}

static void
ask_drop (GbkWindow *w, GbkBrowser *b, GPtrArray *locs, GbkLoc *dest, GtkWidget *attach,
          GdkEvent *trigger)
{
  if (all_in (locs, dest))
    {
      gbk_browser_set_message (b, "Already in this folder");
      return;
    }
  if (refused_remote (b, GBK_MENU_PASTE, true))
    return;
  GtkWidget *menu = gtk_menu_new ();
  g_object_set_data_full (G_OBJECT (menu), KEY_DROP_LOCS, g_ptr_array_ref (locs),
                          (GDestroyNotify) g_ptr_array_unref);
  g_object_set_data_full (G_OBJECT (menu), KEY_DROP_DEST, gbk_loc_ref (dest),
                          (GDestroyNotify) gbk_loc_unref);
  GtkWidget *move = gtk_menu_item_new_with_label ("Move Here");
  g_object_set_data (G_OBJECT (move), KEY_DROP_MOVE, GINT_TO_POINTER (1));
  GtkWidget *copy = gtk_menu_item_new_with_label ("Copy Here");
  gbk_connect (G_OBJECT (w), move, "activate", G_CALLBACK (on_drop_chosen), w);
  gbk_connect (G_OBJECT (w), copy, "activate", G_CALLBACK (on_drop_chosen), w);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), move);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), copy);
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
  gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_menu_item_new_with_label ("Cancel"));

  gtk_menu_attach_to_widget (GTK_MENU (menu), attach, nullptr);
  gbk_connect (G_OBJECT (menu), menu, "deactivate", G_CALLBACK (on_menu_deactivate), nullptr);
  gbk_connect (G_OBJECT (menu), attach, "destroy", G_CALLBACK (on_attach_destroyed), menu);
  gbk_connect (G_OBJECT (menu), b, "destroy", G_CALLBACK (on_attach_destroyed), menu);
  gtk_widget_show_all (menu);
  /* At the pointer, which is where the files were dropped. */
  g_autoptr (GdkEvent) current = trigger == nullptr ? gtk_get_current_event () : nullptr;
  gtk_menu_popup_at_pointer (GTK_MENU (menu), trigger != nullptr ? trigger : current);
}

static void
on_view_drop (GbkView *v, GPtrArray *locs, guint64 folder, GdkEvent *trigger, GbkWindow *w)
{
  GbkBrowser *b = browser_of (v);
  GbkDirModel *m = gbk_browser_model (b);
  g_autoptr (GbkLoc) dest = nullptr;
  if (folder == GBK_HANDLE_NONE)
    dest = gbk_loc_ref (gbk_browser_loc (b));
  else
    {
      const GbkEntry *e = m != nullptr ? gbk_dir_model_get (m, folder) : nullptr;
      if (e == nullptr)
        return;                          /* the folder went away mid-drop */
      dest = gbk_loc_child (gbk_browser_loc (b), gbk_entry_name (e));
    }
  ask_drop (w, b, locs, dest, GTK_WIDGET (v), trigger);
}

static void
on_pathbar_drop (GbkPathbar *bar, GbkLoc *loc, GPtrArray *locs, GdkEvent *trigger, GbkWindow *w)
{
  GtkWidget *b = gtk_widget_get_ancestor (GTK_WIDGET (bar), GBK_TYPE_BROWSER);
  if (b == nullptr)
    gbk_fatal ("path bar outside a browser");
  ask_drop (w, GBK_BROWSER (b), locs, loc, GTK_WIDGET (bar), trigger);
}

/* Path bar buttons are folders: the current one and its ancestors (and, after going up,
 * the deeper ones still shown). */
static void
on_pathbar_menu (GbkPathbar *bar, GbkLoc *loc, GdkEvent *event, GbkWindow *w)
{
  GtkWidget *b = gtk_widget_get_ancestor (GTK_WIDGET (bar), GBK_TYPE_BROWSER);
  if (b == nullptr)
    gbk_fatal ("path bar outside a browser");
  g_autoptr (GPtrArray) items = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  g_ptr_array_add (items, gbk_item_new_dir (loc));
  popup_menu (w, GBK_BROWSER (b), items, GBK_MENU_ON_PATHBAR, GTK_WIDGET (bar), event);
}

/* ---- actions ---- */

#define ACTION(name) \
  static void name (GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, void *data)

ACTION (action_back)
{
  gbk_browser_back (active (data));
}
ACTION (action_forward)
{
  gbk_browser_forward (active (data));
}
ACTION (action_up)
{
  gbk_browser_up (active (data));
}
ACTION (action_home)
{
  gbk_browser_home (active (data));    /* the home of the machine being looked at */
}

/* ---- devices ---- */

/* A device's folders are about to go away (our unmount, or another program's): tabs
 * showing one move to the folder that holds the mount point, letting go of the device. */
static void
on_pre_unmount (GbkDevices *d G_GNUC_UNUSED, const char *root, GbkWindow *w)
{
  g_autoptr (GbkLoc) mount = gbk_loc_new_local (root);
  g_autoptr (GbkLoc) outside = gbk_loc_parent (mount);
  for (int i = 0; i < gtk_notebook_get_n_pages (w->tabs); i++)
    {
      GbkBrowser *b = GBK_BROWSER (gtk_notebook_get_nth_page (w->tabs, i));
      if (gbk_loc_is_under (gbk_browser_loc (b), mount))
        gbk_browser_open (b, outside);
    }
}

static bool
start_devices (void *data)
{
  GbkWindow *w = data;
  gbk_devices_start (w->devices);
  return false;
}

static bool
error_shown (const GError *error)
{
  /* Already told (GtkMountOperation's own dialog), or the password prompt was cancelled. */
  return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)
         || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
device_opened (GObject *owner, const char *path, const GError *error)
{
  GbkWindow *w = GBK_WINDOW (owner);
  if (error == nullptr)
    {
      g_autoptr (GbkLoc) loc = gbk_loc_new_local (path);
      gbk_browser_open (active (w), loc);
    }
  else if (!error_shown (error))
    gbk_jobui_error (GTK_WINDOW (w), "Couldn't open the device", error->message);
}

static void
device_removed (GObject *owner, const char *path G_GNUC_UNUSED, const GError *error)
{
  if (error != nullptr && !error_shown (error))
    gbk_jobui_error (GTK_WINDOW (owner), "Couldn't remove the device", error->message);
}

typedef bool (*DeviceOp) (GbkDevices *d, const char *id, GMountOperation *op, GObject *owner,
                          GbkDeviceDone done);

/* GTK 3's GtkMountOperation holds a reference to its parent window, and if the window is
 * destroyed first its own "destroy" handler forgets the pointer without dropping the
 * reference: the window (and everything it holds) is never freed. Detaching the parent
 * ourselves releases it properly; connected before set_parent, this runs before GTK's. */
static void
on_mount_op_parent_destroyed (GtkWidget *parent G_GNUC_UNUSED, GtkMountOperation *op)
{
  gtk_mount_operation_set_parent (op, nullptr);
}

/* Ids that name no device (an item from a list that has since changed, or a stray D-Bus
 * activation) do nothing. */
static void
device_action (GbkWindow *w, GVariant *param, DeviceOp fn, GbkDeviceDone done)
{
  g_autoptr (GMountOperation) op = gtk_mount_operation_new (nullptr);
  gbk_connect (G_OBJECT (op), w, "destroy", G_CALLBACK (on_mount_op_parent_destroyed), op);
  gtk_mount_operation_set_parent (GTK_MOUNT_OPERATION (op), GTK_WINDOW (w));
  (void) fn (w->devices, g_variant_get_string (param, nullptr), op, G_OBJECT (w), done);
}

static void
action_device_open (GSimpleAction *a G_GNUC_UNUSED, GVariant *param, void *data)
{
  device_action (data, param, gbk_devices_open, device_opened);
}

static void
action_device_unmount (GSimpleAction *a G_GNUC_UNUSED, GVariant *param, void *data)
{
  device_action (data, param, gbk_devices_unmount, device_removed);
}

static void
action_device_eject (GSimpleAction *a G_GNUC_UNUSED, GVariant *param, void *data)
{
  device_action (data, param, gbk_devices_eject, device_removed);
}

ACTION (action_terminal)
{
  GbkWindow *w = data;
  GbkBrowser *b = active (w);
  g_autoptr (GError) error = nullptr;
  if (!gbk_open_terminal (w->ctx->config, gbk_browser_loc (b), &error))
    report (b, "Terminal", error);
}

ACTION (action_copy)
{
  if (refused_remote (active (data), GBK_MENU_COPY, true))
    return;
  g_autoptr (GPtrArray) items = snapshot_selection (data, active (data));
  copy_items (data, active (data), items, false);
}

ACTION (action_cut)
{
  if (refused_remote (active (data), GBK_MENU_CUT, true))
    return;
  g_autoptr (GPtrArray) items = snapshot_selection (data, active (data));
  copy_items (data, active (data), items, true);
}

ACTION (action_paste)
{
  if (refused_remote (active (data), GBK_MENU_PASTE, true))
    return;
  paste_into (data, gbk_browser_loc (active (data)));
}

ACTION (action_trash)
{
  if (refused_remote (active (data), GBK_MENU_TRASH, true))
    return;
  g_autoptr (GPtrArray) items = snapshot_selection (data, active (data));
  trash_items (data, items);
}

ACTION (action_delete)
{
  if (refused_remote (active (data), GBK_MENU_DELETE, true))
    return;
  g_autoptr (GPtrArray) items = snapshot_selection (data, active (data));
  delete_items (data, items);
}

ACTION (action_rename)
{
  if (refused_remote (active (data), GBK_MENU_RENAME, true))
    return;
  g_autoptr (GPtrArray) items = snapshot_selection (data, active (data));
  if (items->len == 1)
    rename_item (data, g_ptr_array_index (items, 0));
}

/* Alt+Enter: the one selected item, or the folder itself when nothing is selected. */
ACTION (action_properties)
{
  GbkWindow *w = data;
  GbkBrowser *b = active (w);
  g_autoptr (GPtrArray) items = snapshot_selection (w, b);
  if (items->len == 0)
    g_ptr_array_add (items, gbk_item_new_dir (gbk_browser_loc (b)));
  if (items->len == 1)
    show_properties (w, g_ptr_array_index (items, 0));
  else
    gbk_browser_set_message (b, "Properties: select one item");
}

ACTION (action_new_tab)
{
  GbkWindow *w = data;
  add_tab (w, gbk_browser_loc (active (w)), true);
}

ACTION (action_close_tab)
{
  GbkWindow *w = data;
  close_tab (w, active (w));
}

ACTION (action_close_window)
{
  gtk_window_close (GTK_WINDOW (data));
}

ACTION (action_new_window)
{
  GbkWindow *w = data;
  GbkWindow *n = gbk_window_new (gtk_window_get_application (GTK_WINDOW (w)), w->ctx,
                                 gbk_browser_loc (active (w)));
  gtk_widget_show_all (GTK_WIDGET (n));
}

ACTION (action_next_tab)
{
  gtk_notebook_next_page (((GbkWindow *) data)->tabs);
}
ACTION (action_prev_tab)
{
  gtk_notebook_prev_page (((GbkWindow *) data)->tabs);
}
static void
action_network (GSimpleAction *a G_GNUC_UNUSED, GVariant *param, void *data)
{
  /* Reachable from outside the process (D-Bus): a name ssh would misread does nothing. */
  const char *host = g_variant_get_string (param, nullptr);
  if (!gbk_host_valid (host))
    return;
  g_autofree char *h = g_strdup (host);
  char empty[] = "";
  GbkAddress home = { h, GBK_ADDRESS_HOME, empty };
  gbk_browser_open_address (active (data), &home);
}

ACTION (action_location)
{
  gbk_pathbar_edit (gbk_browser_pathbar (active (data)));
}

static void
change_show_hidden (GSimpleAction *a G_GNUC_UNUSED, GVariant *state, void *data)
{
  set_show_hidden (data, g_variant_get_boolean (state));
}

static void
change_sort (GSimpleAction *a G_GNUC_UNUSED, GVariant *state, void *data)
{
  /* Actions are reachable from outside the process (D-Bus): ignore unknown values. */
  const char *want = g_variant_get_string (state, nullptr);
  for (size_t i = 0; i < G_N_ELEMENTS (sorts); i++)
    if (g_str_equal (want, sorts[i].state))
      set_sort (data, sorts[i].mode);
}

static const GActionEntry actions[] = {
  { .name = "back", .activate = action_back },
  { .name = "forward", .activate = action_forward },
  { .name = "up", .activate = action_up },
  { .name = "home", .activate = action_home },
  { .name = "terminal", .activate = action_terminal },
  { .name = "copy", .activate = action_copy },
  { .name = "cut", .activate = action_cut },
  { .name = "paste", .activate = action_paste },
  { .name = "trash", .activate = action_trash },
  { .name = "delete", .activate = action_delete },
  { .name = "rename", .activate = action_rename },
  { .name = "properties", .activate = action_properties },
  { .name = "new-tab", .activate = action_new_tab },
  { .name = "close-tab", .activate = action_close_tab },
  { .name = "close-window", .activate = action_close_window },
  { .name = "new-window", .activate = action_new_window },
  { .name = "next-tab", .activate = action_next_tab },
  { .name = "prev-tab", .activate = action_prev_tab },
  { .name = "location", .activate = action_location },
  { .name = "device-open", .parameter_type = "s", .activate = action_device_open },
  { .name = "network", .parameter_type = "s", .activate = action_network },
  { .name = "device-unmount", .parameter_type = "s", .activate = action_device_unmount },
  { .name = "device-eject", .parameter_type = "s", .activate = action_device_eject },
  /* Stateful: no activate handler means a boolean toggles / a string is set. */
  { .name = "show-hidden", .state = "false", .change_state = change_show_hidden },
  { .name = "sort", .parameter_type = "s", .state = "'name'", .change_state = change_sort },
};

/* ---- keys ---- */

static const struct
{
  const char *action;
  const char *accels[3];
} shortcuts[] = {
  { "win.back", { "<Alt>Left", nullptr } },
  { "win.forward", { "<Alt>Right", nullptr } },
  { "win.up", { "<Alt>Up", nullptr } },
  { "win.home", { "<Alt>Home", nullptr } },
  { "win.show-hidden", { "<Control>h", nullptr } },
  { "win.terminal", { "F4", nullptr } },
  { "win.close-tab", { "<Control>w", nullptr } },
  { "win.close-window", { "<Control>q", nullptr } },
  { "win.new-tab", { "<Control>t", nullptr } },
  { "win.new-window", { "<Control>n", nullptr } },
  { "win.next-tab", { "<Control>Page_Down", nullptr } },
  { "win.prev-tab", { "<Control>Page_Up", nullptr } },
  { "win.location", { "<Control>l", nullptr } },
  { "win.copy", { "<Control>c", nullptr } },
  { "win.cut", { "<Control>x", nullptr } },
  { "win.paste", { "<Control>v", nullptr } },
  { "win.trash", { "Delete", "KP_Delete", nullptr } },
  { "win.delete", { "<Shift>Delete", "<Shift>KP_Delete", nullptr } },
  { "win.rename", { "F2", nullptr } },
  { "win.properties", { "<Alt>Return", "<Alt>KP_Enter", nullptr } },
};

void
gbk_window_install_accels (GtkApplication *app)
{
  for (size_t i = 0; i < G_N_ELEMENTS (shortcuts); i++)
    gtk_application_set_accels_for_action (app, shortcuts[i].action, shortcuts[i].accels);
}

/* GTK runs window shortcuts before the focused widget sees a key. With a text entry focused
 * (the location bar), that would make Delete trash the selected files and Ctrl+V paste them
 * instead of editing the text. So while an editable has focus, keys go to it and window
 * shortcuts don't run at all, even for keys the entry ignores (F2 must not open a rename
 * dialog for the selected file while you type a path). Escape leaves the entry. */
/* Coming back to the window (after logging in in a terminal, after sleep): a folder whose
 * machine couldn't be reached tries again now. */
static gboolean
gbk_window_focus_in (GtkWidget *widget, GdkEventFocus *ev)
{
  GbkWindow *w = GBK_WINDOW (widget);
  /* Focus can come and go while the window is being built or torn down: only a window
   * with a tab in it has anything to retry. */
  if (!gtk_widget_in_destruction (widget) && w->tabs != nullptr
      && gtk_notebook_get_current_page (w->tabs) >= 0)
    gbk_browser_reconnect (active (w));
  return GTK_WIDGET_CLASS (gbk_window_parent_class)->focus_in_event (widget, ev);
}

static gboolean
gbk_window_key_press (GtkWidget *widget, GdkEventKey *ev)
{
  GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (widget));
  if (focus != nullptr && GTK_IS_EDITABLE (focus))
    {
      gtk_window_propagate_key_event (GTK_WINDOW (widget), ev);
      return true;
    }
  return GTK_WIDGET_CLASS (gbk_window_parent_class)->key_press_event (widget, ev);
}

/* ---- mouse back / forward buttons ---- */

static void
on_mouse_back (GtkGestureMultiPress *g, int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
               double y G_GNUC_UNUSED, GbkWindow *w)
{
  gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
  gbk_browser_back (active (w));
}

static void
on_mouse_forward (GtkGestureMultiPress *g, int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                  double y G_GNUC_UNUSED, GbkWindow *w)
{
  gtk_gesture_set_state (GTK_GESTURE (g), GTK_EVENT_SEQUENCE_CLAIMED);
  gbk_browser_forward (active (w));
}

/* X11 reports a mouse's back and forward side buttons as buttons 8 and 9. The capture
 * phase sees them first, whatever is under the pointer (files, path bar, status bar). */
static GtkGesture *
side_button (GbkWindow *w, guint button, GCallback fn)
{
  GtkGesture *g = gtk_gesture_multi_press_new (GTK_WIDGET (w));
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (g), button);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (g), GTK_PHASE_CAPTURE);
  gbk_connect (G_OBJECT (w), g, "pressed", fn, w);
  return g;
}

/* ---- GObject ---- */

static void
gbk_window_dispose (GObject *o)
{
  GbkWindow *w = GBK_WINDOW (o);
  /* Gestures aren't owned by their widget in GTK 3: release them while it's still whole. */
  g_clear_object (&w->mouse_back);
  g_clear_object (&w->mouse_forward);
  g_clear_object (&w->devices);
  G_OBJECT_CLASS (gbk_window_parent_class)->dispose (o);
}

static void
gbk_window_finalize (GObject *o)
{
  GbkWindow *w = GBK_WINDOW (o);
  g_clear_pointer (&w->paste_dest, gbk_loc_unref);
  gbk_context_unref (w->ctx);
  G_OBJECT_CLASS (gbk_window_parent_class)->finalize (o);
}

static void
gbk_window_class_init (GbkWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = gbk_window_dispose;
  G_OBJECT_CLASS (klass)->finalize = gbk_window_finalize;
  GTK_WIDGET_CLASS (klass)->key_press_event = gbk_window_key_press;
  GTK_WIDGET_CLASS (klass)->focus_in_event = gbk_window_focus_in;
}

static void
gbk_window_init (GbkWindow *w G_GNUC_UNUSED)
{
}

/* Machines to open: the hosts in ~/.ssh/config, then any this process has connected to.
 * Each opens its home folder in the current tab. */
static GMenuModel *
network_menu (GbkWindow *w)
{
  GMenu *menu = g_menu_new ();
  g_autofree char *path = g_build_filename (g_get_home_dir (), ".ssh", "config", nullptr);
  g_autofree char *text = nullptr;
  gsize len = 0;
  g_autoptr (GPtrArray) hosts = g_file_get_contents (path, &text, &len, nullptr)
                                    ? gbk_ssh_config_hosts (text, len)
                                    : g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) used = gbk_remote_hosts (w->ctx->remote);
  for (guint i = 0; i < used->len; i++)
    {
      const char *h = g_ptr_array_index (used, i);
      bool seen = false;
      for (guint k = 0; k < hosts->len && !seen; k++)
        seen = g_str_equal (g_ptr_array_index (hosts, k), h);
      if (!seen)
        g_ptr_array_add (hosts, g_strdup (h));
    }
  for (guint i = 0; i < hosts->len; i++)
    {
      const char *h = g_ptr_array_index (hosts, i);
      g_autoptr (GMenuItem) item = g_menu_item_new (h, nullptr);
      g_menu_item_set_action_and_target_value (item, "win.network", g_variant_new_string (h));
      g_menu_append_item (menu, item);
    }
  if (hosts->len == 0)
    {
      g_autoptr (GMenuItem) none
          = g_menu_item_new ("Type host:folder in the location bar", nullptr);
      g_menu_item_set_action_and_target_value (none, "win.location", nullptr);
      g_menu_append_item (menu, none);
    }
  return G_MENU_MODEL (menu);
}

static GtkWidget *
app_menu_button (GbkWindow *w)
{
  g_autoptr (GMenu) menu = g_menu_new ();
  g_autoptr (GMenu) s1 = g_menu_new ();
  g_menu_append (s1, "New Tab", "win.new-tab");
  g_menu_append (s1, "New Window", "win.new-window");
  g_menu_append_submenu (s1, "Devices", gbk_devices_menu (w->devices));
  g_autoptr (GMenuModel) network = network_menu (w);
  g_menu_append_submenu (s1, "Network", network);
  g_menu_append_section (menu, nullptr, G_MENU_MODEL (s1));
  g_autoptr (GMenu) s2 = g_menu_new ();
  g_menu_append (s2, "Show Hidden Files", "win.show-hidden");
  g_menu_append (s2, "Sort by Name", "win.sort::name");
  g_menu_append (s2, "Sort by Date Modified", "win.sort::modified");
  g_menu_append (s2, "Sort by Size", "win.sort::size");
  g_menu_append_section (menu, nullptr, G_MENU_MODEL (s2));
  g_autoptr (GMenu) s3 = g_menu_new ();
  g_menu_append (s3, "Open Terminal Here", "win.terminal");
  g_menu_append (s3, "Open Location…", "win.location");
  g_menu_append_section (menu, nullptr, G_MENU_MODEL (s3));
  g_autoptr (GMenu) s4 = g_menu_new ();
  g_menu_append (s4, "Close Tab", "win.close-tab");
  g_menu_append (s4, "Close Window", "win.close-window");
  g_menu_append_section (menu, nullptr, G_MENU_MODEL (s4));

  GtkWidget *button = gtk_menu_button_new ();
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (button), G_MENU_MODEL (menu));
  gtk_menu_button_set_use_popover (GTK_MENU_BUTTON (button), false);
  gtk_container_add (GTK_CONTAINER (button),
                     gtk_image_new_from_icon_name ("open-menu-symbolic", GTK_ICON_SIZE_MENU));
  gtk_widget_set_focus_on_click (button, false);
  return button;
}

GbkWindow *
gbk_window_new (const GtkApplication *app, GbkContext *ctx, GbkLoc *loc)
{
  GbkWindow *w = g_object_new (GBK_TYPE_WINDOW, "application", app, nullptr);
  w->ctx = gbk_context_ref (ctx);
  w->show_hidden = ctx->config->show_hidden;
  w->sort = ctx->config->sort;
  g_action_map_add_action_entries (G_ACTION_MAP (w), actions, G_N_ELEMENTS (actions), w);
  w->devices = gbk_devices_new ();
  gbk_connect (G_OBJECT (w), w->devices, "pre-unmount", G_CALLBACK (on_pre_unmount), w);
  gtk_window_set_default_size (GTK_WINDOW (w), 700, 440);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

  /* Tabs on top: everything below the tab strip, path bar included, belongs to the tab. */
  w->tabs = GTK_NOTEBOOK (gtk_notebook_new ());
  gtk_notebook_set_scrollable (w->tabs, true);
  gtk_notebook_set_show_border (w->tabs, false);
  gtk_box_pack_start (GTK_BOX (box), GTK_WIDGET (w->tabs), true, true, 0);

  GtkWidget *status_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start (status_bar, 6);
  gtk_widget_set_margin_end (status_bar, 6);
  gtk_widget_set_margin_top (status_bar, 2);
  gtk_widget_set_margin_bottom (status_bar, 2);
  w->status = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (w->status, 0);
  gtk_label_set_ellipsize (w->status, PANGO_ELLIPSIZE_END);
  gtk_box_pack_start (GTK_BOX (status_bar), GTK_WIDGET (w->status), true, true, 0);

  /* Right: the sort and hidden-files modes, as small buttons that change them. They never
   * take focus, so the keyboard stays on the files. */
  g_autoptr (GtkCssProvider) css = gtk_css_provider_new ();
  g_autoptr (GError) css_error = nullptr;
  if (!gtk_css_provider_load_from_data (css, "button { padding: 0 6px; min-height: 0; }", -1,
                                        &css_error))
    gbk_fatal ("status bar CSS: %s", css_error->message);
  GtkWidget *modes = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  w->sort_button = GTK_BUTTON (gtk_button_new_with_label (""));
  w->hidden_button = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label (".*"));
  gtk_widget_set_tooltip_text (GTK_WIDGET (w->hidden_button), "Show hidden files (Ctrl+H)");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (w->hidden_button), "win.show-hidden");
  GtkWidget *mode_buttons[] = { GTK_WIDGET (w->sort_button), GTK_WIDGET (w->hidden_button) };
  for (size_t i = 0; i < G_N_ELEMENTS (mode_buttons); i++)
    {
      gtk_widget_set_focus_on_click (mode_buttons[i], false);
      gtk_widget_set_can_focus (mode_buttons[i], false);
      gtk_widget_set_valign (mode_buttons[i], GTK_ALIGN_CENTER);
      gtk_style_context_add_provider (gtk_widget_get_style_context (mode_buttons[i]),
                                      GTK_STYLE_PROVIDER (css),
                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      gtk_box_pack_start (GTK_BOX (modes), mode_buttons[i], false, false, 0);
    }
  gbk_connect (G_OBJECT (w), w->sort_button, "clicked", G_CALLBACK (on_sort_button_clicked), w);
  gtk_box_pack_end (GTK_BOX (status_bar), modes, false, false, 0);
  w->status_bar = gtk_event_box_new ();
  gtk_container_add (GTK_CONTAINER (w->status_bar), status_bar);
  gbk_connect (G_OBJECT (w), w->status_bar, "button-press-event", G_CALLBACK (on_status_bar_press),
               w);
  gtk_box_pack_start (GTK_BOX (box), w->status_bar, false, false, 0);
  gtk_container_add (GTK_CONTAINER (w), box);

  gbk_connect (G_OBJECT (w), w->tabs, "switch-page", G_CALLBACK (on_switch_page), w);
  w->mouse_back = side_button (w, 8, G_CALLBACK (on_mouse_back));
  w->mouse_forward = side_button (w, 9, G_CALLBACK (on_mouse_forward));
  gbk_connect (G_OBJECT (w), w->tabs, "page-removed", G_CALLBACK (on_page_removed), w);

  set_show_hidden (w, w->show_hidden);
  set_sort (w, w->sort);
  add_tab (w, loc, true);
  update_chrome (w);
  /* The device list costs a blocking D-Bus round trip or two: not before the first folder. */
  gbk_timeout_add (G_OBJECT (w), GBK_WINDOW_DEVICES_DELAY_MS, start_devices, w);
  return w;
}

void
gbk_window_open (GbkWindow *w, GbkLoc *loc)
{
  gbk_browser_open (active (w), loc);
}

void
gbk_window_open_address (GbkWindow *w, const GbkAddress *a)
{
  gbk_browser_open_address (active (w), a);
}

void
gbk_window_new_tab (GbkWindow *w, GbkLoc *loc)
{
  add_tab (w, loc, true);
}

GbkLoc *
gbk_window_loc (GbkWindow *w)
{
  return gbk_browser_loc (active (w));
}

GbkLoc *
gbk_window_tab_loc (GbkWindow *w, int i)
{
  GtkWidget *b = gtk_notebook_get_nth_page (w->tabs, i);
  if (b == nullptr)
    gbk_fatal ("no tab %d", i);
  return gbk_browser_loc (GBK_BROWSER (b));
}

GbkDevices *
gbk_window_devices (const GbkWindow *w)
{
  return w->devices;
}

GbkView *
gbk_window_view (GbkWindow *w)
{
  return gbk_browser_view (active (w));
}

const char *
gbk_window_status (GbkWindow *w)
{
  return gtk_label_get_text (w->status);
}

GtkWidget *
gbk_window_status_bar (const GbkWindow *w)
{
  return w->status_bar;
}

GtkButton *
gbk_window_sort_button (const GbkWindow *w)
{
  return w->sort_button;
}

GtkToggleButton *
gbk_window_hidden_button (const GbkWindow *w)
{
  return w->hidden_button;
}

int
gbk_window_n_tabs (GbkWindow *w)
{
  return gtk_notebook_get_n_pages (w->tabs);
}

GbkPathbar *
gbk_window_pathbar (GbkWindow *w)
{
  return gbk_browser_pathbar (active (w));
}
