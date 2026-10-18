#include "edits.h"
#include "notify.h"
#include "lifetime.h"
#include "window.h"

typedef struct
{
  GbkContext *ctx;
  GbkLoc *start;
  GbkAddress remote;     /* host:folder on the command line (host nullptr: none) */
} App;

/* Also the icon's name (data/icons), as Galley's is. */
#define APP_ID "io.github.odclcslkrpb5k.Quoin"

static void
on_activate (GtkApplication *gtk_app, App *app)
{
  gtk_window_set_default_icon_name (APP_ID);
  gbk_window_install_accels (gtk_app);
  gbk_edits_init (gtk_app, app->ctx);   /* saves back to other machines; unsent ones offered */

  GbkWindow *w = gbk_window_new (gtk_app, app->ctx, app->start);
  gtk_widget_show_all (GTK_WIDGET (w));
  if (app->remote.host != nullptr)
    gbk_window_open_address (w, &app->remote);   /* says "Connecting…" until it can go */

  for (guint i = 0; i < app->ctx->config_errors->len; i++)
    g_printerr ("quoin: config: %s\n",
                (const char *) g_ptr_array_index (app->ctx->config_errors, i));
}

int
main (int argc, char **argv)
{
  if (argc == 2 && g_str_equal (argv[1], "--version"))
    {
      printf ("quoin %s\n", GBK_VERSION);
      return 0;
    }
  if (argc > 2)
    {
      fprintf (stderr, "usage: quoin [FOLDER | HOST:FOLDER]\n");
      return 2;
    }

  App app = { .ctx = gbk_context_new (nullptr) };
  /* Relative paths resolve against where we were started; default: the current dir. */
  g_autofree char *cwd = g_get_current_dir ();
  /* host:folder (scp's rule) opens there; the window starts here meanwhile. */
  GbkAddress a = { 0 };
  if (argc == 2 && gbk_address_parse (argv[1], &a) && a.host != nullptr)
    app.remote = a;
  else
    gbk_address_clear (&a);
  g_autofree char *start
      = g_canonicalize_filename (argc == 2 && app.remote.host == nullptr ? argv[1] : cwd, cwd);
  app.start = gbk_loc_new_local (start);

  /* NON_UNIQUE: each invocation is its own process, so a crash can't take other windows
   * with it. We parse argv ourselves; GApplication gets none of it. */
  GtkApplication *gtk_app = gtk_application_new (APP_ID, G_APPLICATION_NON_UNIQUE);
  gbk_connect (G_OBJECT (gtk_app), gtk_app, "activate", G_CALLBACK (on_activate), &app);
  char *no_args[] = { argv[0], nullptr };
  int status = g_application_run (G_APPLICATION (gtk_app), 1, no_args);
  gbk_edits_shutdown ();
  gbk_notify_shutdown ();
  g_object_unref (gtk_app);
  gbk_context_unref (app.ctx);
  gbk_loc_unref (app.start);
  gbk_address_clear (&app.remote);

  /* Everything has been torn down; any registration still alive is a lifetime bug.
   * (Scanner workers are detached and may still be finishing; they hold no callbacks.)
   * Said, not fatal: at this point nothing can go wrong for the person any more, and a
   * core dump on the way out reads as a crash (user, 2026-10-18). Tests check the same
   * thing themselves, fatally; under G_DEBUG=fatal-criticals this aborts too. */
  if (gbk_lifetime_live () != 0)
    g_critical ("%d signal connection(s)/source(s) outlived their owners (a bug: please report "
                "it)",
                gbk_lifetime_live ());
  return status;
}
