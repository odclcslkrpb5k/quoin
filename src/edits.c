#include "edits.h"
#include "jobui.h"
#include "notify.h"
#include "lifetime.h"
#include "window.h"

#define RETRY_MIN_MS 2000
#define RETRY_MAX_MS 60000
#define GONE_KEEP_TRYING_MS ((int) 30 * 60 * 1000)  /* app gone, upload not possible: this long */
#define RESP_UPLOAD 1
#define RESP_SHOW 2
#define RESP_DISCARD 3

typedef enum
{
  APP_STARTING,           /* launched; not seen yet, within the grace period */
  APP_RUNNING,            /* seen */
  APP_GONE,               /* seen, then no longer */
  APP_UNKNOWN,            /* never seen: watched while windows are open */
} AppState;

typedef struct
{
  uint64_t id;
  GbkEditRecord *rec;
  int lock_fd;
  char *asked;            /* the path there as opened */
  char *exe;              /* the app's executable name, or nullptr */
  GPid pid;               /* the launched process, or 0 */
  AppState app;
  int64_t started_us;
  int64_t gone_us;
  GbkFileToken seen;      /* the copy at the last look */
  bool seen_valid;
  GbkJob *job;            /* an upload running */
  GbkFileToken sending;   /* the copy as it is being uploaded */
  GbkFileToken failed;    /* a version whose upload failed: not tried again */
  bool has_failed;
  int64_t retry_at_us;    /* not connected: when to look again */
  int retry_ms;
  bool blocked;           /* the machine answering isn't the one it came from */
  bool adopted;           /* left by an earlier Quoin: ends once uploaded */
} Edit;

/* Copies an earlier Quoin left changed and unsent, with their locks. */
typedef struct
{
  GbkEditRecord *rec;
  int lock_fd;
} Unsent;

static struct
{
  GtkApplication *app;
  GObject *owner;         /* of every callback here: they all end with it (shutdown) */
  GbkContext *ctx;
  char *cache;
  GPtrArray *edits;       /* Edit* */
  guint timer;
  int ticks;
  bool held;
  int64_t windows_gone_us; /* when the last window closed; 0 while one is open */
  uint64_t next_id;
  int poll_ms, grace_ms, after_windows_ms;
  int in_front_test;      /* tests: 0 as it is, 1 in front, 2 not */
  char *activity;         /* what every window's status bar says about edits */
  GbkEditsAnswer (*unsent_answer) (guint n);
  /* The startup scan runs on a thread; the timer picks up what it found. */
  GMutex scan_lock;
  bool scan_running;
  GPtrArray *found;       /* Unsent*, set by the scan */
} E;

static void
unsent_free (Unsent *u)
{
  if (u->rec == nullptr && u->lock_fd < 0)
    {
      g_free (u);
      return;
    }
  if (u->lock_fd >= 0)
    close (u->lock_fd);
  gbk_edit_record_free (u->rec);
  g_free (u);
}

static void
edit_free (Edit *e)
{
  g_clear_object (&e->job);   /* one still running keeps itself alive */
  if (e->lock_fd >= 0)
    close (e->lock_fd);
  gbk_edit_record_free (e->rec);
  g_free (e->asked);
  g_free (e->exe);
  g_free (e);
}

char *
gbk_edits_cache (void)
{
  char *dir = g_build_filename (g_get_user_cache_dir (), "quoin", "remote", nullptr);
  g_mkdir_with_parents (dir, 0700);
  return dir;
}

void
gbk_edits_set_timing_for_test (int poll_ms, int grace_ms, int after_windows_ms)
{
  E.poll_ms = poll_ms;
  E.grace_ms = grace_ms;
  E.after_windows_ms = after_windows_ms;
}

void
gbk_edits_set_unsent_answer_for_test (GbkEditsAnswer (*answer) (guint n))
{
  E.unsent_answer = answer;
}

