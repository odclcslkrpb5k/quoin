/* Compact view widget: layout, selection, keyboard, mouse, type-ahead (under Xvfb). */
#include "lifetime.h"
#include "view.h"

static GbkTypeTable *types;

typedef struct
{
  GtkWidget *window;   /* offscreen */
  GbkView *view;
  GbkDirModel *model;
  int activations;
} Fixture;

static void
flush (void)
{
  while (gtk_events_pending ())
    gtk_main_iteration ();
}

static GbkEntry *
mk (const char *name, GbkFileKind kind)
{
  GbkEntryInfo info = { .kind = kind, .mode = 0644, .size = 1234 };
  GbkEntry *e = gbk_entry_new (name, strlen (name), &info, types);
  if (e == nullptr)
    gbk_fatal ("bad test name %s", name);
  return e;
}

static GbkScanMsg *
msg (GbkScanMsgType type)
{
  GbkScanMsg *m = g_new0 (GbkScanMsg, 1);
  m->type = type;
  m->upserts = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_entry_free);
  m->removes = g_ptr_array_new_with_free_func (g_free);
  return m;
}

static void
on_activate (GbkView *v G_GNUC_UNUSED, Fixture *f)
{
  f->activations++;
}

/* Model: 3 dirs + n files ("file00".."fileNN") + an archive and a long name. */
static void
setup (Fixture *f, int n_files, int width, int height)
{
  f->model = gbk_dir_model_new_detached (types);
  GbkScanMsg *s = msg (GBK_SCAN_CHUNK);
  s->first = s->last = true;
  s->has_free = true;
  s->free_bytes = 42ull << 30;
  for (int i = 0; i < 3; i++)
    {
      g_autofree char *name = g_strdup_printf ("Dir %c", 'A' + i);
      g_ptr_array_add (s->upserts, mk (name, GBK_KIND_DIRECTORY));
    }
  for (int i = 0; i < n_files; i++)
    {
      g_autofree char *name = g_strdup_printf ("file%02d.txt", i);
      g_ptr_array_add (s->upserts, mk (name, GBK_KIND_REGULAR));
    }
  g_ptr_array_add (s->upserts, mk ("archive.zip", GBK_KIND_REGULAR));
  g_ptr_array_add (s->upserts,
                   mk ("zz a very long file name that should be ellipsized in the middle "
                       "because it is far too wide for any sensible column.txt",
                       GBK_KIND_REGULAR));
  g_ptr_array_add (s->upserts, mk ("ünïcödé näme.txt", GBK_KIND_REGULAR));
  gbk_dir_model_apply_for_test (f->model, s);

  f->window = gtk_offscreen_window_new ();
  gtk_window_set_default_size (GTK_WINDOW (f->window), width, height);
  GtkWidget *scroller = gtk_scrolled_window_new (nullptr, nullptr);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_AUTOMATIC,
                                  GTK_POLICY_NEVER);
  f->view = GBK_VIEW (gbk_view_new (types));
  gtk_container_add (GTK_CONTAINER (scroller), GTK_WIDGET (f->view));
  gtk_container_add (GTK_CONTAINER (f->window), scroller);
  gbk_view_set_model (f->view, f->model);
  gbk_connect (G_OBJECT (f->window), f->view, "activate", G_CALLBACK (on_activate), f);
  gtk_widget_show_all (f->window);
  gtk_widget_grab_focus (GTK_WIDGET (f->view));
  flush ();
}

static void
teardown (Fixture *f, const void *data G_GNUC_UNUSED)
{
  gtk_widget_destroy (f->window);
  g_object_unref (f->model);
  flush ();
}

static size_t
idx (Fixture *f, GbkHandle h)
{
  return gbk_dir_model_visible_index (f->model, h);
}

static const char *
name_at (Fixture *f, size_t i)
{
  return gbk_entry_name (gbk_dir_model_get (f->model, gbk_dir_model_visible_handle (f->model, i)));
}

static const char *
cursor_name (Fixture *f)
{
  const GbkEntry *e = gbk_dir_model_get (f->model, gbk_view_cursor (f->view));
  return e != nullptr ? gbk_entry_name (e) : "(none)";
}

