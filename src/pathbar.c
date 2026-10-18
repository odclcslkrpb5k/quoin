#include "pathbar.h"
#include "cliptext.h"
#include "dnd.h"
#include "lifetime.h"

#define KEY_LOC "gbk-loc"

struct _GbkPathbar
{
  GtkBox parent_instance;
  GtkStack *stack;
  GtkBox *buttons;
  GtkScrolledWindow *scroller;
  GtkEntry *entry;
  GtkToggleButton *edit;
  GbkLoc *loc;         /* current */
  GbkLoc *deep;        /* deepest location whose buttons are shown */
  bool updating;       /* programmatic toggles: ignore their "clicked" */
};

G_DEFINE_FINAL_TYPE (GbkPathbar, gbk_pathbar, GTK_TYPE_BOX)

enum
{
  SIGNAL_NAVIGATE,
  SIGNAL_CONTEXT_MENU,
  SIGNAL_DROP,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void
on_button_clicked (GtkToggleButton *button, GbkPathbar *bar)
{
  if (bar->updating)
    return;
  /* Our own reference: the button may go while handlers run. */
  g_autoptr (GbkLoc) target = gbk_loc_ref (g_object_get_data (G_OBJECT (button), KEY_LOC));
  /* Keep the pressed state honest until the window confirms the navigation. */
  bar->updating = true;
  gtk_toggle_button_set_active (button, gbk_loc_equal (target, bar->loc));
  bar->updating = false;
  g_signal_emit (bar, signals[SIGNAL_NAVIGATE], 0, target);
}

static gboolean
on_button_press (GtkWidget *button, GdkEventButton *ev, GbkPathbar *bar)
{
  if (!gdk_event_triggers_context_menu ((GdkEvent *) ev))
    return false;
  g_autoptr (GbkLoc) target = gbk_loc_ref (g_object_get_data (G_OBJECT (button), KEY_LOC));
  g_signal_emit (bar, signals[SIGNAL_CONTEXT_MENU], 0, target, (GdkEvent *) ev);
  return true;          /* no press/toggle for a right-click */
}

/* ---- drops onto buttons (see dnd.h): highlight, then hand the files to the window ---- */

static gboolean
on_drag_motion (GtkWidget *button, GdkDragContext *ctx, int x G_GNUC_UNUSED, int y G_GNUC_UNUSED,
                guint time, GbkPathbar *bar G_GNUC_UNUSED)
{
  GdkDragAction action
      = gtk_drag_dest_find_target (button, ctx, nullptr) != GDK_NONE ? gbk_dnd_action (ctx) : 0;
  if (action != 0)
    gtk_drag_highlight (button);
  gdk_drag_status (ctx, action, time);
  return true;
}

static void
on_drag_leave (GtkWidget *button, GdkDragContext *ctx G_GNUC_UNUSED, guint time G_GNUC_UNUSED,
               GbkPathbar *bar G_GNUC_UNUSED)
{
  gtk_drag_unhighlight (button);
}

static gboolean
on_drag_drop (GtkWidget *button, GdkDragContext *ctx, int x G_GNUC_UNUSED, int y G_GNUC_UNUSED,
              guint time, GbkPathbar *bar G_GNUC_UNUSED)
{
  GdkAtom target = gtk_drag_dest_find_target (button, ctx, nullptr);
  if (target == GDK_NONE)
    gtk_drag_finish (ctx, false, false, time);
  else
    gtk_drag_get_data (button, ctx, target, time);
  return true;
}

static void
deliver_drop (GbkPathbar *bar, GbkLoc *dest, GPtrArray *locs, GdkEvent *trigger)
{
  if (locs->len == 0)
    return;
  g_autoptr (GbkLoc) keep = gbk_loc_ref (dest);   /* the button may go while handlers run */
  g_signal_emit (bar, signals[SIGNAL_DROP], 0, keep, locs, trigger);
}

static void
on_drag_data_received (GtkWidget *button, GdkDragContext *ctx, int x G_GNUC_UNUSED,
                       int y G_GNUC_UNUSED, GtkSelectionData *sd, guint info G_GNUC_UNUSED,
                       guint time, GbkPathbar *bar)
{
  gtk_drag_unhighlight (button);
  g_autoptr (GPtrArray) locs = gbk_dnd_locs (sd);
  gtk_drag_finish (ctx, locs->len > 0, false, time);   /* never "delete the source" */
  g_autoptr (GdkEvent) trigger = gtk_get_current_event ();
  deliver_drop (bar, g_object_get_data (G_OBJECT (button), KEY_LOC), locs, trigger);
}

static GtkWidget *
make_button (GbkPathbar *bar, GbkLoc *loc, const char *label, const char *icon)
{
  GtkWidget *button = gtk_toggle_button_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  if (icon != nullptr)
    gtk_box_pack_start (GTK_BOX (box), gtk_image_new_from_icon_name (icon, GTK_ICON_SIZE_MENU),
                        false, false, 0);
  GtkWidget *l = gtk_label_new (label);
  gtk_box_pack_start (GTK_BOX (box), l, false, false, 0);
  gtk_container_add (GTK_CONTAINER (button), box);
  gtk_widget_set_focus_on_click (button, false);
  g_object_set_data_full (G_OBJECT (button), KEY_LOC, gbk_loc_ref (loc),
                          (GDestroyNotify) gbk_loc_unref);
  g_object_set_data_full (G_OBJECT (button), "gbk-label", g_strdup (label), g_free);
  gbk_connect (G_OBJECT (bar), button, "clicked", G_CALLBACK (on_button_clicked), bar);
  gbk_connect (G_OBJECT (bar), button, "button-press-event", G_CALLBACK (on_button_press), bar);
  gtk_drag_dest_set (button, 0, nullptr, 0, GBK_DND_ACTIONS);
  g_autoptr (GtkTargetList) targets = gbk_dnd_dest_targets ();
  gtk_drag_dest_set_target_list (button, targets);
  gbk_connect (G_OBJECT (bar), button, "drag-motion", G_CALLBACK (on_drag_motion), bar);
  gbk_connect (G_OBJECT (bar), button, "drag-leave", G_CALLBACK (on_drag_leave), bar);
  gbk_connect (G_OBJECT (bar), button, "drag-drop", G_CALLBACK (on_drag_drop), bar);
  gbk_connect (G_OBJECT (bar), button, "drag-data-received", G_CALLBACK (on_drag_data_received),
               bar);
  return button;
}

static void
rebuild (GbkPathbar *bar)
{
  g_autoptr (GList) old = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = old; l != nullptr; l = l->next)
    gtk_widget_destroy (l->data);

