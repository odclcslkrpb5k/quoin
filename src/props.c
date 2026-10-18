#include "props.h"
#include "job.h"
#include "jobui.h"
#include "lifetime.h"

struct _GbkProps
{
  GtkWindow parent_instance;
  GbkLoc *loc;
  char *mime;

  GtkBox *body;
  GtkWidget *details;           /* grid of fields, rebuilt on every read */
  GHashTable *values;           /* field name -> GtkLabel (borrowed from `details`) */
  GtkLabel *status;             /* "Reading…" or the error */
  GtkWidget *perm_section;      /* heading, grid and note: hidden until details arrive */
  GtkWidget *perm_box;
  GtkToggleButton *perm[3][3];  /* [owner, group, others][read, write, execute] */
  GtkLabel *octal;
  GtkLabel *perm_note;
  GtkWidget *apply;

  bool loaded;
  bool updating;                /* setting the check boxes from the file, not the user */
  uint32_t loaded_rwx;
};

G_DEFINE_FINAL_TYPE (GbkProps, gbk_props, GTK_TYPE_WINDOW)

/* ---- permissions ---- */

static uint32_t
checked_rwx (GbkProps *p)
{
  uint32_t rwx = 0;
  for (int who = 0; who < 3; who++)
    for (int what = 0; what < 3; what++)
      if (gtk_toggle_button_get_active (p->perm[who][what]))
        rwx |= 1u << (8 - (who * 3 + what));
  return rwx;
}

static void
update_octal (GbkProps *p)
{
  uint32_t rwx = checked_rwx (p);
  g_autofree char *text = g_strdup_printf ("%03o", rwx);
  gtk_label_set_text (p->octal, text);
  gtk_widget_set_sensitive (p->apply, p->loaded && rwx != p->loaded_rwx);
}

static void
on_perm_toggled (GtkToggleButton *b G_GNUC_UNUSED, GbkProps *p)
{
  if (!p->updating)
    update_octal (p);
}

/* ---- details ---- */

static char *
format_time (int64_t ns)
{
  g_autoptr (GDateTime) t = g_date_time_new_from_unix_local_usec (ns / 1000);
  return t != nullptr ? g_date_time_format (t, "%Y-%m-%d %H:%M:%S") : g_strdup ("?");
}

static char *
format_id (const char *name, uint32_t id)
{
  return name != nullptr ? g_strdup_printf ("%s (%u)", name, id) : g_strdup_printf ("%u", id);
}