static int
poll_ms (void)
{
  return E.poll_ms > 0 ? E.poll_ms : GBK_EDITS_POLL_MS;
}

static int
grace_ms (void)
{
  return E.grace_ms > 0 ? E.grace_ms : GBK_EDITS_GRACE_MS;
}

static int
after_windows_ms (void)
{
  return E.after_windows_ms > 0 ? E.after_windows_ms : GBK_EDITS_AFTER_WINDOWS_MS;
}

static GtkWindow *
some_window (void)
{
  return E.app != nullptr ? gtk_application_get_active_window (E.app) : nullptr;
}

static void
save (const Edit *e)
{
  g_autoptr (GError) error = nullptr;
  if (!gbk_edit_record_save (E.cache, e->rec, &error))
    g_warning ("saving an edit's record: %s", error->message);
}

/* ---- is the app still running? ---- */

/* Launchers and interpreters: their names say nothing about which app is running. */
static bool
generic (const char *name)
{
  static const char *const names[]
      = { "sh",      "bash", "dash", "env",      "flatpak",  "snap",  "python",
          "python3", "perl", "gio",  "exo-open", "xdg-open", "bwrap", "systemd-run" };
  for (size_t i = 0; i < G_N_ELEMENTS (names); i++)
    if (g_str_equal (name, names[i]))
      return true;
  return false;
}

/* A process of ours: its executable's name and its command name (comm), unless a zombie. */
static bool
process_names (const char *pid_dir, char **exe, char **comm)
{
  *exe = *comm = nullptr;
  g_autofree char *stat_path = g_build_filename (pid_dir, "stat", nullptr);
  g_autofree char *stat_text = nullptr;
  if (!g_file_get_contents (stat_path, &stat_text, nullptr, nullptr))
    return false;
  const char *close_paren = strrchr (stat_text, ')');
  if (close_paren == nullptr || close_paren[1] != ' ' || close_paren[2] == 'Z')
    return false;
  g_autofree char *exe_link = g_build_filename (pid_dir, "exe", nullptr);
  g_autofree char *target = g_file_read_link (exe_link, nullptr);
  if (target != nullptr)
    {
      char *deleted = g_strrstr (target, " (deleted)");
      if (deleted != nullptr)
        *deleted = '\0';
      *exe = g_path_get_basename (target);
    }
  g_autofree char *comm_path = g_build_filename (pid_dir, "comm", nullptr);
  if (g_file_get_contents (comm_path, comm, nullptr, nullptr))
    g_strstrip (*comm);
  return true;
}

/* Names of this user's running processes (executables and commands). */
static GHashTable *
running_names (void)
{
  GHashTable *names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nullptr);
  g_autoptr (GDir) d = g_dir_open ("/proc", 0, nullptr);
  const char *name;
  uid_t me = getuid ();
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    {
      if (!g_ascii_isdigit (name[0]))
        continue;
      g_autofree char *dir = g_build_filename ("/proc", name, nullptr);
      struct stat st;
      char *exe, *comm;
      if (stat (dir, &st) != 0 || st.st_uid != me || !process_names (dir, &exe, &comm))
        continue;
      if (exe != nullptr)
        g_hash_table_add (names, exe);
      if (comm != nullptr)
        g_hash_table_add (names, comm);
    }
  return names;
}

static bool
pid_running (GPid pid, char **exe)
{
  *exe = nullptr;
  g_autofree char *dir = g_strdup_printf ("/proc/%d", (int) pid);
  g_autofree char *comm = nullptr;
  return process_names (dir, exe, &comm);
}

/* Does an app's executable name match a process name? Command names are cut at 15. */
static bool
name_running (GHashTable *names, const char *exe)
{
  if (g_hash_table_contains (names, exe))
    return true;
  g_autofree char *cut = g_strndup (exe, 15);
  return strlen (exe) > 15 && g_hash_table_contains (names, cut);
}

