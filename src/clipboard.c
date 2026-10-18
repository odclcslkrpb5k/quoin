#include "clipboard.h"
#include "cliptext.h"
#include "lifetime.h"

#define GNOME_TARGET "x-special/gnome-copied-files"
#define URI_TARGET "text/uri-list"
#define LOCS_TARGET "x-special/quoin-locations"   /* Quoin to Quoin: see cliptext.h */

enum
{
  INFO_GNOME,
  INFO_URIS,
  INFO_TEXT,
  INFO_LOCS,
};

typedef struct
{
  char *gnome;             /* nullptr: some item is on another machine */
  char *uris;
  char *text;
  char *locs;
} Payload;

/* The payload we currently own, if any (main thread only). */
static Payload *owned;

static void
payload_get (GtkClipboard *cb G_GNUC_UNUSED, GtkSelectionData *sd, guint info, void *data)
{
  Payload *p = data;
  switch (info)
    {
    case INFO_GNOME:
      gtk_selection_data_set (sd, gtk_selection_data_get_target (sd), 8, (const guchar *) p->gnome,
                              (int) strlen (p->gnome));
      break;
    case INFO_URIS:
      gtk_selection_data_set (sd, gtk_selection_data_get_target (sd), 8, (const guchar *) p->uris,
                              (int) strlen (p->uris));
      break;
    case INFO_TEXT: gtk_selection_data_set_text (sd, p->text, -1); break;
    case INFO_LOCS:
      gtk_selection_data_set (sd, gtk_selection_data_get_target (sd), 8, (const guchar *) p->locs,
                              (int) strlen (p->locs));
      break;
    default: break;
    }
}

static void
payload_clear (GtkClipboard *cb G_GNUC_UNUSED, void *data)
{
  Payload *p = data;
  if (owned == p)
    owned = nullptr;
  g_free (p->gnome);
  g_free (p->uris);
  g_free (p->text);
  g_free (p->locs);
  g_free (p);
}

bool
gbk_clipboard_set_files (GtkWidget *widget, const GPtrArray *locs, bool cut)
{
  g_autoptr (GPtrArray) paths = gbk_loc_array_as_local (locs);
  if (paths == nullptr)
    {
      /* Something is on another machine: only other Quoin windows can take it, and text
       * names it as host:/path (never a bare path, which would name a file here). */
      static char t_locs[] = LOCS_TARGET, t_utf8[] = "UTF8_STRING",
                  t_plain_utf8[] = "text/plain;charset=utf-8", t_plain[] = "text/plain";
      const GtkTargetEntry targets[] = {
        { t_locs, 0, INFO_LOCS },
        { t_utf8, 0, INFO_TEXT },
        { t_plain_utf8, 0, INFO_TEXT },
        { t_plain, 0, INFO_TEXT },
      };
      Payload *p = g_new0 (Payload, 1);
      p->locs = gbk_clip_format_locs (locs, cut);
      GString *text = g_string_new ("");
      for (guint i = 0; i < locs->len; i++)
        {
          g_autofree char *shown = gbk_loc_display (g_ptr_array_index (locs, i));
          g_string_append_printf (text, "%s%s", i > 0 ? "\n" : "", shown);
        }
      p->text = g_string_free (text, false);
      GtkClipboard *cb = gtk_widget_get_clipboard (widget, GDK_SELECTION_CLIPBOARD);
      if (gtk_clipboard_set_with_data (cb, targets, G_N_ELEMENTS (targets), payload_get,
                                       payload_clear, p))
        owned = p;
      else
        payload_clear (cb, p);
      return true;
    }
  /* GtkTargetEntry wants mutable strings. */
  static char t_locs[] = LOCS_TARGET, t_gnome[] = GNOME_TARGET, t_uris[] = URI_TARGET,
              t_utf8[] = "UTF8_STRING", t_plain_utf8[] = "text/plain;charset=utf-8",
              t_plain[] = "text/plain";
  const GtkTargetEntry targets[] = {
    { t_locs, 0, INFO_LOCS }, { t_gnome, 0, INFO_GNOME },     { t_uris, 0, INFO_URIS },
    { t_utf8, 0, INFO_TEXT }, { t_plain_utf8, 0, INFO_TEXT }, { t_plain, 0, INFO_TEXT },
  };
  Payload *p = g_new0 (Payload, 1);
  p->locs = gbk_clip_format_locs (locs, cut);
  p->gnome = gbk_clip_format_gnome (paths, cut);
  p->uris = gbk_clip_format_uri_list (paths);
  p->text = gbk_clip_format_text (paths);
  GtkClipboard *cb = gtk_widget_get_clipboard (widget, GDK_SELECTION_CLIPBOARD);
  if (gtk_clipboard_set_with_data (cb, targets, G_N_ELEMENTS (targets), payload_get, payload_clear,
                                   p))
    owned = p;
  else
    payload_clear (cb, p);
  return true;
}

