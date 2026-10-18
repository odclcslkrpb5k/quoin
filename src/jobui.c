#include "jobui.h"
#include "lifetime.h"

#define KEY_JOB "gbk-job"

typedef struct
{
  GtkWidget *window;       /* the progress window; owns this struct (KEY_UI) */
  GtkLabel *title;
  GtkLabel *current;
  GtkProgressBar *bar;
  GbkJob *job;             /* ref */
  guint show_timeout;
} Ui;

static void
ui_free (Ui *ui)
{
  g_object_unref (ui->job);
  g_free (ui);
}

static const char *
verb (GbkJob *job)
{
  switch (gbk_job_kind (job))
    {
    case GBK_JOB_COPY: return "Copying";
    case GBK_JOB_MOVE: return "Moving";
    case GBK_JOB_TRASH: return "Moving to Trash";
    case GBK_JOB_DELETE: return "Deleting";
    case GBK_JOB_RENAME: return "Renaming";
    case GBK_JOB_CREATE_FILE: return "Creating File";
    case GBK_JOB_CREATE_FOLDER: return "Creating Folder";
    case GBK_JOB_SET_EXECUTABLE:
    case GBK_JOB_CLEAR_EXECUTABLE:
    case GBK_JOB_PERMISSIONS: return "Changing Permissions";
    case GBK_JOB_INFO: return "Reading Details";
    case GBK_JOB_FETCH: return "Downloading";
    case GBK_JOB_UPLOAD: return "Uploading";
    default: return "Working";
    }
}

static void
on_error_response (GtkDialog *d, int response G_GNUC_UNUSED, void *data G_GNUC_UNUSED)
{
  gtk_widget_destroy (GTK_WIDGET (d));
}

void
gbk_jobui_keep_app (GtkWindow *w, GtkApplication *app)
{
  if (app == nullptr && GTK_IS_APPLICATION (g_application_get_default ()))
    app = GTK_APPLICATION (g_application_get_default ());
  if (app != nullptr)
    gtk_window_set_application (w, app);
}

void
gbk_jobui_error (GtkWindow *parent, const char *title, const char *message)
{
  GtkWidget *d = gtk_message_dialog_new (parent, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                                         GTK_BUTTONS_CLOSE, "%s", title);
  gbk_jobui_keep_app (GTK_WINDOW (d), nullptr);
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (d), "%s", message);
  gbk_connect (G_OBJECT (d), d, "response", G_CALLBACK (on_error_response), nullptr);
  gtk_widget_show (d);
}

/* ---- progress ---- */

static bool
show_progress (void *data)
{
  Ui *ui = data;
  ui->show_timeout = 0;
  gtk_widget_show_all (ui->window);
  return false;
}

static void
on_progress (GbkJob *job, Ui *ui)
{
  double f = gbk_job_fraction (job);
  if (f < 0)
    gtk_progress_bar_pulse (ui->bar);
  else
    gtk_progress_bar_set_fraction (ui->bar, f);
  gtk_label_set_text (ui->current, gbk_job_current (job));
}

static void
on_cancel_clicked (GtkButton *b G_GNUC_UNUSED, Ui *ui)
{
  gbk_job_cancel (ui->job);
}

static gboolean
on_progress_delete (GtkWidget *w G_GNUC_UNUSED, GdkEvent *e G_GNUC_UNUSED, Ui *ui)
{
  gbk_job_cancel (ui->job);      /* closing the progress window cancels; the window goes */
  return true;                   /* away when the job reports it has finished */
}

static void
on_finished (GbkJob *job G_GNUC_UNUSED, GError *error, Ui *ui)
{
  GtkWindow *transient = gtk_window_get_transient_for (GTK_WINDOW (ui->window));
  if (error != nullptr && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_autofree char *title = g_strdup_printf ("%s failed", verb (ui->job));
      gbk_jobui_error (transient, title, error->message);
    }
  gtk_widget_destroy (ui->window);   /* frees ui */
}