/* Real GDK events through gtk_widget_event, like the main loop would deliver them. */
static bool
send_key (Fixture *f, guint keyval, GdkModifierType state)
{
  GdkEvent *ev = gdk_event_new (GDK_KEY_PRESS);
  ev->key.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (f->view)));
  ev->key.keyval = keyval;
  ev->key.state = state;
  gdk_event_set_device (ev, gdk_seat_get_keyboard (gdk_display_get_default_seat (
                                gtk_widget_get_display (GTK_WIDGET (f->view)))));
  bool handled = gtk_widget_event (GTK_WIDGET (f->view), ev);
  gdk_event_free (ev);
  flush ();
  return handled;
}

/* A point inside the cell of the item at visible `index`. */
static void
item_point (Fixture *f, size_t index, double *x_out, double *y_out)
{
  /* Column = index / rows, row = index % rows. Columns may differ in width, so find x by
   * probing. */
  size_t rows = gbk_view_rows_per_column (f->view);
  GbkHandle want = gbk_dir_model_visible_handle (f->model, index);
  int row_h = gtk_widget_get_allocated_height (GTK_WIDGET (f->view)) / (int) rows;
  double y = (double) (index % rows) * row_h + 2;
  double x = -1;
  for (int px = 0; px < gtk_widget_get_allocated_width (GTK_WIDGET (f->view)); px += 2)
    if (gbk_view_item_at (f->view, px, y) == want)
      {
        x = px + 2;
        break;
      }
  if (x < 0)
    gbk_fatal ("item %zu not on screen", index);
  *x_out = x;
  *y_out = y;
}

static void
send_click (Fixture *f, size_t index, GdkModifierType state, GdkEventType type, guint button)
{
  double x, y;
  item_point (f, index, &x, &y);

  GdkEvent *ev = gdk_event_new (type);
  ev->button.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (f->view)));
  ev->button.x = x;
  ev->button.y = y;
  ev->button.button = button;
  ev->button.state = state;
  gdk_event_set_device (ev, gdk_seat_get_pointer (gdk_display_get_default_seat (
                                gtk_widget_get_display (GTK_WIDGET (f->view)))));
  gtk_widget_event (GTK_WIDGET (f->view), ev);
  gdk_event_free (ev);
  flush ();
}

static void
test_layout (Fixture *f, const void *data G_GNUC_UNUSED)
{
  size_t rows = gbk_view_rows_per_column (f->view);
  g_assert_cmpuint (rows, >=, 3);
  /* Dirs first, then natural order. */
  g_assert_cmpstr (name_at (f, 0), ==, "Dir A");
  g_assert_cmpstr (name_at (f, 3), ==, "archive.zip");
  g_assert_cmpuint (gbk_view_item_at (f->view, 5, 2), ==,
                    gbk_dir_model_visible_handle (f->model, 0));
  g_assert_cmpuint (gbk_view_item_at (f->view, 5, 2 + (double) (rows - 1) * 20), !=,
                    GBK_HANDLE_NONE);
  g_assert_cmpuint (gbk_view_item_at (f->view, -1, 2), ==, GBK_HANDLE_NONE);
  g_assert_cmpuint (gbk_view_item_at (f->view, 5, 100000), ==, GBK_HANDLE_NONE);
}

/* Width caches are per model: slot numbers restart in each one, so a new folder must not
 * inherit the widths measured for the previous folder's names ("movies" drawn as "m…s"). */
static void
test_widths_follow_model (Fixture *f, const void *data G_GNUC_UNUSED)
{
  const char *long_name = "movies-and-other-long-named-things";
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  GbkScanMsg *s = msg (GBK_SCAN_CHUNK);
  s->first = s->last = true;
  g_ptr_array_add (s->upserts, mk (long_name, GBK_KIND_REGULAR));
  gbk_dir_model_apply_for_test (m, s);
  /* Slot 0 of the fixture's model is "Dir A", whose width is cached by now. */
  gbk_view_set_model (f->view, m);
  flush ();
  GbkHandle only = gbk_dir_model_visible_handle (m, 0);
  /* Its column is as wide as its name: an x well past "Dir A"'s width still hits it. */
  int name_w = 0, h;
  PangoLayout *l = gtk_widget_create_pango_layout (GTK_WIDGET (f->view), long_name);
  pango_layout_get_pixel_size (l, &name_w, &h);
  g_object_unref (l);
  g_assert_cmpuint (gbk_view_item_at (f->view, 16 + name_w, 2), ==, only);
  gbk_view_set_model (f->view, f->model);
}

