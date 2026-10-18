#include "job.h"
#include "lifetime.h"

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
  bool remote;             /* a location isn't on this machine: the job fails at start */
  GPtrArray *sources;      /* local paths */
  char *dest;
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
  g_free (m);
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
  g_free (s->name);
  g_free (s);
}

static void
send (Shared *s, Msg *m)
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
  send (s, m);

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
  send (s, m);
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
  GbkOpsHooks hooks
      = { hook_conflict, hook_error, hook_untrashable, hook_progress, hook_cancelled, s };
  Msg *done = g_new0 (Msg, 1);
  done->type = MSG_FINISHED;
  done->created = g_ptr_array_new_with_free_func (g_free);
  const char *first = g_ptr_array_index (s->sources, 0);
  bool ok;
  switch (s->kind)
    {
    case GBK_JOB_COPY:
      ok = gbk_ops_run (GBK_OP_COPY, s->sources, s->dest, &hooks, done->created, &done->error);
      agree (ok, done->error);
      break;
    case GBK_JOB_MOVE:
      ok = gbk_ops_run (GBK_OP_MOVE, s->sources, s->dest, &hooks, done->created, &done->error);
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
    default: gbk_fatal ("bad job kind %d", (int) s->kind);
    }
  send (s, done);
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
  GThread *t = nullptr;
  if (job->s->remote)
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                         "Not possible for files on another machine yet");
  else
    t = g_thread_try_new ("gbk-job", worker, job->s, &error);
  if (t == nullptr)
    {
      shared_unref (job->s);
      Msg *m = g_new0 (Msg, 1);
      m->type = MSG_FINISHED;
      m->error = g_steal_pointer (&error);
      m->created = g_ptr_array_new_with_free_func (g_free);
      send (job->s, m); /* delivered via the normal path */
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

/* `sources` are GbkLoc*; `dest` may be nullptr. */
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
  s->sources = gbk_loc_array_as_local (sources);
  const char *dest_path = nullptr;
  s->remote = s->sources == nullptr || (dest != nullptr && !gbk_loc_as_local (dest, &dest_path));
  if (s->sources == nullptr)
    s->sources = g_ptr_array_new_with_free_func (g_free);
  s->dest = g_strdup (dest_path);
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