static void
judge_app (Edit *e, GHashTable *names, int64_t now)
{
  bool running = false;
  if (e->pid > 0)
    {
      g_autofree char *exe = nullptr;
      running = pid_running (e->pid, &exe);
      if (running && exe != nullptr && e->exe == nullptr && !generic (exe))
        e->exe = g_steal_pointer (&exe);   /* what the launch turned into */
      if (!running)
        e->pid = 0;
    }
  if (!running && e->exe != nullptr)
    running = name_running (names, e->exe);
  switch (e->app)
    {
    case APP_STARTING:
      if (running)
        e->app = APP_RUNNING;
      else if (now - e->started_us >= (int64_t) grace_ms () * 1000)
        e->app = APP_UNKNOWN;
      break;
    case APP_RUNNING:
      if (!running)
        {
          e->app = APP_GONE;
          e->gone_us = now;
        }
      break;
    case APP_GONE:
    case APP_UNKNOWN: break;
    default: gbk_fatal ("bad app state %d", (int) e->app);
    }
}

/* ---- news ---- */

void
gbk_edits_say (const char *text)
{
  g_free (E.activity);
  E.activity = g_strdup (text);
  for (GList *l = E.app != nullptr ? gtk_application_get_windows (E.app) : nullptr; l != nullptr;
       l = l->next)
    if (GBK_IS_WINDOW (l->data))
      gbk_window_set_activity (l->data, text);
}

const char *
gbk_edits_activity (void)
{
  return E.activity != nullptr ? E.activity : "";
}

void
gbk_edits_set_in_front_for_test (int in_front)
{
  E.in_front_test = in_front < 0 ? 0 : in_front > 0 ? 1 : 2;
}

/* Is a Quoin window the one in use? Then the status bar is enough. */
static bool
in_front (void)
{
  if (E.in_front_test != 0)
    return E.in_front_test == 1;
  for (GList *l = E.app != nullptr ? gtk_application_get_windows (E.app) : nullptr; l != nullptr;
       l = l->next)
    if (gtk_window_is_active (l->data) && gtk_widget_get_visible (l->data))
      return true;
  return false;
}

/* A desktop notification about `e`, unless Quoin is in front (it shows in the status bar). */
static void
tell (const Edit *e, GbkNotifyKind kind, const char *summary, const char *body)
{
  if (in_front ())
    return;
  g_autofree char *key = g_strdup_printf ("edit-%" G_GUINT64_FORMAT, e->id);
  gbk_notify (key, kind, summary, body);
}

/* ---- uploading ---- */

static void
on_upload_question (GbkJob *job G_GNUC_UNUSED, GbkQuestion *q G_GNUC_UNUSED, Edit *e)
{
  g_autofree char *name = g_filename_display_name (e->rec->name);
  g_autofree char *summary = g_strdup_printf ("Saving “%s” to %s needs you", name, e->rec->host);
  gbk_edits_say (summary);
  tell (e, GBK_NOTIFY_NORMAL, summary,
        "It changed there since you opened it. Quoin asks what to do.");
}