static void
add_field (GbkProps *p, int row, const char *field, const char *value)
{
  GtkWidget *key = gtk_label_new (field);
  gtk_label_set_xalign (GTK_LABEL (key), 1);
  gtk_style_context_add_class (gtk_widget_get_style_context (key), GTK_STYLE_CLASS_DIM_LABEL);
  GtkWidget *val = gtk_label_new (value);
  gtk_label_set_xalign (GTK_LABEL (val), 0);
  gtk_label_set_selectable (GTK_LABEL (val), true);
  gtk_label_set_ellipsize (GTK_LABEL (val), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_width_chars (GTK_LABEL (val), (int) MIN (g_utf8_strlen (value, -1), 48));
  gtk_widget_set_tooltip_text (val, value);
  gtk_widget_set_can_focus (val, false);    /* selectable labels grab focus and select all */
  gtk_grid_attach (GTK_GRID (p->details), key, 0, row, 1, 1);
  gtk_grid_attach (GTK_GRID (p->details), val, 1, row, 1, 1);
  g_hash_table_insert (p->values, g_strdup (field), val);
}

static void
show_details (GbkProps *p, const GbkFileInfo *info)
{
  if (p->details != nullptr)
    gtk_widget_destroy (p->details);
  g_hash_table_remove_all (p->values);
  p->details = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (p->details), 4);
  gtk_grid_set_column_spacing (GTK_GRID (p->details), 12);
  gtk_box_pack_start (p->body, p->details, false, false, 0);
  gtk_box_reorder_child (p->body, p->details, 0);

  int row = 0;
  g_autofree char *name = gbk_loc_display_name (p->loc);
  g_autoptr (GbkLoc) dir = gbk_loc_parent (p->loc);
  g_autofree char *location = gbk_loc_display (dir);
  add_field (p, row++, "Name", name);
  add_field (p, row++, "Location", location);
  bool is_dir = S_ISDIR (info->mode) && !info->dangling;
  add_field (p, row++, "Type", info->dangling ? "Broken link" : is_dir ? "Folder" : p->mime);
  if (info->symlink)
    {
      g_autofree char *target = info->link_target != nullptr
                                    ? g_filename_display_name (info->link_target)
                                    : g_strdup ("?");
      add_field (p, row++, "Link target", target);
    }
  if (!is_dir)
    {
      g_autofree char *human = g_format_size_full (info->size, G_FORMAT_SIZE_IEC_UNITS);
      g_autofree char *size
          = g_strdup_printf ("%s (%" G_GUINT64_FORMAT " bytes)", human, info->size);
      add_field (p, row++, "Size", size);
    }
  g_autofree char *mtime = format_time (info->mtime_ns);
  g_autofree char *atime = format_time (info->atime_ns);
  g_autofree char *ctime = format_time (info->ctime_ns);
  add_field (p, row++, "Modified", mtime);
  add_field (p, row++, "Accessed", atime);
  add_field (p, row++, "Changed", ctime);
  if (info->has_btime)
    {
      g_autofree char *btime = format_time (info->btime_ns);
      add_field (p, row++, "Created", btime);
    }
  g_autofree char *owner = format_id (info->owner, info->uid);
  g_autofree char *group = format_id (info->group, info->gid);
  add_field (p, row++, "Owner", owner);
  add_field (p, row++, "Group", group);
  gtk_widget_show_all (p->details);

  /* Permissions: only the owner (or root) may change them; a broken link has none. */
  p->loaded_rwx = info->mode & 0777;
  p->updating = true;
  for (int who = 0; who < 3; who++)
    for (int what = 0; what < 3; what++)
      gtk_toggle_button_set_active (p->perm[who][what],
                                    (p->loaded_rwx & (1u << (8 - (who * 3 + what)))) != 0);
  p->updating = false;
  uid_t me = geteuid ();
  bool may_change = !info->dangling && (me == 0 || me == info->uid);
  gtk_widget_set_sensitive (p->perm_box, may_change);
  if (info->dangling)
    gtk_label_set_text (p->perm_note, "A broken link has no permissions.");
  else if (!may_change)
    {
      g_autofree char *note = g_strdup_printf ("Only %s can change these.", owner);
      gtk_label_set_text (p->perm_note, note);
    }
  gtk_widget_set_visible (GTK_WIDGET (p->perm_note), !may_change);
  gtk_widget_set_visible (p->perm_section, true);
  gtk_widget_set_visible (GTK_WIDGET (p->status), false);
  p->loaded = true;
  update_octal (p);
}

static void
on_info_finished (GbkJob *job, GError *error, GbkProps *p)
{
  if (error != nullptr)
    {
      g_autofree char *text = g_strdup_printf ("Can't read details: %s", error->message);
      gtk_label_set_text (p->status, text);
      gtk_widget_set_visible (GTK_WIDGET (p->status), true);
      return;
    }
  show_details (p, gbk_job_info (job));
}

static void
read_info (GbkProps *p)
{
  p->loaded = false;
  gtk_widget_set_sensitive (p->apply, false);
  g_autoptr (GbkJob) job = gbk_job_new_info (p->loc);
  gbk_connect (G_OBJECT (p), job, "finished", G_CALLBACK (on_info_finished), p);
  gbk_job_start (job);         /* a running job keeps itself alive */
}

/* ---- buttons ---- */

static void
on_permissions_finished (GbkJob *job G_GNUC_UNUSED, GError *error G_GNUC_UNUSED, GbkProps *p)
{
  /* Errors are reported by the job UI. Either way, show what the file has now. */
  read_info (p);
}

