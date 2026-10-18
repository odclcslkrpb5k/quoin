#include "job.h"
#include "lifetime.h"
#include "remote.h"

#define PROGRESS_INTERVAL_US (INT64_C (50) * 1000)

typedef enum
{
  MSG_PROGRESS,
  MSG_QUESTION,
  MSG_FINISHED,
} MsgType;

typedef struct
{
  MsgType type;
  uint64_t done, total;
  char *current;
  GbkQuestion q;
  GError *error;
  GPtrArray *created;
  GbkFileInfo *info;
  GPtrArray *fetched;      /* FETCH: GbkFetched* */
  bool uploaded;           /* UPLOAD: one was written, its state in `token` */
  GbkFileToken token;
} Msg;

/* Shared between the main thread and the worker. */
typedef struct
{
  gatomicrefcount rc;
  GAsyncQueue *queue;      /* Msg* */
  int wake_fd;
  int cancelled;           /* atomic */

  GMutex lock;             /* guards the answer */
  GCond cond;
  bool answered;
  GbkChoice answer;
  bool answer_all;

  GbkJobKind kind;
  GError *refused;         /* can't run at all (another machine, beyond what works there) */
  GbkSource *source;       /* a copy or move from another machine: read through it */
  /* Run on another machine (the destination's): asked through `asker`; when the sources are
   * here, it reads them through `answerer`, in a session for just them. */
  GbkAsker *asker;
  GbkAnswerer *answerer;
  GPtrArray *sources;      /* paths on the machine that has them */
  char *dest;              /* a path on the machine that has it */
  GPtrArray *dirs;         /* FETCH: a folder here for each source */
  GbkFileToken base;       /* UPLOAD: the file there as it was opened */
  char *name;              /* rename: the new name; create: the name to create */
  uint32_t rwx;            /* permissions */
  int64_t last_progress;   /* worker only */
} Shared;

struct _GbkJob
{
  GObject parent_instance;
  Shared *s;
  guint watch;
  bool running;
  bool waiting;            /* a question is outstanding */
  GbkLoc *dest_loc;        /* where things are created, or nullptr */
  uint64_t done, total;
  char *current;
  GPtrArray *created;
  GbkFileInfo *info;
  GPtrArray *fetched;
  bool uploaded;
  GbkFileToken token;
};

G_DEFINE_FINAL_TYPE (GbkJob, gbk_job, G_TYPE_OBJECT)

enum
{
  SIGNAL_PROGRESS,
  SIGNAL_QUESTION,
  SIGNAL_FINISHED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];
static int live_jobs;

int
gbk_job_live (void)
{
  return g_atomic_int_get (&live_jobs);
}

static void
msg_free (Msg *m)
{
  g_free (m->current);
  g_free (m->q.path);
  g_free (m->q.other);
  g_clear_error (&m->error);
  g_clear_pointer (&m->created, g_ptr_array_unref);
  g_clear_pointer (&m->info, gbk_file_info_free);
  g_clear_pointer (&m->fetched, g_ptr_array_unref);
  g_free (m);
}

void
gbk_fetched_free (GbkFetched *f)
{
  if (f == nullptr)
    return;
  g_free (f->name);
  g_free (f->remote);
  g_free (f);
}

static void
shared_unref (Shared *s)
{
  if (!g_atomic_ref_count_dec (&s->rc))
    return;
  g_async_queue_unref (s->queue);
  close (s->wake_fd);
  g_mutex_clear (&s->lock);
  g_cond_clear (&s->cond);
  g_ptr_array_unref (s->sources);
  g_free (s->dest);
  g_clear_pointer (&s->dirs, g_ptr_array_unref);
  g_clear_error (&s->refused);
  gbk_source_free (s->source);
  gbk_asker_unref (s->asker);
  gbk_answerer_unref (s->answerer);
  g_free (s->name);
  g_free (s);
}