static void
on_uploaded (GbkJob *job, GError *error, Edit *e)
{
  GbkFileToken t;
  g_autofree char *name = g_filename_display_name (e->rec->name);
  if (error == nullptr || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      /* Uploaded, or declined ("Don't Upload", or the question closed): this version is
       * settled; the next save is asked about again. */
      if (error == nullptr && gbk_job_uploaded (job, &t))
        {
          const GPtrArray *made = gbk_job_created (job);
          g_autofree char *was = g_path_get_basename (e->rec->remote);
          g_autofree char *as = nullptr;
          if (made != nullptr && made->len == 1 && !g_str_equal (made->pdata[0], was))
            {
              /* Kept beside theirs: later saves go to ours. */
              g_autofree char *dir = g_path_get_dirname (e->rec->remote);
              g_free (e->rec->remote);
              e->rec->remote = g_build_filename (dir, made->pdata[0], nullptr);
              g_autofree char *shown = g_filename_display_name (made->pdata[0]);
              as = g_strdup_printf (" as “%s”", shown);
            }
          e->rec->base = t;
          g_autoptr (GDateTime) now = g_date_time_new_now_local ();
          g_autofree char *at = g_date_time_format (now, "%H:%M");
          g_autofree char *said = g_strdup_printf ("Saved %s to %s%s at %s", name, e->rec->host,
                                                   as != nullptr ? as : "", at);
          gbk_edits_say (said);
          g_autofree char *summary
              = g_strdup_printf ("Saved %s to %s%s", name, e->rec->host, as != nullptr ? as : "");
          tell (e, GBK_NOTIFY_LOW, summary, "");
        }
      else
        {
          g_autofree char *said = g_strdup_printf ("Not uploaded: %s (your copy stays here)", name);
          gbk_edits_say (said);
        }
      e->rec->synced = e->sending;
      e->has_failed = false;
      save (e);
    }
  else
    {
      /* This version isn't tried again; the next save is. */
      e->failed = e->sending;
      e->has_failed = true;
      g_autofree char *summary = g_strdup_printf ("Couldn't save %s to %s", name, e->rec->host);
      g_autofree char *said = g_strdup_printf ("%s: %s", summary, error->message);
      gbk_edits_say (said);
      tell (e, GBK_NOTIFY_NORMAL, summary, error->message);
    }
  g_clear_object (&e->job);
}

static void
refuse (Edit *e, const GbkHello *h)
{
  e->blocked = true;
  g_autofree char *name = g_filename_display_name (e->rec->name);
  g_autofree char *message = g_strdup_printf (
      "“%s” came from %s, but %s now answers as a different machine or user (%s, uid %u; it "
      "was %s, uid %u). Your copy stays here, and Quoin offers it again when it next starts.",
      name, e->rec->host, e->rec->host, h->machine_id, h->uid, e->rec->machine_id, e->rec->uid);
  g_autofree char *said
      = g_strdup_printf ("Not uploaded: %s (%s is a different machine now)", name, e->rec->host);
  gbk_edits_say (said);
  g_autofree char *summary = g_strdup_printf ("Not uploading %s", name);
  tell (e, GBK_NOTIFY_NORMAL, summary, message);
  gbk_jobui_error (some_window (), "Not uploaded", message);
}

static void
upload (Edit *e, const GbkFileToken *cur, int64_t now)
{
  g_autofree char *name = g_filename_display_name (e->rec->name);
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkConn) c = gbk_remote_conn (E.ctx->remote, e->rec->host, &error);
  if (c == nullptr || gbk_conn_state (c) != GBK_CONN_READY)
    {
      /* Not connected (yet): it connects meanwhile; no dialogs for that. */
      e->retry_ms = CLAMP (e->retry_ms * 2, RETRY_MIN_MS, RETRY_MAX_MS);
      e->retry_at_us = now + (int64_t) e->retry_ms * 1000;
      g_autofree char *said
          = g_strdup_printf ("Waiting for %s to save %s there", e->rec->host, name);
      gbk_edits_say (said);
      return;
    }
  e->retry_ms = 0;
  const GbkHello *h = gbk_conn_hello (c);
  if (!g_str_equal (h->machine_id, e->rec->machine_id) || h->uid != e->rec->uid)
    {
      refuse (e, h);
      return;
    }
  g_autoptr (GbkLoc) target = gbk_loc_new_remote (e->rec->host, e->rec->remote);
  g_autofree char *copy = gbk_edit_record_copy (E.cache, e->rec);
  e->sending = *cur;
  e->job = gbk_job_new_upload (copy, target, &e->rec->base);
  gbk_connect (E.owner, e->job, "question", G_CALLBACK (on_upload_question), e);
  gbk_connect (E.owner, e->job, "finished", G_CALLBACK (on_uploaded), e);
  g_autofree char *said = g_strdup_printf ("Saving %s to %s…", name, e->rec->host);
  gbk_edits_say (said);
  gbk_jobui_start (e->job, some_window ());
}