static void
test_keyboard (Fixture *f, const void *data G_GNUC_UNUSED)
{
  size_t rows = gbk_view_rows_per_column (f->view);
  size_t n = gbk_dir_model_n_visible (f->model);

  g_assert_true (send_key (f, GDK_KEY_Down, 0));          /* no cursor yet: first item */
  g_assert_cmpstr (cursor_name (f), ==, "Dir A");
  g_assert_true (send_key (f, GDK_KEY_Down, 0));
  g_assert_cmpstr (cursor_name (f), ==, "Dir B");
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 1);

  g_assert_true (send_key (f, GDK_KEY_Right, 0));         /* next column, same row */
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, 1 + rows);
  g_assert_true (send_key (f, GDK_KEY_Left, 0));
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, 1);

  g_assert_true (send_key (f, GDK_KEY_End, 0));
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, n - 1);
  g_assert_true (send_key (f, GDK_KEY_Right, 0));         /* clamps at the end */
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, n - 1);
  g_assert_true (send_key (f, GDK_KEY_Home, 0));
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, 0);

  /* Shift extends from the anchor. */
  send_key (f, GDK_KEY_Down, GDK_SHIFT_MASK);
  send_key (f, GDK_KEY_Down, GDK_SHIFT_MASK);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 3);

  /* Ctrl moves the cursor only; Space toggles. */
  send_key (f, GDK_KEY_Down, GDK_CONTROL_MASK);
  send_key (f, GDK_KEY_Down, GDK_CONTROL_MASK);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 3);
  g_assert_false (gbk_view_is_selected (f->view, gbk_view_cursor (f->view)));
  send_key (f, GDK_KEY_space, 0);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 4);
  send_key (f, GDK_KEY_space, 0);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 3);

  /* Ctrl+A, then Alt is left for the window. */
  send_key (f, GDK_KEY_a, GDK_CONTROL_MASK);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, n);
  g_assert_false (send_key (f, GDK_KEY_Up, GDK_MOD1_MASK));

  /* Enter activates. */
  send_key (f, GDK_KEY_Home, 0);
  send_key (f, GDK_KEY_Return, 0);
  g_assert_cmpint (f->activations, ==, 1);
}

static void
test_mouse (Fixture *f, const void *data G_GNUC_UNUSED)
{
  send_click (f, 4, 0, GDK_BUTTON_PRESS, 1);
  g_assert_cmpstr (cursor_name (f), ==, "file00.txt");
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 1);

  send_click (f, 6, GDK_CONTROL_MASK, GDK_BUTTON_PRESS, 1);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 2);
  send_click (f, 6, GDK_CONTROL_MASK, GDK_BUTTON_PRESS, 1);   /* toggles off */
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 1);

  /* Ctrl+click moved the anchor to 6 (as in Thunar/GtkTreeView): range is 6..8. */
  send_click (f, 8, GDK_SHIFT_MASK, GDK_BUTTON_PRESS, 1);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 3);
  /* Plain click sets the anchor: range 4..8. */
  send_click (f, 4, 0, GDK_BUTTON_PRESS, 1);
  send_click (f, 8, GDK_SHIFT_MASK, GDK_BUTTON_PRESS, 1);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 5);

  /* Right-click on an unselected item selects just it. */
  send_click (f, 1, 0, GDK_BUTTON_PRESS, 3);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 1);
  g_assert_cmpstr (cursor_name (f), ==, "Dir B");

  send_click (f, 2, 0, GDK_2BUTTON_PRESS, 1);
  g_assert_cmpint (f->activations, ==, 1);
}

/* Pressing on a selected item keeps the selection (it may be the start of dragging all of
 * it); releasing without a drag selects just that item. */