static void
post_msg (Shared *s, Msg *m)
{
  g_async_queue_push (s->queue, m);
  uint64_t one = 1;
  while (write (s->wake_fd, &one, sizeof one) < 0 && errno == EINTR)
    ;
}

static bool
is_cancelled (Shared *s)
{
  return g_atomic_int_get (&s->cancelled) != 0;
}

/* ---- worker side ---- */

/* Ask the main thread and block until it answers (or the job is cancelled). */
static GbkChoice
ask (Shared *s, GbkQuestionKind kind, const char *path, const char *other, bool other_is_dir,
     bool *apply_all)
{
  g_mutex_lock (&s->lock);
  s->answered = false;
  g_mutex_unlock (&s->lock);

  Msg *m = g_new0 (Msg, 1);
  m->type = MSG_QUESTION;
  m->q.kind = kind;
  m->q.path = g_strdup (path);
  m->q.other = g_strdup (other);
  m->q.other_is_dir = other_is_dir;
  post_msg (s, m);

  g_mutex_lock (&s->lock);
  while (!s->answered && !is_cancelled (s))
    g_cond_wait (&s->cond, &s->lock);
  GbkChoice choice = s->answered ? s->answer : GBK_CHOICE_CANCEL;
  if (apply_all != nullptr)
    *apply_all = s->answered && s->answer_all;
  g_mutex_unlock (&s->lock);
  return is_cancelled (s) ? GBK_CHOICE_CANCEL : choice;
}

static GbkChoice
hook_conflict (const char *src, const char *dst, bool dst_is_dir, bool *apply_all, void *data)
{
  return ask (data, GBK_QUESTION_CONFLICT, src, dst, dst_is_dir, apply_all);
}

static GbkChoice
hook_error (const char *path, const GError *error, void *data)
{
  return ask (data, GBK_QUESTION_ERROR, path, error->message, false, nullptr);
}

static GbkChoice
hook_upload_conflict (const char *target, bool linked, void *data)
{
  return ask (data, GBK_QUESTION_UPLOAD, target, nullptr, linked, nullptr);
}

static bool
hook_untrashable (const char *path, void *data)
{
  return ask (data, GBK_QUESTION_UNTRASHABLE, path, nullptr, false, nullptr) == GBK_CHOICE_REPLACE;
}

static void
hook_progress (uint64_t done, uint64_t total, const char *current, void *data)
{
  Shared *s = data;
  int64_t now = g_get_monotonic_time ();
  if (now - s->last_progress < PROGRESS_INTERVAL_US)
    return;
  s->last_progress = now;
  Msg *m = g_new0 (Msg, 1);
  m->type = MSG_PROGRESS;
  m->done = done;
  m->total = total;
  m->current = g_filename_display_basename (current);
  post_msg (s, m);
}

static bool
hook_cancelled (void *data)
{
  return is_cancelled (data);
}

/* Execute bit on several files: a failure asks Skip / Cancel when others remain. */
static void
run_executable (Shared *s, bool on, GError **error)
{
  for (guint i = 0; i < s->sources->len && !is_cancelled (s); i++)
    {
      const char *path = g_ptr_array_index (s->sources, i);
      g_autoptr (GError) e = nullptr;
      if (gbk_ops_set_executable (path, on, &e))
        continue;
      if (s->sources->len == 1)
        {
          g_propagate_error (error, g_steal_pointer (&e));
          return;
        }
      if (hook_error (path, e, s) == GBK_CHOICE_CANCEL)
        break;
    }
  if (is_cancelled (s))
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
}

