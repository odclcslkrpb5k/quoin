/* User interface for a running job: a progress window (shown only if the job takes a
 * while), the conflict / error / untrashable prompts, and an error dialog at the end.
 * All dialogs are asynchronous (no nested main loops). The UI owns a reference to the job;
 * it never depends on the window that started it. */
#pragma once

#include "job.h"

/* Start `job` with its UI. `parent` (may be nullptr) is only used to place dialogs. */
void gbk_jobui_start (GbkJob *job, GtkWindow *parent);

/* A window of ours that may be on screen with no file window open (a job's progress, its
 * questions' parent, an error, a notice): it joins the running application, which then
 * doesn't end while the window is up. Otherwise the process could end under it, with the
 * window and its callbacks still alive. `app` nullptr: the default application. */
void gbk_jobui_keep_app (GtkWindow *w, GtkApplication *app);

/* Show a modeless error message (also used by the window for its own errors). */
void gbk_jobui_error (GtkWindow *parent, const char *title, const char *message);

/* The answers of a job's question dialogs (tests answer them with gtk_dialog_response). */
enum
{
  GBK_JOBUI_REPLACE = 1,
  GBK_JOBUI_SKIP,
  GBK_JOBUI_KEEP_BOTH,
  GBK_JOBUI_DELETE
};

/* How long a job runs before its progress window appears. */
#define GBK_JOBUI_DELAY_MS 500