  /* Home is this machine's: a location elsewhere starts at its root. */
  g_autoptr (GbkLoc) home = gbk_loc_is_local (bar->deep) && g_get_home_dir () != nullptr
                                ? gbk_loc_new_like (bar->deep, g_get_home_dir ())
                                : nullptr;
  bool in_home = home != nullptr && !gbk_loc_is_root (home) && gbk_loc_is_under (bar->deep, home);
  g_autoptr (GbkLoc) start = in_home ? gbk_loc_ref (home) : gbk_loc_new_like (bar->deep, "/");
  g_autofree char *start_label = in_home ? gbk_loc_display_name (home) : g_strdup ("File System");
  gtk_box_pack_start (
      bar->buttons, make_button (bar, start, start_label, in_home ? "user-home" : "drive-harddisk"),
      false, false, 0);

  /* One button per component below the start. */
  const char *rest = gbk_loc_path (bar->deep) + (in_home ? strlen (gbk_loc_path (start)) : 0);
  g_auto (GStrv) parts = g_strsplit (rest, "/", -1);
  g_autoptr (GbkLoc) acc = gbk_loc_ref (start);
  for (size_t i = 0; parts[i] != nullptr; i++)
    {
      if (parts[i][0] == '\0')
        continue;
      GbkLoc *next = gbk_loc_child (acc, parts[i]);
      gbk_loc_unref (acc);
      acc = next;
      g_autofree char *label = g_filename_display_name (parts[i]);
      gtk_box_pack_start (bar->buttons, make_button (bar, acc, label, nullptr), false, false, 0);
    }
  gtk_widget_show_all (GTK_WIDGET (bar->buttons));
}

static bool
scroll_to_current (void *data)
{
  GbkPathbar *bar = data;
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = kids; l != nullptr; l = l->next)
    if (gtk_toggle_button_get_active (l->data))
      {
        GtkAllocation a;
        gtk_widget_get_allocation (l->data, &a);
        GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment (bar->scroller);
        double page = gtk_adjustment_get_page_size (adj);
        gtk_adjustment_clamp_page (adj, a.x, MIN (a.x + a.width, a.x + page));
      }
  return false;
}