void
gbk_clipboard_set_text (GtkWidget *widget, const char *text)
{
  /* Taking ownership runs payload_clear on any files payload of ours, resetting `owned`. */
  gtk_clipboard_set_text (gtk_widget_get_clipboard (widget, GDK_SELECTION_CLIPBOARD), text, -1);
}

void
gbk_clipboard_clear_if_ours (GtkWidget *widget)
{
  if (owned != nullptr)
    gtk_clipboard_clear (gtk_widget_get_clipboard (widget, GDK_SELECTION_CLIPBOARD));
}

typedef struct
{
  GObject *owner;          /* borrowed: the request wrapper checks it's alive */
  GtkWidget *widget;       /* borrowed, same lifetime as owner */
  GbkFilesFunc fn;
  void *data;
} Get;

static void
got_uris (GtkSelectionData *sd, void *p)
{
  Get *g = p;
  int len = gtk_selection_data_get_length (sd);
  bool cut = false;
  g_autoptr (GPtrArray) paths = gbk_clip_parse ((const char *) gtk_selection_data_get_data (sd),
                                                len > 0 ? (size_t) len : 0, false, &cut);
  g_autoptr (GPtrArray) locs = gbk_loc_array_from_local (paths);
  g->fn (locs, false, g->data);
}

static void
got_gnome (GtkSelectionData *sd, void *p)
{
  Get *g = p;
  int len = gtk_selection_data_get_length (sd);
  if (len > 0)
    {
      bool cut = false;
      g_autoptr (GPtrArray) paths = gbk_clip_parse ((const char *) gtk_selection_data_get_data (sd),
                                                    (size_t) len, true, &cut);
      g_autoptr (GPtrArray) locs = gbk_loc_array_from_local (paths);
      g->fn (locs, cut, g->data);
      return;
    }
  /* Not from a file manager: try a plain URI list (always a copy). Owner is alive here
   * (we were called), so handing a copy of the request on is safe. */
  Get *next = g_memdup2 (g, sizeof *g);
  gbk_clipboard_request (g->owner, gtk_widget_get_clipboard (g->widget, GDK_SELECTION_CLIPBOARD),
                         gdk_atom_intern_static_string (URI_TARGET), got_uris, next, g_free);
}

/* Another Quoin window's locations first (they may be on other machines); then what file
 * managers put there. */
static void
got_locs (GtkSelectionData *sd, void *p)
{
  Get *g = p;
  int len = gtk_selection_data_get_length (sd);
  if (len > 0)
    {
      bool cut = false;
      g_autoptr (GPtrArray) locs = gbk_clip_parse_locs (
          (const char *) gtk_selection_data_get_data (sd), (size_t) len, &cut);
      g->fn (locs, cut, g->data);
      return;
    }
  Get *next = g_memdup2 (g, sizeof *g);
  gbk_clipboard_request (g->owner, gtk_widget_get_clipboard (g->widget, GDK_SELECTION_CLIPBOARD),
                         gdk_atom_intern_static_string (GNOME_TARGET), got_gnome, next, g_free);
}

void
gbk_clipboard_get_files (GObject *owner, GtkWidget *widget, GbkFilesFunc fn, void *data)
{
  Get *g = g_new0 (Get, 1);
  g->owner = owner;
  g->widget = widget;
  g->fn = fn;
  g->data = data;
  gbk_clipboard_request (owner, gtk_widget_get_clipboard (widget, GDK_SELECTION_CLIPBOARD),
                         gdk_atom_intern_static_string (LOCS_TARGET), got_locs, g, g_free);
}