/* COPY or MOVE: here, from another machine, or on another machine. */
[[nodiscard]] static bool
run_transfer (Shared *s, GbkOpKind kind, const GbkOpsHooks *hooks, GPtrArray *created,
              GError **error)
{
  if (s->asker == nullptr)
    return s->source != nullptr
               ? gbk_ops_run_from (s->source, kind, s->sources, s->dest, hooks, created, error)
               : gbk_ops_run (kind, s->sources, s->dest, hooks, created, error);
  /* Sources here: the other machine may read them (and, moving, remove them) and nothing
   * else, for as long as this job runs. */
  bool here = s->answerer != nullptr;
  uint64_t session = here ? gbk_answerer_open (s->answerer, s->sources, kind == GBK_OP_MOVE) : 0;
  GbkJobRequest req = { .kind = kind, .sources_here = here, .session = session, .dest = s->dest };
  bool ok = gbk_asker_run_job (s->asker, &req, s->sources, hooks, created, nullptr, error);
  if (here)
    gbk_answerer_close (s->answerer, session);
  return ok;
}

/* One file from the other machine into `dir` here as `name`: the file there as read in
 * *token. The copy is made only if it all came unchanged. */
[[nodiscard]] static bool
fetch_one (Shared *s, const GbkOpsHooks *hooks, const char *remote, const char *dir,
           const char *name, uint64_t *done, uint64_t total, GbkFileToken *token, GError **error)
{
  GbkSourceFile *f = s->source->ops->open (s->source, remote, error);
  if (f == nullptr)
    return false;
  g_autofree char *local = g_build_filename (dir, name, nullptr);
  int out = open (local, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (out < 0)
    {
      g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (errno), "%s",
                   g_strerror (errno));
      s->source->ops->close (f);
      return false;
    }
  g_autofree char *buf = g_malloc (GBK_PROTO_DATA_MAX);
  bool ok = true;
  for (;;)
    {
      ssize_t n = s->source->ops->read (f, buf, GBK_PROTO_DATA_MAX, error);
      if (n <= 0)
        {
          ok = n == 0;
          break;
        }
      for (ssize_t off = 0; ok && off < n;)
        {
          ssize_t w = write (out, buf + off, (size_t) (n - off));
          if (w < 0 && errno == EINTR)
            continue;
          if (w < 0)
            {
              g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (errno), "%s",
                           g_strerror (errno));
              ok = false;
            }
          else
            off += w;
        }
      if (!ok)
        break;
      *done += (uint64_t) n;
      hooks->progress (*done, total, remote, hooks->data);
    }
  if (ok && !s->source->ops->state (f, token))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "It changed while it was copied");
      ok = false;
    }
  s->source->ops->close (f);
  if (ok && fsync (out) != 0)
    {
      g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (errno), "%s",
                   g_strerror (errno));
      ok = false;
    }
  close (out);
  if (!ok)
    unlink (local);
  return ok;
}

static void
free_error (void *e)
{
  if (e != nullptr)
    g_error_free (e);
}

/* Every source, followed through links there, into its own folder here. A file that can't
 * come is asked about (Skip / Cancel). */