/* ---- questions ---- */

#define RESP_REPLACE GBK_JOBUI_REPLACE
#define RESP_SKIP GBK_JOBUI_SKIP
#define RESP_KEEP_BOTH GBK_JOBUI_KEEP_BOTH
#define RESP_DELETE GBK_JOBUI_DELETE

static void
on_question_response (GtkDialog *d, int response, GbkJob *job)
{
  GtkToggleButton *all = g_object_get_data (G_OBJECT (d), "gbk-all");
  bool apply_all = all != nullptr && gtk_toggle_button_get_active (all);
  switch (response)
    {
    case RESP_REPLACE:
    case RESP_DELETE: gbk_job_answer (job, GBK_CHOICE_REPLACE, apply_all); break;
    case RESP_SKIP: gbk_job_answer (job, GBK_CHOICE_SKIP, apply_all); break;
    case RESP_KEEP_BOTH: gbk_job_answer (job, GBK_CHOICE_KEEP_BOTH, apply_all); break;
    default: gbk_job_cancel (job); break;        /* Cancel, Escape, or window closed */
    }
  gtk_widget_destroy (GTK_WIDGET (d));
}

static void
on_question (GbkJob *job, GbkQuestion *q, Ui *ui)
{
  /* A question needs a visible parent now, whatever the delay. */
  gbk_source_cancel (&ui->show_timeout);
  gtk_widget_show_all (ui->window);

  g_autofree char *name = g_filename_display_basename (q->path);
  GtkWidget *d;
  switch (q->kind)
    {
    case GBK_QUESTION_CONFLICT:
      {
        g_autofree char *where = g_path_get_dirname (q->other);
        g_autofree char *where_shown = g_filename_display_name (where);
        d = gtk_message_dialog_new (GTK_WINDOW (ui->window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                    q->other_is_dir ? "A folder named “%s” already exists"
                                                    : "A file named “%s” already exists",
                                    name);
        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (d), "in %s", where_shown);
        gtk_dialog_add_buttons (GTK_DIALOG (d), "_Cancel", GTK_RESPONSE_CANCEL, "_Skip", RESP_SKIP,
                                "_Keep Both", RESP_KEEP_BOTH,
                                q->other_is_dir ? "_Merge" : "_Replace", RESP_REPLACE, nullptr);
        GtkWidget *all = gtk_check_button_new_with_mnemonic ("_Apply to all");
        gtk_box_pack_end (GTK_BOX (gtk_message_dialog_get_message_area (GTK_MESSAGE_DIALOG (d))),
                          all, false, false, 0);
        g_object_set_data (G_OBJECT (d), "gbk-all", all);
        gtk_dialog_set_default_response (GTK_DIALOG (d), RESP_SKIP);
        break;
      }
    case GBK_QUESTION_ERROR:
      d = gtk_message_dialog_new (GTK_WINDOW (ui->window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                  GTK_MESSAGE_ERROR, GTK_BUTTONS_NONE, "%s", q->other);
      gtk_dialog_add_buttons (GTK_DIALOG (d), "_Cancel", GTK_RESPONSE_CANCEL, "_Skip", RESP_SKIP,
                              nullptr);
      gtk_dialog_set_default_response (GTK_DIALOG (d), RESP_SKIP);
      break;
    case GBK_QUESTION_UNTRASHABLE:
      d = gtk_message_dialog_new (GTK_WINDOW (ui->window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                  GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                  "“%s” can't be moved to the trash", name);
      gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (d),
                                                "Delete it permanently instead?");
      gtk_dialog_add_buttons (GTK_DIALOG (d), "_Cancel", GTK_RESPONSE_CANCEL, "_Skip", RESP_SKIP,
                              "_Delete Permanently", RESP_DELETE, nullptr);
      gtk_dialog_set_default_response (GTK_DIALOG (d), RESP_SKIP);
      break;
    case GBK_QUESTION_UPLOAD:
      /* Nothing is lost whatever the answer: "Don't Upload" keeps the copy here. */
      d = gtk_message_dialog_new (GTK_WINDOW (ui->window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                  GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                  q->other_is_dir
                                      ? "“%s” has other names (hard links) on the other machine"
                                      : "“%s” changed on the other machine since you opened it",
                                  name);
      gtk_message_dialog_format_secondary_text (
          GTK_MESSAGE_DIALOG (d), "%s",
          q->other_is_dir ? "Replacing it with your version separates it from them. Keep Both "
                            "saves yours beside it instead."
                          : "Replace it with your version, or Keep Both: yours is saved beside "
                            "it under a new name.");
      gtk_dialog_add_buttons (GTK_DIALOG (d), "Do_n't Upload", RESP_SKIP, "_Keep Both",
                              RESP_KEEP_BOTH, "_Replace", RESP_REPLACE, nullptr);
      gtk_dialog_set_default_response (GTK_DIALOG (d), RESP_KEEP_BOTH);
      break;
    default: gbk_fatal ("bad question kind %d", (int) q->kind);
    }
  g_object_set_data_full (G_OBJECT (d), KEY_JOB, g_object_ref (job), g_object_unref);
  gbk_connect (G_OBJECT (d), d, "response", G_CALLBACK (on_question_response), job);
  gtk_widget_show_all (d);
}

void
gbk_jobui_start (GbkJob *job, GtkWindow *parent)
{
  Ui *ui = g_new0 (Ui, 1);
  ui->job = g_object_ref (job);
  ui->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  g_object_set_data_full (G_OBJECT (ui->window), "gbk-ui", ui, (GDestroyNotify) ui_free);
  GtkWindow *win = GTK_WINDOW (ui->window);
  gtk_window_set_transient_for (win, parent);
  gbk_jobui_keep_app (win, nullptr);   /* a running job outlives its file window */
  gtk_window_set_title (win, verb (job));
  gtk_window_set_default_size (win, 420, -1);
  gtk_window_set_resizable (win, false);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width (GTK_CONTAINER (box), 12);
  g_autofree char *title = g_strdup_printf ("%s %zu %s…", verb (job), gbk_job_n_sources (job),
                                            gbk_job_n_sources (job) == 1 ? "item" : "items");
  ui->title = GTK_LABEL (gtk_label_new (title));
  gtk_label_set_xalign (ui->title, 0);
  ui->bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  ui->current = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (ui->current, 0);
  gtk_label_set_ellipsize (ui->current, PANGO_ELLIPSIZE_MIDDLE);
  GtkWidget *cancel = gtk_button_new_with_mnemonic ("_Cancel");
  gtk_widget_set_halign (cancel, GTK_ALIGN_END);
  gtk_box_pack_start (GTK_BOX (box), GTK_WIDGET (ui->title), false, false, 0);
  gtk_box_pack_start (GTK_BOX (box), GTK_WIDGET (ui->bar), false, false, 0);
  gtk_box_pack_start (GTK_BOX (box), GTK_WIDGET (ui->current), false, false, 0);
  gtk_box_pack_start (GTK_BOX (box), cancel, false, false, 0);
  gtk_container_add (GTK_CONTAINER (ui->window), box);

  GObject *owner = G_OBJECT (ui->window);
  gbk_connect (owner, cancel, "clicked", G_CALLBACK (on_cancel_clicked), ui);
  gbk_connect (owner, ui->window, "delete-event", G_CALLBACK (on_progress_delete), ui);
  gbk_connect (owner, job, "progress", G_CALLBACK (on_progress), ui);
  gbk_connect (owner, job, "question", G_CALLBACK (on_question), ui);
  gbk_connect (owner, job, "finished", G_CALLBACK (on_finished), ui);
  ui->show_timeout = gbk_timeout_add (owner, GBK_JOBUI_DELAY_MS, show_progress, ui);
  gbk_job_start (job);
}