/* Changed and then still for one look: the app has finished saving. */
static void
look (Edit *e, int64_t now)
{
  if (e->job != nullptr || e->blocked)
    return;
  GbkFileToken cur;
  if (!gbk_edit_record_dirty (E.cache, e->rec, &cur))
    {
      e->seen_valid = false;
      return;
    }
  if (!e->seen_valid || !gbk_file_token_same (&cur, &e->seen))
    {
      e->seen = cur;
      e->seen_valid = true;
      return;
    }
  if (now < e->retry_at_us || (e->has_failed && gbk_file_token_same (&cur, &e->failed)))
    return;
  upload (e, &cur, now);
}

static bool
settled (const Edit *e)
{
  GbkFileToken cur;
  return e->blocked || !gbk_edit_record_dirty (E.cache, e->rec, &cur);
}

static bool
finished (const Edit *e, int64_t now)
{
  if (e->job != nullptr)
    return false;
  if (e->adopted)
    return settled (e) || e->has_failed;
  switch (e->app)
    {
    case APP_STARTING:
    case APP_RUNNING: return false;
    case APP_GONE: return settled (e) || now - e->gone_us >= (int64_t) GONE_KEEP_TRYING_MS * 1000;
    case APP_UNKNOWN:
      return E.windows_gone_us != 0
             && now - E.windows_gone_us >= (int64_t) after_windows_ms () * 1000;
    default: gbk_fatal ("bad app state %d", (int) e->app);
    }
}

/* ---- the timer ---- */

static void offer_unsent (GPtrArray *found);

static void
hold (bool on)
{
  if (on == E.held || E.app == nullptr)
    return;
  E.held = on;
  if (on)
    g_application_hold (G_APPLICATION (E.app));
  else
    g_application_release (G_APPLICATION (E.app));
}

static bool
tick (void *data G_GNUC_UNUSED)
{
  int64_t now = g_get_monotonic_time ();
  E.ticks++;
  g_mutex_lock (&E.scan_lock);
  GPtrArray *found = g_steal_pointer (&E.found);
  bool scanning = E.scan_running;
  g_mutex_unlock (&E.scan_lock);
  if (found != nullptr)
    offer_unsent (found);

  int every = MIN (GBK_EDITS_APP_CHECK_MS, grace_ms ());
  bool judge = (int64_t) E.ticks * poll_ms () % every < poll_ms ();
  g_autoptr (GHashTable) names = judge && E.edits->len > 0 ? running_names () : nullptr;
  for (guint i = 0; i < E.edits->len;)
    {
      Edit *e = g_ptr_array_index (E.edits, i);
      look (e, now);
      if (names != nullptr)
        judge_app (e, names, now);
      if (finished (e, now))
        {
          save (e);
          g_ptr_array_remove_index (E.edits, i);   /* frees it: its lock goes */
          continue;
        }
      i++;
    }
  if (E.edits->len > 0 || scanning)
    return true;
  hold (false);
  E.timer = 0;
  return false;
}

static void
run_timer (void)
{
  if (E.timer == 0 && E.owner != nullptr)
    E.timer = gbk_timeout_add (E.owner, (guint) poll_ms (), tick, nullptr);
}

/* ---- watching ---- */

uint64_t
gbk_edits_watch (GbkEditRecord *rec, int lock_fd, const char *asked, const char *executable)
{
  Edit *e = g_new0 (Edit, 1);
  e->id = ++E.next_id;
  e->rec = rec;
  e->lock_fd = lock_fd;
  e->asked = g_strdup (asked);
  e->exe = executable != nullptr && !generic (executable) ? g_strdup (executable) : nullptr;
  e->app = APP_STARTING;
  e->started_us = g_get_monotonic_time ();
  g_ptr_array_add (E.edits, e);
  hold (true);
  run_timer ();
  return e->id;
}