static void
run_fetch (Shared *s, const GbkOpsHooks *hooks, GPtrArray *fetched, GError **error)
{
  s->source->cancelled = hooks->cancelled;
  s->source->cancel_data = hooks->data;
  g_autoptr (GPtrArray) real = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) why = g_ptr_array_new_with_free_func (free_error);   /* or nullptr */
  uint64_t total = 0, done = 0;
  for (guint i = 0; i < s->sources->len; i++)
    {
      const char *path = g_ptr_array_index (s->sources, i);
      GError *e = nullptr;
      char *r = s->source->ops->resolve (s->source, path, &e);
      g_autoptr (GbkStatEntry) st
          = r != nullptr ? s->source->ops->lstat (s->source, r, &e) : nullptr;
      if (st != nullptr && !S_ISREG (st->mode))
        e = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE, "Not a file");
      else if (st != nullptr)
        total += st->size;
      g_ptr_array_add (real, e == nullptr ? r : nullptr);
      if (e != nullptr)
        g_free (r);
      g_ptr_array_add (why, e);
    }
  for (guint i = 0; i < s->sources->len && !is_cancelled (s); i++)
    {
      const char *path = g_ptr_array_index (s->sources, i);
      const char *remote = g_ptr_array_index (real, i);
      g_autoptr (GError) e
          = remote == nullptr ? g_error_copy (g_ptr_array_index (why, i)) : nullptr;
      g_autofree char *name = g_path_get_basename (path);
      GbkFileToken token = { 0 };
      if (e == nullptr
          && fetch_one (s, hooks, remote, g_ptr_array_index (s->dirs, i), name, &done, total,
                        &token, &e))
        {
          GbkFetched *f = g_new0 (GbkFetched, 1);
          f->index = i;
          f->name = g_steal_pointer (&name);
          f->remote = g_strdup (remote);
          f->token = token;
          g_ptr_array_add (fetched, f);
          continue;
        }
      if (is_cancelled (s) || g_error_matches (e, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        break;
      g_autofree char *shown = g_filename_display_basename (path);
      g_autoptr (GError) told
          = g_error_new (e->domain, e->code, "Can't open %s: %s", shown, e->message);
      if (hook_error (path, told, s) == GBK_CHOICE_CANCEL)
        break;
    }
  if (is_cancelled (s))
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
}

/* The edited copy over the file there, if that is still as it was. */
static bool
run_upload (Shared *s, const GbkOpsHooks *hooks, Msg *done)
{
  uint64_t session = gbk_answerer_open (s->answerer, s->sources, false);
  GbkJobRequest req = { .kind = GBK_OP_UPLOAD,
                        .sources_here = true,
                        .session = session,
                        .dest = s->dest,
                        .base = s->base };
  done->token = (GbkFileToken) { 0 };
  bool ok = gbk_asker_run_job (s->asker, &req, s->sources, hooks, done->created, &done->token,
                               &done->error);
  gbk_answerer_close (s->answerer, session);
  done->uploaded = ok && done->created->len > 0;
  return ok;
}

/* The result travels as `error` alone, so it must say what the return value said: false
 * exactly when the error is set (GError's convention). Read the error only after the call
 * has returned: as two arguments of one call, their order is unspecified. */
static void
agree (bool ok, const GError *error)
{
  if (ok == (error != nullptr))
    gbk_fatal ("job: returned %s with%s an error", ok ? "true" : "false", ok ? "" : "out");
}

static void *
worker (void *data)
{
  Shared *s = data;
  GbkOpsHooks hooks = { hook_conflict,  hook_error, hook_untrashable,    hook_progress,
                        hook_cancelled, s,          hook_upload_conflict };
  Msg *done = g_new0 (Msg, 1);
  done->type = MSG_FINISHED;
  done->created = g_ptr_array_new_with_free_func (g_free);
  const char *first = g_ptr_array_index (s->sources, 0);
  bool ok;
  switch (s->kind)
    {
    case GBK_JOB_COPY:
    case GBK_JOB_MOVE:
      ok = run_transfer (s, s->kind == GBK_JOB_COPY ? GBK_OP_COPY : GBK_OP_MOVE, &hooks,
                         done->created, &done->error);
      agree (ok, done->error);
      break;
    case GBK_JOB_TRASH:
      ok = gbk_ops_run (GBK_OP_TRASH, s->sources, s->dest, &hooks, done->created, &done->error);
      agree (ok, done->error);
      break;
    case GBK_JOB_DELETE:
      ok = gbk_ops_run (GBK_OP_DELETE, s->sources, s->dest, &hooks, done->created, &done->error);
      agree (ok, done->error);
      break;
    case GBK_JOB_RENAME:
      if (gbk_ops_rename (first, s->name, &done->error))
        g_ptr_array_add (done->created, g_strdup (s->name));
      break;
    case GBK_JOB_CREATE_FILE:
    case GBK_JOB_CREATE_FOLDER:
      if (gbk_ops_create (s->dest, s->name, s->kind == GBK_JOB_CREATE_FOLDER, &done->error))
        g_ptr_array_add (done->created, g_strdup (s->name));
      break;
    case GBK_JOB_SET_EXECUTABLE: run_executable (s, true, &done->error); break;
    case GBK_JOB_CLEAR_EXECUTABLE: run_executable (s, false, &done->error); break;
    case GBK_JOB_PERMISSIONS:
      ok = gbk_ops_set_permissions (first, s->rwx, &done->error);
      agree (ok, done->error);
      break;
    case GBK_JOB_INFO: done->info = gbk_ops_info (first, &done->error); break;
    case GBK_JOB_FETCH:
      done->fetched = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_fetched_free);
      run_fetch (s, &hooks, done->fetched, &done->error);
      break;
    case GBK_JOB_UPLOAD:
      ok = run_upload (s, &hooks, done);
      agree (ok, done->error);
      break;
    default: gbk_fatal ("bad job kind %d", (int) s->kind);
    }
  post_msg (s, done);
  shared_unref (s);
  return nullptr;
}

