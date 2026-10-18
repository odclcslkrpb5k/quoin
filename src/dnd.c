#include "dnd.h"
#include "cliptext.h"

GtkTargetList *
gbk_dnd_source_targets (void)
{
  GtkTargetList *t = gtk_target_list_new (nullptr, 0);
  gtk_target_list_add (t, gdk_atom_intern_static_string ("text/uri-list"), 0, GBK_DND_URIS);
  gtk_target_list_add_text_targets (t, GBK_DND_TEXT);
  return t;
}

GtkTargetList *
gbk_dnd_dest_targets (void)
{
  GtkTargetList *t = gtk_target_list_new (nullptr, 0);
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
  g_autoptr (GPtrArray) paths = gbk_clip_parse ((const char *) data, (size_t) len, false, &cut);
  return gbk_loc_array_from_local (paths);
}

bool
gbk_dnd_set_data (GtkSelectionData *sd, guint info, const GPtrArray *locs)
{
  g_autoptr (GPtrArray) paths = gbk_loc_array_as_local (locs);
  if (paths == nullptr)
    return false;
  if (info == GBK_DND_URIS)
    {
      g_autofree char *uris = gbk_clip_format_uri_list (paths);
      gtk_selection_data_set (sd, gtk_selection_data_get_target (sd), 8, (const guchar *) uris,
                              (int) strlen (uris));
    }
  else
    {
      g_autofree char *text = gbk_clip_format_text (paths);
      gtk_selection_data_set_text (sd, text, -1);
    }
  return true;
}