void
gbk_edits_launched (uint64_t id, GPid pid)
{
  for (guint i = 0; i < E.edits->len; i++)
    {
      Edit *e = g_ptr_array_index (E.edits, i);
      if (e->id == id)
        e->pid = pid;
    }
}

char *
gbk_edits_find (const char *host, const char *path)
{
  for (guint i = 0; E.edits != nullptr && i < E.edits->len; i++)
    {
      const Edit *e = g_ptr_array_index (E.edits, i);
      if (g_str_equal (e->rec->host, host)
          && (g_str_equal (e->asked, path) || g_str_equal (e->rec->remote, path)))
        return gbk_edit_record_copy (E.cache, e->rec);
    }
  return nullptr;
}

guint
gbk_edits_count (void)
{
  return E.edits != nullptr ? E.edits->len : 0;
}

/* ---- the last window closes ---- */

static void
on_notice_response (GtkDialog *d, int response G_GNUC_UNUSED, void *data G_GNUC_UNUSED)
{
  gtk_widget_destroy (GTK_WIDGET (d));
}

/* Always, when the last window closes while anything is watched: which files, which apps. */
static void
notice (void)
{
  GString *list = g_string_new ("");
  for (guint i = 0; i < E.edits->len; i++)
    {
      const Edit *e = g_ptr_array_index (E.edits, i);
      g_autofree char *name = g_filename_display_name (e->rec->name);
      if (e->rec->app[0] != '\0')
        g_string_append_printf (list, "%s (%s) from %s\n", name, e->rec->app, e->rec->host);
      else
        g_string_append_printf (list, "%s from %s\n", name, e->rec->host);
    }
  g_string_append (list, "\nIt stops once those apps have closed and every save is uploaded.");
  GtkWidget *d = gtk_message_dialog_new (nullptr, 0, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                         "Quoin keeps running to upload your saves");
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (d), "%s", list->str);
  g_string_free (list, true);
  gtk_window_set_title (GTK_WINDOW (d), "Quoin");
  gbk_connect (G_OBJECT (d), d, "response", G_CALLBACK (on_notice_response), nullptr);
  gtk_widget_show (d);
}

static void
on_window_added (GtkApplication *app G_GNUC_UNUSED, GtkWindow *w G_GNUC_UNUSED,
                 void *data G_GNUC_UNUSED)
{
  E.windows_gone_us = 0;
}

static void
on_window_removed (GtkApplication *app, GtkWindow *w G_GNUC_UNUSED, void *data G_GNUC_UNUSED)
{
  if (gtk_application_get_windows (app) != nullptr)
    return;
  E.windows_gone_us = g_get_monotonic_time ();
  if (E.edits->len > 0)
    notice ();
}

/* ---- copies an earlier Quoin left unsent ---- */

static void
act_on_unsent (GPtrArray *found, GbkEditsAnswer answer)
{
  for (guint i = 0; i < found->len; i++)
    {
      Unsent *u = g_ptr_array_index (found, i);
      switch (answer)
        {
        case GBK_EDITS_UPLOAD:
          {
            Edit *e = g_new0 (Edit, 1);
            e->id = ++E.next_id;
            e->rec = g_steal_pointer (&u->rec);
            e->lock_fd = u->lock_fd;
            u->lock_fd = -1;
            e->asked = g_strdup (e->rec->remote);
            e->app = APP_UNKNOWN;
            e->adopted = true;
            e->started_us = g_get_monotonic_time ();
            g_ptr_array_add (E.edits, e);
            hold (true);
            run_timer ();
            break;
          }
        case GBK_EDITS_SHOW:
          {
            GtkWindow *w = some_window ();
            g_autofree char *dir = gbk_edit_record_dir (E.cache, u->rec);
            g_autoptr (GbkLoc) loc = gbk_loc_new_local (dir);
            if (GBK_IS_WINDOW (w))
              gbk_window_new_tab (GBK_WINDOW (w), loc);
            break;
          }
        case GBK_EDITS_DISCARD:
          {
            /* This version is settled: not offered again; cleaned up with the rest. */
            GbkFileToken now;
            if (gbk_edit_record_dirty (E.cache, u->rec, &now))
              {
                u->rec->synced = now;
                g_autoptr (GError) error = nullptr;
                if (!gbk_edit_record_save (E.cache, u->rec, &error))
                  g_warning ("discarding an unsent copy: %s", error->message);
              }
            break;
          }
        default: gbk_fatal ("bad answer %d", (int) answer);
        }
    }
}