/* ---- main thread side ---- */

static void
finish (GbkJob *job, Msg *m)
{
  job->running = false;
  job->waiting = false;
  g_clear_pointer (&job->created, g_ptr_array_unref);
  job->created = g_steal_pointer (&m->created);
  g_clear_pointer (&job->info, gbk_file_info_free);
  job->info = g_steal_pointer (&m->info);
  g_clear_pointer (&job->fetched, g_ptr_array_unref);
  job->fetched = g_steal_pointer (&m->fetched);
  job->uploaded = m->uploaded;
  job->token = m->token;
  gbk_source_cancel (&job->watch);
  g_signal_emit (job, signals[SIGNAL_FINISHED], 0, m->error);
  g_atomic_int_dec_and_test (&live_jobs);
  g_object_unref (job);        /* the running job's self-reference */
}

static bool
on_wake (int fd G_GNUC_UNUSED, GIOCondition cond G_GNUC_UNUSED, void *data)
{
  GbkJob *job = data;
  uint64_t v;
  while (read (job->s->wake_fd, &v, sizeof v) < 0 && errno == EINTR)
    ;
  /* The worker blocks on each question, so at most one is ever queued. */
  Msg *m;
  while (job->running && (m = g_async_queue_try_pop (job->s->queue)) != nullptr)
    {
      switch (m->type)
        {
        case MSG_PROGRESS:
          job->done = m->done;
          job->total = m->total;
          g_free (job->current);
          job->current = g_steal_pointer (&m->current);
          g_signal_emit (job, signals[SIGNAL_PROGRESS], 0);
          break;
        case MSG_QUESTION:
          job->waiting = true;
          g_signal_emit (job, signals[SIGNAL_QUESTION], 0, &m->q);
          break;
        case MSG_FINISHED:
          {
            /* finish() may drop the last reference: keep `job` valid until we return. */
            g_autoptr (GbkJob) hold = g_object_ref (job);
            finish (job, m);
            msg_free (m);
            return false;
          }
        default: gbk_fatal ("bad job message %d", (int) m->type);
        }
      msg_free (m);
    }
  return job->running;
}

void
gbk_job_answer (GbkJob *job, GbkChoice choice, bool apply_all)
{
  if (!job->waiting)
    return; /* stale answer (e.g. dialog closed after cancel) */
  job->waiting = false;
  g_mutex_lock (&job->s->lock);
  job->s->answer = choice;
  job->s->answer_all = apply_all;
  job->s->answered = true;
  g_cond_broadcast (&job->s->cond);
  g_mutex_unlock (&job->s->lock);
}

void
gbk_job_cancel (GbkJob *job)
{
  g_atomic_int_set (&job->s->cancelled, 1);
  g_mutex_lock (&job->s->lock);
  g_cond_broadcast (&job->s->cond); /* wake a worker waiting for an answer */
  g_mutex_unlock (&job->s->lock);
}