void
gbk_pathbar_set_loc (GbkPathbar *bar, GbkLoc *loc)
{
  g_clear_pointer (&bar->loc, gbk_loc_unref);
  bar->loc = gbk_loc_ref (loc);
  if (bar->deep == nullptr || !gbk_loc_is_under (bar->deep, loc))
    {
      g_clear_pointer (&bar->deep, gbk_loc_unref);
      bar->deep = gbk_loc_ref (loc);
      rebuild (bar);
    }
  bar->updating = true;
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = kids; l != nullptr; l = l->next)
    gtk_toggle_button_set_active (l->data,
                                  gbk_loc_equal (g_object_get_data (l->data, KEY_LOC), loc));
  bar->updating = false;
  /* After layout: bring the pressed button into view (long paths scroll). */
  gbk_idle_add (G_OBJECT (bar), scroll_to_current, bar);
}

/* ---- text entry mode ---- */

static void
show_buttons (GbkPathbar *bar)
{
  gtk_stack_set_visible_child_name (bar->stack, "buttons");
  bar->updating = true;
  gtk_toggle_button_set_active (bar->edit, false);
  bar->updating = false;
}

void
gbk_pathbar_edit (GbkPathbar *bar)
{
  g_autofree char *shown = bar->loc != nullptr ? gbk_loc_display (bar->loc) : g_strdup ("/");
  gtk_entry_set_text (bar->entry, shown);
  gtk_stack_set_visible_child_name (bar->stack, "entry");
  bar->updating = true;
  gtk_toggle_button_set_active (bar->edit, true);
  bar->updating = false;
  gtk_widget_grab_focus (GTK_WIDGET (bar->entry));
  gtk_editable_select_region (GTK_EDITABLE (bar->entry), 0, -1);
}

bool
gbk_pathbar_editing (GbkPathbar *bar)
{
  return g_str_equal (gtk_stack_get_visible_child_name (bar->stack), "entry");
}

static void
on_entry_activate (GtkEntry *entry, GbkPathbar *bar)
{
  const char *text = gtk_entry_get_text (entry);
  g_autofree char *expanded = nullptr;
  if (text[0] == '~' && (text[1] == '\0' || text[1] == '/'))
    expanded = g_strconcat (g_get_home_dir (), text + 1, nullptr);
  else
    expanded = g_strdup (text);
  g_autofree char *fs = g_filename_from_utf8 (expanded, -1, nullptr, nullptr, nullptr);
  show_buttons (bar);
  if (fs == nullptr || fs[0] == '\0')
    return;
  if (bar->loc == nullptr)
    return;
  /* Relative input is relative to the current folder. */
  g_autoptr (GbkLoc) target = gbk_loc_new_like (bar->loc, fs);
  g_signal_emit (bar, signals[SIGNAL_NAVIGATE], 0, target);
}

static gboolean
on_entry_key (GtkWidget *w G_GNUC_UNUSED, GdkEventKey *ev, GbkPathbar *bar)
{
  if (ev->keyval == GDK_KEY_Escape)
    {
      show_buttons (bar);
      return true;
    }
  return false;
}

static void
on_edit_toggled (GtkToggleButton *b, GbkPathbar *bar)
{
  if (bar->updating)
    return;
  if (gtk_toggle_button_get_active (b))
    gbk_pathbar_edit (bar);
  else
    show_buttons (bar);
}

/* ---- tests ---- */

char *
gbk_pathbar_describe (GbkPathbar *bar)
{
  GString *s = g_string_new ("");
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = kids; l != nullptr; l = l->next)
    {
      const char *label = g_object_get_data (l->data, "gbk-label");
      if (s->len > 0)
        g_string_append_c (s, '|');
      if (gtk_toggle_button_get_active (l->data))
        g_string_append_printf (s, "[%s]", label);
      else
        g_string_append (s, label);
    }
  return g_string_free (s, false);
}

void
gbk_pathbar_click (GbkPathbar *bar, const char *path)
{
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = kids; l != nullptr; l = l->next)
    if (g_str_equal (gbk_loc_path (g_object_get_data (l->data, KEY_LOC)), path))
      {
        gtk_button_clicked (GTK_BUTTON (l->data));
        return;
      }
  gbk_fatal ("no path bar button for that folder");
}