static void
on_unsent_response (GtkDialog *d, int response, GPtrArray *found)
{
  GbkEditsAnswer answer = response == RESP_UPLOAD    ? GBK_EDITS_UPLOAD
                          : response == RESP_DISCARD ? GBK_EDITS_DISCARD
                                                     : GBK_EDITS_SHOW;
  /* Closed without an answer: shown, and offered again next time. */
  act_on_unsent (found, answer);
  gtk_widget_destroy (GTK_WIDGET (d));   /* frees `found`, releasing the locks not taken */
}

static void
offer_unsent (GPtrArray *found)
{
  if (found->len == 0)
    {
      g_ptr_array_unref (found);
      return;
    }
  if (E.unsent_answer != nullptr)
    {
      act_on_unsent (found, E.unsent_answer (found->len));
      g_ptr_array_unref (found);
      return;
    }
  GString *list = g_string_new ("");
  for (guint i = 0; i < found->len; i++)
    {
      const Unsent *u = g_ptr_array_index (found, i);
      g_autofree char *name = g_filename_display_name (u->rec->name);
      g_autofree char *remote = g_filename_display_name (u->rec->remote);
      g_string_append_printf (list, "%s (%s:%s)\n", name, u->rec->host, remote);
    }
  g_string_append (list, "\nUpload sends each one back, asking first where the file there has "
                         "changed. Show opens their folders here. Discard keeps them here only.");
  GtkWidget *d = gtk_message_dialog_new (
      some_window (), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
      found->len == 1 ? "An edited file from another machine was never uploaded"
                      : "%u edited files from other machines were never uploaded",
      found->len);
  gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (d), "%s", list->str);
  g_string_free (list, true);
  gtk_dialog_add_buttons (GTK_DIALOG (d), "_Discard", RESP_DISCARD, "_Show", RESP_SHOW, "_Upload",
                          RESP_UPLOAD, nullptr);
  gtk_dialog_set_default_response (GTK_DIALOG (d), RESP_UPLOAD);
  g_object_set_data_full (G_OBJECT (d), "gbk-unsent", found, (GDestroyNotify) g_ptr_array_unref);
  gbk_connect (G_OBJECT (d), d, "response", G_CALLBACK (on_unsent_response), found);
  gtk_widget_show (d);
}

/* On a thread: records whose lock is free (no Quoin watches them). Changed copies are
 * offered (their locks kept); finished ones older than the limit are removed, and so are
 * old folders without a record (read-only copies from before records existed). */