void
gbk_job_start (GbkJob *job)
{
  g_assert (!job->running && job->watch == 0);
  job->running = true;
  g_object_ref (job); /* released in finish() */
  g_atomic_int_inc (&live_jobs);
  job->watch = gbk_fd_watch (G_OBJECT (job), job->s->wake_fd, G_IO_IN, on_wake, job);
  g_atomic_ref_count_inc (&job->s->rc);
  g_autoptr (GError) error = nullptr;
  GThread *t
      = job->s->refused == nullptr ? g_thread_try_new ("gbk-job", worker, job->s, &error) : nullptr;
  if (job->s->refused != nullptr)
    error = g_error_copy (job->s->refused);
  if (t == nullptr)
    {
      shared_unref (job->s);
      Msg *m = g_new0 (Msg, 1);
      m->type = MSG_FINISHED;
      m->error = g_steal_pointer (&error);
      m->created = g_ptr_array_new_with_free_func (g_free);
      post_msg (job->s, m); /* delivered via the normal path */
      return;
    }
  g_thread_unref (t);
}

static void
gbk_job_finalize (GObject *o)
{
  GbkJob *job = GBK_JOB (o);
  g_assert (!job->running);
  shared_unref (job->s);
  g_clear_pointer (&job->dest_loc, gbk_loc_unref);
  g_free (job->current);
  g_clear_pointer (&job->created, g_ptr_array_unref);
  g_clear_pointer (&job->info, gbk_file_info_free);
  g_clear_pointer (&job->fetched, g_ptr_array_unref);
  G_OBJECT_CLASS (gbk_job_parent_class)->finalize (o);
}

static void
gbk_job_class_init (GbkJobClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gbk_job_finalize;
  signals[SIGNAL_PROGRESS] = g_signal_new ("progress", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                           0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  signals[SIGNAL_QUESTION]
      = g_signal_new ("question", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr,
                      nullptr, G_TYPE_NONE, 1, G_TYPE_POINTER);
  signals[SIGNAL_FINISHED]
      = g_signal_new ("finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr,
                      nullptr, G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void
gbk_job_init (GbkJob *job G_GNUC_UNUSED)
{
}

/* The one machine all of `locs` are on: "" for this one, nullptr if they are on several. */
static const char *
one_machine (const GPtrArray *locs)
{
  const char *host = locs->len > 0 ? gbk_loc_host (g_ptr_array_index (locs, 0)) : nullptr;
  for (guint i = 1; i < locs->len; i++)
    if (g_strcmp0 (gbk_loc_host (g_ptr_array_index (locs, i)), host) != 0)
      return nullptr;
  return host != nullptr ? host : "";
}

static GbkConn *
conn_for (Shared *s, const char *host)
{
  GbkRemote *r = gbk_remote_default ();
  GbkConn *c = r != nullptr ? gbk_remote_conn (r, host, &s->refused) : nullptr;
  if (c == nullptr && s->refused == nullptr)
    s->refused = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                      "Other machines can't be reached from here");
  return c;
}

/* Paths of `locs`, on whichever machine they are. */
static GPtrArray *
paths_of (const GPtrArray *locs)
{
  GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < locs->len; i++)
    g_ptr_array_add (paths, g_strdup (gbk_loc_path (g_ptr_array_index (locs, i))));
  return paths;
}

/* COPY or MOVE with another machine on either side: decide where it runs (s->source,
 * s->asker, s->answerer), or why it can't (s->refused). */
static void
plan_transfer (Shared *s, const GPtrArray *locs, const GbkLoc *dest)
{
  if (dest == nullptr)
    gbk_fatal ("a copy or move without a destination");
  const char *from = one_machine (locs);
  const char *to = gbk_loc_is_local (dest) ? "" : gbk_loc_host (dest);
  if (from == nullptr)
    {
      s->refused = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                        "Copy or move from one machine at a time");
      return;
    }
  if (from[0] != '\0' && to[0] != '\0' && !g_str_equal (from, to))
    {
      s->refused = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                        "Copying between two other machines isn't possible yet");
      return;
    }
  g_autoptr (GbkConn) c = conn_for (s, to[0] != '\0' ? to : from);
  if (c == nullptr)
    return;
  s->sources = paths_of (locs);
  s->dest = g_strdup (gbk_loc_path (dest));
  if (to[0] == '\0')
    s->source = gbk_conn_source (c, s->kind == GBK_JOB_MOVE); /* here, reading there */
  else
    {
      s->asker = gbk_conn_asker (c); /* there */
      if (from[0] == '\0')
        s->answerer = gbk_conn_answerer (c); /* reading here */
    }
}