static void
test_press_on_selection (Fixture *f, const void *data G_GNUC_UNUSED)
{
  gbk_view_select_all (f->view);
  size_t all = gbk_view_n_selected (f->view);
  g_assert_cmpuint (all, >, 3);
  send_click (f, 4, 0, GDK_BUTTON_PRESS, 1);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, all);
  g_assert_cmpstr (cursor_name (f), ==, "file00.txt");
  send_click (f, 4, 0, GDK_BUTTON_RELEASE, 1);
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 1);
  g_assert_cmpstr (cursor_name (f), ==, "file00.txt");
  /* On an unselected item, the press selects at once. */
  send_click (f, 5, 0, GDK_BUTTON_PRESS, 1);
  g_assert_true (gbk_view_is_selected (f->view, gbk_dir_model_visible_handle (f->model, 5)));
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 1);
  send_click (f, 5, 0, GDK_BUTTON_RELEASE, 1);
}

/* Where drops land: on a folder item, into it; anywhere else, into the view's folder. A drag
 * from this view can't go into an item being dragged, or back where it came from. */
static void
test_drop_targets (Fixture *f, const void *data G_GNUC_UNUSED)
{
  GbkHandle dir_a = gbk_dir_model_visible_handle (f->model, 0);   /* "Dir A" */
  double ax, ay, fx, fy;
  item_point (f, 0, &ax, &ay);
  item_point (f, 4, &fx, &fy);                                   /* a file */
  GbkHandle folder;
  g_assert_true (gbk_view_drop_target_at (f->view, ax, ay, false, &folder));
  g_assert_cmpuint (folder, ==, dir_a);
  g_assert_true (gbk_view_drop_target_at (f->view, fx, fy, false, &folder));
  g_assert_cmpuint (folder, ==, GBK_HANDLE_NONE);

  gbk_view_select_only (f->view, dir_a);
  g_assert_false (gbk_view_drop_target_at (f->view, ax, ay, true, &folder));   /* into itself */
  g_assert_false (gbk_view_drop_target_at (f->view, fx, fy, true, &folder));   /* where it is */
  double bx, by;
  item_point (f, 1, &bx, &by);                                   /* "Dir B": fine */
  g_assert_true (gbk_view_drop_target_at (f->view, bx, by, true, &folder));
  g_assert_cmpuint (folder, ==, gbk_dir_model_visible_handle (f->model, 1));
}

static void
test_type_ahead (Fixture *f, const void *data G_GNUC_UNUSED)
{
  send_key (f, GDK_KEY_f, 0);
  g_assert_cmpstr (cursor_name (f), ==, "file00.txt");
  send_key (f, GDK_KEY_i, 0);
  send_key (f, GDK_KEY_l, 0);
  send_key (f, GDK_KEY_e, 0);
  send_key (f, GDK_KEY_1, 0);
  g_assert_cmpstr (cursor_name (f), ==, "file10.txt");
  g_assert_cmpstr (gbk_view_type_ahead_text (f->view), ==, "file1");
  send_key (f, GDK_KEY_BackSpace, 0);
  g_assert_cmpstr (gbk_view_type_ahead_text (f->view), ==, "file");
  send_key (f, GDK_KEY_Escape, 0);
  g_assert_cmpstr (gbk_view_type_ahead_text (f->view), ==, "");

  /* case-insensitive, non-ASCII */
  g_assert_true (gbk_view_type_ahead (f->view, "ÜN"));
  g_assert_cmpstr (cursor_name (f), ==, "ünïcödé näme.txt");
  send_key (f, GDK_KEY_Escape, 0);
  g_assert_true (gbk_view_type_ahead (f->view, "DIR B"));    /* space inside the search */
  g_assert_cmpstr (cursor_name (f), ==, "Dir B");
  send_key (f, GDK_KEY_Escape, 0);
  g_assert_false (gbk_view_type_ahead (f->view, "nomatch"));
  g_assert_cmpstr (cursor_name (f), ==, "Dir B");           /* unchanged */
}

