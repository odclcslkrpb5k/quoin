#include "appchoose.h"
#include "jobui.h"
#include "lifetime.h"

#define KEY_ASK "gbk-appchoose"

static GAppInfo *pick_for_test;

typedef struct
{
  char *mime;            /* nullptr: several types */
  GtkToggleButton *always; /* nullptr: several types */
  GbkAppChosen done;
  void *data;
  GDestroyNotify destroy;
} Ask;

static void
ask_free (Ask *a)
{
  if (a->destroy != nullptr)
    a->destroy (a->data);
  g_free (a->mime);
  g_free (a);
}

/* Make `app` the default for the type, or only one of its recommended applications. */
static void
remember (GtkWindow *parent, GAppInfo *app, const char *mime, bool always)
{
  g_autoptr (GError) error = nullptr;
  if (always)
    {
      if (!g_app_info_set_as_default_for_type (app, mime, &error))
        gbk_jobui_error (parent, "Couldn't make it the default", error->message);
    }
  else if (!g_app_info_set_as_last_used_for_type (app, mime, &error))
    g_warning ("couldn't remember %s for %s: %s", g_app_info_get_display_name (app), mime,
               error->message);
}

static void
on_response (GtkDialog *d, int response, void *unused G_GNUC_UNUSED)
{
  Ask *a = g_object_get_data (G_OBJECT (d), KEY_ASK);
  GAppInfo *app = nullptr;
  if (response == GTK_RESPONSE_OK)
    app = pick_for_test != nullptr ? g_object_ref (pick_for_test)
                                   : gtk_app_chooser_get_app_info (GTK_APP_CHOOSER (d));
  /* Out of sight first: what the choice starts may take a moment. */
  gtk_widget_hide (GTK_WIDGET (d));
  if (app != nullptr)
    {
      GtkWindow *parent = gtk_window_get_transient_for (GTK_WINDOW (d));
      if (a->mime != nullptr)
        remember (parent, app, a->mime, gtk_toggle_button_get_active (a->always));
      a->done (app, a->data);
      g_object_unref (app);
    }
  gtk_widget_destroy (GTK_WIDGET (d));
}

void
gbk_appchoose (GtkWindow *parent, const char *what, const char *mime, GbkAppChosen done, void *data,
               GDestroyNotify destroy)
{
  Ask *a = g_new0 (Ask, 1);
  a->mime = g_strdup (mime);
  a->done = done;
  a->data = data;
  a->destroy = destroy;
  /* Several types: no one type's applications are recommended, so show them all. */
  GtkWidget *d = gtk_app_chooser_dialog_new_for_content_type (
      parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      mime != nullptr ? mime : "application/octet-stream");
  g_autofree char *heading = g_strdup_printf ("Open %s with", what);
  gtk_app_chooser_dialog_set_heading (GTK_APP_CHOOSER_DIALOG (d), heading);
  GtkWidget *chooser = gtk_app_chooser_dialog_get_widget (GTK_APP_CHOOSER_DIALOG (d));
  if (mime == nullptr)
    gtk_app_chooser_widget_set_show_all (GTK_APP_CHOOSER_WIDGET (chooser), true);
  else
    {
      g_autofree char *desc = g_content_type_get_description (mime);
      g_autofree char *label = g_strdup_printf ("_Always use it for this type (%s)", desc);
      GtkWidget *always = gtk_check_button_new_with_mnemonic (label);
      GtkWidget *area = gtk_dialog_get_content_area (GTK_DIALOG (d));
      gtk_box_pack_end (GTK_BOX (area), always, false, false, 6);
      a->always = GTK_TOGGLE_BUTTON (always);
    }
  g_object_set_data_full (G_OBJECT (d), KEY_ASK, a, (GDestroyNotify) ask_free);
  gbk_connect (G_OBJECT (d), d, "response", G_CALLBACK (on_response), nullptr);
  gtk_widget_show_all (d);
}

void
gbk_appchoose_set_pick_for_test (GAppInfo *app)
{
  g_clear_object (&pick_for_test);
  pick_for_test = app != nullptr ? g_object_ref (app) : nullptr;
}