GtkWidget *
gbk_pathbar_button (GbkPathbar *bar, const char *path)
{
  g_autoptr (GList) kids = gtk_container_get_children (GTK_CONTAINER (bar->buttons));
  for (GList *l = kids; l != nullptr; l = l->next)
    if (g_str_equal (gbk_loc_path (g_object_get_data (l->data, KEY_LOC)), path))
      return l->data;
  gbk_fatal ("no path bar button for that folder");
}

GtkEntry *
gbk_pathbar_entry (const GbkPathbar *bar)
{
  return bar->entry;
}

void
gbk_pathbar_drop_for_test (GbkPathbar *bar, const char *path, const char *uri_list,
                           GdkEvent *trigger)
{
  bool cut;
  g_autoptr (GPtrArray) paths = gbk_clip_parse (uri_list, strlen (uri_list), false, &cut);
  g_autoptr (GPtrArray) locs = gbk_loc_array_from_local (paths);
  g_autoptr (GbkLoc) dest = gbk_loc_new_like (bar->loc, path);
  deliver_drop (bar, dest, locs, trigger);
}

/* ---- GObject ---- */

static void
gbk_pathbar_finalize (GObject *o)
{
  const GbkPathbar *bar = GBK_PATHBAR (o);
  gbk_loc_unref (bar->loc);
  gbk_loc_unref (bar->deep);
  G_OBJECT_CLASS (gbk_pathbar_parent_class)->finalize (o);
}

static void
gbk_pathbar_class_init (GbkPathbarClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gbk_pathbar_finalize;
  signals[SIGNAL_NAVIGATE]
      = g_signal_new ("navigate", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr,
                      nullptr, G_TYPE_NONE, 1, G_TYPE_POINTER);
  /* GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE: the event is passed through, not copied. */
  signals[SIGNAL_CONTEXT_MENU] = g_signal_new (
      "context-menu", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
      G_TYPE_NONE, 2, G_TYPE_POINTER, GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);
  signals[SIGNAL_DROP] = g_signal_new ("drop", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                       nullptr, nullptr, nullptr, G_TYPE_NONE, 3, G_TYPE_POINTER,
                                       G_TYPE_POINTER, GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);
}

static void
gbk_pathbar_init (GbkPathbar *bar)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (bar), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing (GTK_BOX (bar), 4);

  bar->stack = GTK_STACK (gtk_stack_new ());
  bar->scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new (nullptr, nullptr));
  /* EXTERNAL: scrollable (wheel, programmatic) without a visible scrollbar. */
  gtk_scrolled_window_set_policy (bar->scroller, GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  bar->buttons = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (bar->buttons)),
                               GTK_STYLE_CLASS_LINKED);
  gtk_container_add (GTK_CONTAINER (bar->scroller), GTK_WIDGET (bar->buttons));
  bar->entry = GTK_ENTRY (gtk_entry_new ());
  gtk_stack_add_named (bar->stack, GTK_WIDGET (bar->scroller), "buttons");
  gtk_stack_add_named (bar->stack, GTK_WIDGET (bar->entry), "entry");
  gtk_box_pack_start (GTK_BOX (bar), GTK_WIDGET (bar->stack), true, true, 0);

  bar->edit = GTK_TOGGLE_BUTTON (gtk_toggle_button_new ());
  gtk_container_add (GTK_CONTAINER (bar->edit),
                     gtk_image_new_from_icon_name ("document-edit-symbolic", GTK_ICON_SIZE_MENU));
  gtk_widget_set_tooltip_text (GTK_WIDGET (bar->edit), "Type a location (Ctrl+L)");
  gtk_widget_set_focus_on_click (GTK_WIDGET (bar->edit), false);
  gtk_box_pack_start (GTK_BOX (bar), GTK_WIDGET (bar->edit), false, false, 0);

  gbk_connect (G_OBJECT (bar), bar->entry, "activate", G_CALLBACK (on_entry_activate), bar);
  gbk_connect (G_OBJECT (bar), bar->entry, "key-press-event", G_CALLBACK (on_entry_key), bar);
  gbk_connect (G_OBJECT (bar), bar->edit, "toggled", G_CALLBACK (on_edit_toggled), bar);
}

GtkWidget *
gbk_pathbar_new (void)
{
  return g_object_new (GBK_TYPE_PATHBAR, nullptr);
}