static GbkJob *
job_new (GbkJobKind kind, const GPtrArray *sources, GbkLoc *dest)
{
  GbkJob *job = g_object_new (GBK_TYPE_JOB, nullptr);
  Shared *s = g_new0 (Shared, 1);
  g_atomic_ref_count_init (&s->rc);
  s->queue = g_async_queue_new_full ((GDestroyNotify) msg_free);
  s->wake_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (s->wake_fd < 0)
    gbk_fatal ("eventfd: %s", g_strerror (errno));
  g_mutex_init (&s->lock);
  g_cond_init (&s->cond);
  s->kind = kind;
  const char *dest_path = nullptr;
  bool dest_here = dest == nullptr || gbk_loc_as_local (dest, &dest_path);
  s->sources = gbk_loc_array_as_local (sources);
  if (s->sources != nullptr && dest_here)
    s->dest = g_strdup (dest_path); /* all on this machine */
  else if (kind == GBK_JOB_COPY || kind == GBK_JOB_MOVE)
    {
      g_clear_pointer (&s->sources, g_ptr_array_unref);
      plan_transfer (s, sources, dest);
    }
  else
    s->refused = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                      "Not possible for files on another machine yet");
  if (s->sources == nullptr)
    s->sources = g_ptr_array_new_with_free_func (g_free);
  job->dest_loc = dest != nullptr ? gbk_loc_ref (dest) : nullptr;
  job->s = s;
  job->total = 0;
  return job;
}

GbkJob *
gbk_job_new (GbkOpKind kind, const GPtrArray *sources, GbkLoc *dest_dir)
{
  GbkJobKind k;
  switch (kind)
    {
    case GBK_OP_COPY: k = GBK_JOB_COPY; break;
    case GBK_OP_MOVE: k = GBK_JOB_MOVE; break;
    case GBK_OP_TRASH:
      k = GBK_JOB_TRASH;
      dest_dir = nullptr;
      break;
    case GBK_OP_DELETE:
      k = GBK_JOB_DELETE;
      dest_dir = nullptr;
      break;
    case GBK_OP_UPLOAD: /* gbk_job_new_upload */
    default: gbk_fatal ("bad operation %d", (int) kind);
    }
  return job_new (k, sources, dest_dir);
}

static GbkJob *
job_new_one (GbkJobKind kind, GbkLoc *loc, GbkLoc *dest)
{
  g_autoptr (GPtrArray) one = gbk_loc_array_new ();
  g_ptr_array_add (one, gbk_loc_ref (loc));
  return job_new (kind, one, dest);
}

GbkJob *
gbk_job_new_rename (GbkLoc *loc, const char *new_name)
{
  g_autoptr (GbkLoc) dir = gbk_loc_parent (loc);
  GbkJob *job = job_new_one (GBK_JOB_RENAME, loc, dir);
  job->s->name = g_strdup (new_name);
  return job;
}

GbkJob *
gbk_job_new_create (GbkLoc *dir, const char *name, bool folder)
{
  /* The worker uses dest + name; the folder is also the one "source", for the count. */
  GbkJob *job = job_new_one (folder ? GBK_JOB_CREATE_FOLDER : GBK_JOB_CREATE_FILE, dir, dir);
  job->s->name = g_strdup (name);
  return job;
}