static void
test_live_changes (Fixture *f, const void *data G_GNUC_UNUSED)
{
  GbkHandle h5 = gbk_dir_model_visible_handle (f->model, 5);
  gbk_view_select_only (f->view, h5);
  GbkHandle other = gbk_dir_model_visible_handle (f->model, 9);

  /* The selected file is deleted: selection drops it, cursor stays at the same index. */
  GbkScanMsg *d = msg (GBK_SCAN_DELTA);
  g_ptr_array_add (d->removes, g_strdup (gbk_entry_name (gbk_dir_model_get (f->model, h5))));
  g_ptr_array_add (d->upserts, mk ("aaa-new.txt", GBK_KIND_REGULAR));
  gbk_dir_model_apply_for_test (f->model, d);
  flush ();
  g_assert_cmpuint (gbk_view_n_selected (f->view), ==, 0);
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, 5);

  /* Hiding hidden-ness: a selected item that becomes invisible is deselected. */
  gbk_view_select_only (f->view, other);
  d = msg (GBK_SCAN_DELTA);
  g_ptr_array_add (d->upserts, mk (".now-hidden", GBK_KIND_REGULAR));
  gbk_dir_model_apply_for_test (f->model, d);
  flush ();
  g_assert_true (gbk_view_is_selected (f->view, other));   /* unaffected */
}

static void
test_many_items_and_screenshot (Fixture *f, const void *data G_GNUC_UNUSED)
{
  /* Scroll to the end with the keyboard; the cursor must stay on screen. */
  send_key (f, GDK_KEY_End, 0);
  GtkAdjustment *adj = gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (f->view));
  g_assert_cmpfloat (gtk_adjustment_get_value (adj), >, 0);
  send_key (f, GDK_KEY_Home, 0);
  g_assert_cmpfloat (gtk_adjustment_get_value (adj), ==, 0);

  send_key (f, GDK_KEY_Down, 0);
  send_key (f, GDK_KEY_Down, GDK_SHIFT_MASK);
  const char *dir = g_getenv ("GBK_ARTIFACTS");
  if (dir != nullptr)
    {
      /* Draw synchronously: the offscreen surface may lag the latest state by a frame.
       * Finish any pending size allocation first (gtk_widget_draw requires it). */
      gtk_container_check_resize (GTK_CONTAINER (f->window));
      int w = gtk_widget_get_allocated_width (f->window);
      int h = gtk_widget_get_allocated_height (f->window);
      cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
      cairo_t *cr = cairo_create (s);
      gtk_widget_draw (f->window, cr);
      cairo_destroy (cr);
      g_autofree char *path = g_build_filename (dir, "view.png", nullptr);
      g_assert_cmpint (cairo_surface_write_to_png (s, path), ==, CAIRO_STATUS_SUCCESS);
      cairo_surface_destroy (s);
    }
}

/* Icons view: a grid of cells, rows left to right, scrolling down; arrows move across rows
 * and down columns. */