static void
on_apply (GtkButton *b G_GNUC_UNUSED, GbkProps *p)
{
  if (!p->loaded)
    return;
  g_autoptr (GbkJob) job = gbk_job_new_permissions (p->loc, checked_rwx (p));
  gbk_connect (G_OBJECT (p), job, "finished", G_CALLBACK (on_permissions_finished), p);
  gtk_widget_set_sensitive (p->apply, false);
  gbk_jobui_start (job, GTK_WINDOW (p));
}

static void
on_close (GtkButton *b G_GNUC_UNUSED, GbkProps *p)
{
  gtk_widget_destroy (GTK_WIDGET (p));
}

static gboolean
gbk_props_key_press (GtkWidget *w, GdkEventKey *ev)
{
  if (ev->keyval == GDK_KEY_Escape)
    {
      gtk_widget_destroy (w);
      return true;
    }
  return GTK_WIDGET_CLASS (gbk_props_parent_class)->key_press_event (w, ev);
}

/* ---- GObject ---- */

static void
gbk_props_finalize (GObject *o)
{
  GbkProps *p = GBK_PROPS (o);
  g_clear_pointer (&p->loc, gbk_loc_unref);
  g_free (p->mime);
  g_hash_table_unref (p->values);
  G_OBJECT_CLASS (gbk_props_parent_class)->finalize (o);
}

static void
gbk_props_class_init (GbkPropsClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gbk_props_finalize;
  GTK_WIDGET_CLASS (klass)->key_press_event = gbk_props_key_press;
}

static void
gbk_props_init (GbkProps *p)
{
  p->values = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nullptr);
}

static GtkWidget *
heading (const char *text)
{
  GtkWidget *l = gtk_label_new (nullptr);
  g_autofree char *markup = g_markup_printf_escaped ("<b>%s</b>", text);
  gtk_label_set_markup (GTK_LABEL (l), markup);
  gtk_label_set_xalign (GTK_LABEL (l), 0);
  return l;
}