GbkJob *
gbk_job_new_executable (const GPtrArray *locs, bool executable)
{
  if (locs->len == 0)
    gbk_fatal ("executable job without files");
  return job_new (executable ? GBK_JOB_SET_EXECUTABLE : GBK_JOB_CLEAR_EXECUTABLE, locs, nullptr);
}

GbkJob *
gbk_job_new_permissions (GbkLoc *loc, uint32_t rwx)
{
  GbkJob *job = job_new_one (GBK_JOB_PERMISSIONS, loc, nullptr);
  job->s->rwx = rwx & 0777;
  return job;
}

GbkJob *
gbk_job_new_info (GbkLoc *loc)
{
  return job_new_one (GBK_JOB_INFO, loc, nullptr);
}

/* A job on one other machine's files, through its connection: nullptr with s->refused. */
static GbkConn *
remote_conn (Shared *s, const GbkLoc *loc)
{
  if (gbk_loc_is_local (loc))
    {
      s->refused
          = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "That is on this machine");
      return nullptr;
    }
  return conn_for (s, gbk_loc_host (loc));
}

GbkJob *
gbk_job_new_fetch (const GPtrArray *locs, const GPtrArray *dirs)
{
  g_autoptr (GPtrArray) none = gbk_loc_array_new ();
  GbkJob *job = job_new (GBK_JOB_FETCH, none, nullptr);
  Shared *s = job->s;
  const char *host = one_machine (locs);
  if (locs->len == 0 || locs->len != dirs->len || host == nullptr || host[0] == '\0')
    {
      s->refused = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                        "Open files from one other machine at a time");
      return job;
    }
  g_autoptr (GbkConn) c = remote_conn (s, g_ptr_array_index (locs, 0));
  if (c == nullptr)
    return job;
  s->source = gbk_conn_source (c, false);
  g_ptr_array_unref (s->sources);
  s->sources = paths_of (locs);
  s->dirs = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < dirs->len; i++)
    g_ptr_array_add (s->dirs, g_strdup (g_ptr_array_index (dirs, i)));
  return job;
}

GbkJob *
gbk_job_new_upload (const char *local, const GbkLoc *target, const GbkFileToken *base)
{
  g_autoptr (GPtrArray) none = gbk_loc_array_new ();
  g_autoptr (GbkLoc) dir = gbk_loc_parent (target);
  GbkJob *job = job_new (GBK_JOB_UPLOAD, none, nullptr);
  job->dest_loc = gbk_loc_ref (dir);
  Shared *s = job->s;
  g_autoptr (GbkConn) c = remote_conn (s, target);
  if (c == nullptr)
    return job;
  s->asker = gbk_conn_asker (c);
  s->answerer = gbk_conn_answerer (c);
  g_ptr_array_add (s->sources, g_strdup (local));
  s->dest = g_strdup (gbk_loc_path (target));
  s->base = *base;
  return job;
}

const GPtrArray *
gbk_job_fetched (const GbkJob *job)
{
  return job->fetched;
}

bool
gbk_job_uploaded (const GbkJob *job, GbkFileToken *token)
{
  if (job->uploaded)
    *token = job->token;
  return job->uploaded;
}

bool
gbk_job_running (const GbkJob *job)
{
  return job->running;
}
GbkJobKind
gbk_job_kind (GbkJob *job)
{
  return job->s->kind;
}
const GbkFileInfo *
gbk_job_info (const GbkJob *job)
{
  return job->info;
}
const GbkLoc *
gbk_job_dest (const GbkJob *job)
{
  return job->dest_loc;
}
size_t
gbk_job_n_sources (GbkJob *job)
{
  return job->s->sources->len;
}
const char *
gbk_job_current (GbkJob *job)
{
  return job->current != nullptr ? job->current : "";
}
GPtrArray *
gbk_job_created (const GbkJob *job)
{
  return job->created;
}

double
gbk_job_fraction (const GbkJob *job)
{
  if (job->total == 0)
    return -1;
  return CLAMP ((double) job->done / (double) job->total, 0.0, 1.0);
}