static void
test_icons_view (Fixture *f, const void *data G_GNUC_UNUSED)
{
  GtkScrolledWindow *sw = GTK_SCROLLED_WINDOW (gtk_widget_get_parent (GTK_WIDGET (f->view)));
  gtk_scrolled_window_set_policy (sw, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gbk_view_set_mode (f->view, GBK_VIEW_ICONS, 48);
  flush ();
  g_assert_cmpint (gbk_view_mode (f->view), ==, GBK_VIEW_ICONS);
  /* Cells are 48 + 56 = 104 px wide: 4 to a row in 500 px (less a scrollbar). */
  g_assert_cmpstr (name_at (f, idx (f, gbk_view_item_at (f->view, 50, 10))), ==, "Dir A");
  g_assert_cmpstr (name_at (f, idx (f, gbk_view_item_at (f->view, 150, 10))), ==, "Dir B");
  g_assert_true (gbk_view_item_at (f->view, 450, 10) == GBK_HANDLE_NONE);   /* past the row */
  send_key (f, GDK_KEY_Home, 0);
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, 0);
  send_key (f, GDK_KEY_Right, 0);
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, 1);
  send_key (f, GDK_KEY_Down, 0);
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, 5);
  send_key (f, GDK_KEY_Up, 0);
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, 1);

  /* The end is down there: scrolled to, and the sideways scrollbar is out of it. */
  GtkAdjustment *v = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (f->view));
  GtkAdjustment *h = gtk_scrollable_get_hadjustment (GTK_SCROLLABLE (f->view));
  send_key (f, GDK_KEY_End, 0);
  g_assert_cmpfloat (gtk_adjustment_get_value (v), >, 0);
  g_assert_cmpfloat (gtk_adjustment_get_upper (h), ==, 0);
  g_assert_cmpuint (idx (f, gbk_view_cursor (f->view)), ==, gbk_dir_model_n_visible (f->model) - 1);
  const char *dir = g_getenv ("GBK_ARTIFACTS");
  if (dir != nullptr)
    {
      gtk_container_check_resize (GTK_CONTAINER (f->window));
      int w = gtk_widget_get_allocated_width (f->window);
      int ht = gtk_widget_get_allocated_height (f->window);
      cairo_surface_t *s = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, ht);
      cairo_t *cr = cairo_create (s);
      gtk_widget_draw (f->window, cr);
      cairo_destroy (cr);
      g_autofree char *path = g_build_filename (dir, "view-icons.png", nullptr);
      g_assert_cmpint (cairo_surface_write_to_png (s, path), ==, CAIRO_STATUS_SUCCESS);
      cairo_surface_destroy (s);
    }
  send_key (f, GDK_KEY_Home, 0);
  g_assert_cmpfloat (gtk_adjustment_get_value (v), ==, 0);

  /* And back: columns again, the cursor where it was, nothing scrolled down. */
  send_key (f, GDK_KEY_Right, 0);
  gtk_scrolled_window_set_policy (sw, GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gbk_view_set_mode (f->view, GBK_VIEW_COMPACT, 48);
  flush ();
  g_assert_cmpint (gbk_view_mode (f->view), ==, GBK_VIEW_COMPACT);
  g_assert_cmpstr (cursor_name (f), ==, "Dir B");
  g_assert_cmpfloat (gtk_adjustment_get_upper (v), ==, 0);
  send_key (f, GDK_KEY_Down, 0);
  g_assert_cmpstr (cursor_name (f), ==, "Dir C");
}

#define ADD(path, fn, n, w, h) \
  g_test_add (path, Fixture, nullptr, setup_##n##_##w##_##h, fn, teardown)

static void
setup_30_500_160 (Fixture *f, const void *d G_GNUC_UNUSED)
{
  setup (f, 30, 500, 160);
}
static void
setup_30_500_300 (Fixture *f, const void *d G_GNUC_UNUSED)
{
  setup (f, 30, 500, 300);
}
static void
setup_200_600_200 (Fixture *f, const void *d G_GNUC_UNUSED)
{
  setup (f, 200, 600, 200);
}

int
main (int argc, char **argv)
{
  gtk_test_init (&argc, &argv, nullptr);
  const char globs[] = "50:text/plain:*.txt\n50:application/zip:*.zip\n";
  GbkTypeBuilder *b = gbk_type_builder_new ();
  gbk_type_builder_load_globs2 (b, globs, strlen (globs));
  const char icons[] = "application/zip:package-x-generic\n";
  gbk_type_builder_load_generic_icons (b, icons, strlen (icons));
  types = gbk_type_builder_finish (b);

  ADD ("/view/layout", test_layout, 30, 500, 160);
  ADD ("/view/widths-follow-model", test_widths_follow_model, 30, 500, 160);
  ADD ("/view/keyboard", test_keyboard, 30, 500, 160);
  ADD ("/view/mouse", test_mouse, 30, 500, 160);
  ADD ("/view/press-on-selection", test_press_on_selection, 30, 500, 160);
  ADD ("/view/drop-targets", test_drop_targets, 30, 500, 160);
  ADD ("/view/type-ahead", test_type_ahead, 30, 500, 160);
  ADD ("/view/live-changes", test_live_changes, 30, 500, 160);
  ADD ("/view/many-items", test_many_items_and_screenshot, 200, 600, 200);
  ADD ("/view/icons", test_icons_view, 30, 500, 300);
  int status = g_test_run ();

  gbk_type_table_unref (types);
  if (gbk_lifetime_live () != 0)
    gbk_fatal ("%d registrations leaked", gbk_lifetime_live ());
  return status;
}