GbkProps *
gbk_props_new (GtkWindow *parent, GbkLoc *loc, const char *mime)
{
  GbkProps *p = g_object_new (GBK_TYPE_PROPS, "type", GTK_WINDOW_TOPLEVEL, nullptr);
  p->loc = gbk_loc_ref (loc);
  p->mime = g_strdup (mime);
  g_autofree char *name = gbk_loc_display_name (loc);
  g_autofree char *title = g_strdup_printf ("%s Properties", name);
  gtk_window_set_title (GTK_WINDOW (p), title);
  gtk_window_set_transient_for (GTK_WINDOW (p), parent);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (p), true);
  gtk_window_set_type_hint (GTK_WINDOW (p), GDK_WINDOW_TYPE_HINT_DIALOG);

  p->body = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 12));
  gtk_container_set_border_width (GTK_CONTAINER (p->body), 12);

  p->status = GTK_LABEL (gtk_label_new ("Reading…"));
  gtk_label_set_xalign (p->status, 0);
  gtk_label_set_line_wrap (p->status, true);
  gtk_box_pack_start (p->body, GTK_WIDGET (p->status), false, false, 0);

  /* Permissions grid: rows owner/group/others, columns read/write/execute. */
  GtkWidget *perm_section = p->perm_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_pack_start (GTK_BOX (perm_section), heading ("Permissions"), false, false, 0);
  p->perm_box = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (p->perm_box), 2);
  gtk_grid_set_column_spacing (GTK_GRID (p->perm_box), 16);
  static const char *const whos[] = { "Owner", "Group", "Others" };
  static const char *const whats[] = { "Read", "Write", "Execute" };
  for (int what = 0; what < 3; what++)
    gtk_grid_attach (GTK_GRID (p->perm_box), gtk_label_new (whats[what]), what + 1, 0, 1, 1);
  for (int who = 0; who < 3; who++)
    {
      GtkWidget *l = gtk_label_new (whos[who]);
      gtk_label_set_xalign (GTK_LABEL (l), 0);
      gtk_grid_attach (GTK_GRID (p->perm_box), l, 0, who + 1, 1, 1);
      for (int what = 0; what < 3; what++)
        {
          GtkWidget *c = gtk_check_button_new ();
          gtk_widget_set_halign (c, GTK_ALIGN_CENTER);
          g_autofree char *tip = g_strdup_printf ("%s: %s", whos[who], whats[what]);
          gtk_widget_set_tooltip_text (c, tip);
          p->perm[who][what] = GTK_TOGGLE_BUTTON (c);
          gbk_connect (G_OBJECT (p), c, "toggled", G_CALLBACK (on_perm_toggled), p);
          gtk_grid_attach (GTK_GRID (p->perm_box), c, what + 1, who + 1, 1, 1);
        }
    }
  GtkWidget *octal_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *octal_key = gtk_label_new ("Octal");
  gtk_style_context_add_class (gtk_widget_get_style_context (octal_key), GTK_STYLE_CLASS_DIM_LABEL);
  p->octal = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_selectable (p->octal, true);
  gtk_widget_set_can_focus (GTK_WIDGET (p->octal), false);
  gtk_box_pack_start (GTK_BOX (octal_row), octal_key, false, false, 0);
  gtk_box_pack_start (GTK_BOX (octal_row), GTK_WIDGET (p->octal), false, false, 0);
  gtk_grid_attach (GTK_GRID (p->perm_box), octal_row, 0, 4, 4, 1);
  gtk_box_pack_start (GTK_BOX (perm_section), p->perm_box, false, false, 0);
  p->perm_note = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (p->perm_note, 0);
  gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (p->perm_note)),
                               GTK_STYLE_CLASS_DIM_LABEL);
  gtk_box_pack_start (GTK_BOX (perm_section), GTK_WIDGET (p->perm_note), false, false, 0);
  gtk_box_pack_start (p->body, perm_section, false, false, 0);

  GtkWidget *buttons = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_button_box_set_layout (GTK_BUTTON_BOX (buttons), GTK_BUTTONBOX_END);
  gtk_box_set_spacing (GTK_BOX (buttons), 6);
  GtkWidget *close = gtk_button_new_with_mnemonic ("_Close");
  p->apply = gtk_button_new_with_mnemonic ("_Apply");
  gtk_container_add (GTK_CONTAINER (buttons), close);
  gtk_container_add (GTK_CONTAINER (buttons), p->apply);
  gtk_box_pack_end (p->body, buttons, false, false, 0);
  gbk_connect (G_OBJECT (p), close, "clicked", G_CALLBACK (on_close), p);
  gbk_connect (G_OBJECT (p), p->apply, "clicked", G_CALLBACK (on_apply), p);

  gtk_container_add (GTK_CONTAINER (p), GTK_WIDGET (p->body));
  gtk_widget_show_all (GTK_WIDGET (p->body));
  gtk_widget_set_visible (p->perm_section, false);
  gtk_widget_set_sensitive (p->apply, false);
  read_info (p);
  return p;
}

bool
gbk_props_loaded (const GbkProps *p)
{
  return p->loaded;
}

const char *
gbk_props_value (GbkProps *p, const char *field)
{
  GtkLabel *l = g_hash_table_lookup (p->values, field);
  return l != nullptr ? gtk_label_get_text (l) : nullptr;
}

GtkToggleButton *
gbk_props_permission (GbkProps *p, int who, int what)
{
  if (who < 0 || who > 2 || what < 0 || what > 2)
    gbk_fatal ("no permission %d/%d", who, what);
  return p->perm[who][what];
}

GtkButton *
gbk_props_apply_button (GbkProps *p)
{
  return GTK_BUTTON (p->apply);
}

const char *
gbk_props_octal (GbkProps *p)
{
  return gtk_label_get_text (p->octal);
}