static void *
scan_worker (void *data)
{
  g_autofree char *cache = data;
  g_autoptr (GPtrArray) found = g_ptr_array_new_with_free_func ((GDestroyNotify) unsent_free);
  g_autoptr (GDir) d = g_dir_open (cache, 0, nullptr);
  time_t limit = time (nullptr) - (time_t) GBK_EDITS_KEEP_DAYS * 24 * 3600;
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    {
      g_autofree char *path = g_build_filename (cache, name, nullptr);
      struct stat st;
      if (lstat (path, &st) != 0)
        continue;
      if (S_ISDIR (st.st_mode))
        {
          g_autofree char *ini = g_strconcat (path, ".ini", nullptr);
          if (st.st_mtime < limit && !g_file_test (ini, G_FILE_TEST_EXISTS))
            {
              int lock = gbk_edit_lock (cache, name);   /* not one being set up right now */
              if (lock >= 0)
                {
                  gbk_edit_record_remove (cache, name);
                  close (lock);
                }
            }
          continue;
        }
      if (!g_str_has_suffix (name, ".ini"))
        continue;
      g_autofree char *id = g_strndup (name, strlen (name) - 4);
      int lock = gbk_edit_lock (cache, id);
      if (lock < 0)
        continue;                        /* another Quoin is watching it */
      g_autoptr (GError) error = nullptr;
      GbkEditRecord *rec = gbk_edit_record_load (cache, id, &error);
      GbkFileToken now;
      if (rec != nullptr && gbk_edit_record_dirty (cache, rec, &now))
        {
          Unsent *u = g_new0 (Unsent, 1);
          u->rec = rec;
          u->lock_fd = lock;
          g_ptr_array_add (found, u);
          continue;
        }
      if (rec != nullptr && st.st_mtime < limit)
        gbk_edit_record_remove (cache, id);
      gbk_edit_record_free (rec);
      close (lock);
    }
  g_mutex_lock (&E.scan_lock);
  if (E.found != nullptr)
    g_ptr_array_extend_and_steal (E.found, g_steal_pointer (&found));
  else
    E.found = g_steal_pointer (&found);
  E.scan_running = false;
  g_mutex_unlock (&E.scan_lock);
  return nullptr;
}

void
gbk_edits_init (GtkApplication *app, GbkContext *ctx)
{
  E.app = app;
  E.ctx = gbk_context_ref (ctx);
  E.cache = gbk_edits_cache ();
  E.edits = g_ptr_array_new_with_free_func ((GDestroyNotify) edit_free);
  E.owner = g_object_new (G_TYPE_OBJECT, nullptr);
  gbk_connect (E.owner, app, "window-added", G_CALLBACK (on_window_added), nullptr);
  gbk_connect (E.owner, app, "window-removed", G_CALLBACK (on_window_removed), nullptr);
  gbk_edits_scan ();
}

void
gbk_edits_scan (void)
{
  if (E.edits == nullptr)
    return;
  g_mutex_lock (&E.scan_lock);
  bool already = E.scan_running;
  E.scan_running = true;
  g_mutex_unlock (&E.scan_lock);
  if (already)
    return;
  char *cache = g_strdup (E.cache);
  GThread *t = g_thread_try_new ("gbk-edits-scan", scan_worker, cache, nullptr);
  if (t != nullptr)
    g_thread_unref (t);
  else
    {
      g_free (cache);
      g_mutex_lock (&E.scan_lock);
      E.scan_running = false;
      g_mutex_unlock (&E.scan_lock);
    }
  run_timer ();
}

bool
gbk_edits_ready (void)
{
  return E.edits != nullptr;
}

void
gbk_edits_shutdown (void)
{
  if (E.edits == nullptr)
    return;
  /* The scan finishes quickly (a folder listing); it must not outlive what it fills in. */
  for (int i = 0; i < 500; i++)
    {
      g_mutex_lock (&E.scan_lock);
      bool scanning = E.scan_running;
      g_mutex_unlock (&E.scan_lock);
      if (!scanning)
        break;
      g_usleep (10000);
    }
  gbk_source_cancel (&E.timer);
  hold (false);
  g_mutex_lock (&E.scan_lock);
  g_clear_pointer (&E.found, g_ptr_array_unref);
  g_mutex_unlock (&E.scan_lock);
  /* Uploads still running finish on their own; their results go nowhere now. */
  g_clear_object (&E.owner);
  g_clear_pointer (&E.edits, g_ptr_array_unref);
  g_clear_pointer (&E.ctx, gbk_context_unref);
  g_clear_pointer (&E.cache, g_free);
  g_clear_pointer (&E.activity, g_free);
  E.app = nullptr;
}
