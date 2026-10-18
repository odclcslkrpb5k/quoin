#include "peer.h"

#define TASKS_MAX 16                 /* requests answered at once */
#define SESSIONS_MAX 1024
#define JOBS_MAX 8
#define JOB_SOURCES_MAX 1000000      /* sources of jobs not started yet, all together */
#define LIST_BATCH 1000              /* entries per LISTING handed over: bounded memory */
#define READ_WINDOW (1u << 20)       /* bytes a READ may send ahead of what's been copied */
#define WAIT_STEP_US (G_USEC_PER_SEC / 10)
#define PROGRESS_EVERY_US (INT64_C (100) * 1000)
#define STRICT (RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS)

uint64_t
gbk_peer_channel (bool server)
{
  static uint64_t next = 1;
  uint64_t ch;
  static GMutex lock;
  g_mutex_lock (&lock);
  ch = next++;
  g_mutex_unlock (&lock);
  return server ? ch | GBK_PROTO_SERVER_CHANNEL : ch;
}

static GError *
not_understood (void)
{
  return g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                              "The other machine answered with something Quoin doesn't understand");
}

/* ---- the outbox ---- */

struct GbkOutbox
{
  gatomicrefcount rc;
  GMutex lock;
  GByteArray *bytes;
  int wake;               /* eventfd */
};

GbkOutbox *
gbk_outbox_new (void)
{
  GbkOutbox *o = g_new0 (GbkOutbox, 1);
  g_atomic_ref_count_init (&o->rc);
  g_mutex_init (&o->lock);
  o->bytes = g_byte_array_new ();
  o->wake = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (o->wake < 0)
    gbk_fatal ("eventfd: %s", g_strerror (errno));
  return o;
}

GbkOutbox *
gbk_outbox_ref (GbkOutbox *o)
{
  g_atomic_ref_count_inc (&o->rc);
  return o;
}

void
gbk_outbox_unref (GbkOutbox *o)
{
  if (o == nullptr || !g_atomic_ref_count_dec (&o->rc))
    return;
  g_mutex_clear (&o->lock);
  g_byte_array_unref (o->bytes);
  close (o->wake);
  g_free (o);
}

static void
wake (int fd)
{
  uint64_t one = 1;
  while (write (fd, &one, sizeof one) < 0 && errno == EINTR)
    ;
}

void
gbk_outbox_put (GbkOutbox *o, const GByteArray *frames)
{
  g_mutex_lock (&o->lock);
  g_byte_array_append (o->bytes, frames->data, frames->len);
  g_mutex_unlock (&o->lock);
  wake (o->wake);
}

GByteArray *
gbk_outbox_begin (GbkOutbox *o)
{
  g_mutex_lock (&o->lock);
  return o->bytes;
}

void
gbk_outbox_end (GbkOutbox *o)
{
  bool any = o->bytes->len > 0;
  g_mutex_unlock (&o->lock);
  if (any)
    wake (o->wake);
}

size_t
gbk_outbox_len (GbkOutbox *o)
{
  g_mutex_lock (&o->lock);
  size_t n = o->bytes->len;
  g_mutex_unlock (&o->lock);
  return n;
}

int
gbk_outbox_wake_fd (const GbkOutbox *o)
{
  return o->wake;
}

void
gbk_outbox_woken (const GbkOutbox *o)
{
  uint64_t v;
  while (read (o->wake, &v, sizeof v) < 0 && errno == EINTR)
    ;
}

/* ---- asking ---- */

struct GbkAsker
{
  gatomicrefcount rc;
  bool server;
  GbkOutbox *out;
  GMutex lock;            /* guards everything below */
  GHashTable *calls;      /* &Call.channel -> Call* */
  bool dead;
  GError *dead_error;
  int users;              /* sources and jobs in use */
  int64_t unused_since;
};

typedef struct
{
  GbkMsgType type;
  GBytes *payload;
  GError *error;          /* the connection died */
} Reply;

typedef struct
{
  GbkAsker *a;            /* referenced */
  uint64_t channel;
  GAsyncQueue *q;         /* Reply* */
} Call;

static void
reply_free (Reply *r)
{
  g_clear_pointer (&r->payload, g_bytes_unref);
  g_clear_error (&r->error);
  g_free (r);
}

GbkAsker *
gbk_asker_new (bool server, GbkOutbox *out)
{
  GbkAsker *a = g_new0 (GbkAsker, 1);
  g_atomic_ref_count_init (&a->rc);
  a->server = server;
  a->out = gbk_outbox_ref (out);
  g_mutex_init (&a->lock);
  a->calls = g_hash_table_new (g_int64_hash, g_int64_equal);
  a->unused_since = g_get_monotonic_time ();
  return a;
}

GbkAsker *
gbk_asker_ref (GbkAsker *a)
{
  g_atomic_ref_count_inc (&a->rc);
  return a;
}

void
gbk_asker_unref (GbkAsker *a)
{
  if (a == nullptr || !g_atomic_ref_count_dec (&a->rc))
    return;
  gbk_outbox_unref (a->out);
  g_mutex_clear (&a->lock);
  g_hash_table_unref (a->calls);   /* empty: each call holds a reference */
  g_clear_error (&a->dead_error);
  g_free (a);
}

bool
gbk_asker_answer_type (GbkMsgType type)
{
  switch (type)
    {
    case GBK_MSG_FAILED:
    case GBK_MSG_LISTING:
    case GBK_MSG_STATED:
    case GBK_MSG_DATA:
    case GBK_MSG_END:
    case GBK_MSG_REMOVED:
    case GBK_MSG_QUESTION:
    case GBK_MSG_PROGRESS:
    case GBK_MSG_CREATED:
    case GBK_MSG_DONE: return true;
    case GBK_MSG_HELLO:
    case GBK_MSG_SCAN:
    case GBK_MSG_INFO:
    case GBK_MSG_PONG:
    case GBK_MSG_WATCH:
    case GBK_MSG_UNWATCH:
    case GBK_MSG_GET_INFO:
    case GBK_MSG_PING:
    case GBK_MSG_LIST:
    case GBK_MSG_LSTAT:
    case GBK_MSG_READ:
    case GBK_MSG_CREDIT:
    case GBK_MSG_CANCEL:
    case GBK_MSG_REMOVE:
    case GBK_MSG_SESSION:
    case GBK_MSG_FORGET:
    case GBK_MSG_SOURCES:
    case GBK_MSG_JOB:
    case GBK_MSG_ANSWER:
    default: return false;
    }
}

bool
gbk_asker_route (GbkAsker *a, const GbkFrame *f, bool *valid)
{
  g_mutex_lock (&a->lock);
  Call *call = g_hash_table_lookup (a->calls, &f->channel);
  if (call != nullptr)
    {
      Reply *r = g_new0 (Reply, 1);
      r->type = f->type;
      r->payload = g_bytes_new (f->payload, f->len);
      g_async_queue_push (call->q, r);
    }
  g_mutex_unlock (&a->lock);
  *valid = gbk_asker_answer_type (f->type);
  return call != nullptr;
}

void
gbk_asker_dead (GbkAsker *a, const GError *error)
{
  g_mutex_lock (&a->lock);
  if (!a->dead)
    {
      a->dead = true;
      a->dead_error = g_error_copy (error);
      GHashTableIter it;
      void *call;
      g_hash_table_iter_init (&it, a->calls);
      while (g_hash_table_iter_next (&it, nullptr, &call))
        {
          Reply *r = g_new0 (Reply, 1);
          r->error = g_error_copy (error);
          g_async_queue_push (((Call *) call)->q, r);
        }
    }
  g_mutex_unlock (&a->lock);
}

static void
use_begin (GbkAsker *a)
{
  g_mutex_lock (&a->lock);
  a->users++;
  g_mutex_unlock (&a->lock);
}

static void
use_end (GbkAsker *a)
{
  g_mutex_lock (&a->lock);
  if (--a->users == 0)
    a->unused_since = g_get_monotonic_time ();
  g_mutex_unlock (&a->lock);
}

int64_t
gbk_asker_unused_since (GbkAsker *a)
{
  g_mutex_lock (&a->lock);
  int64_t since = a->users > 0 ? 0 : a->unused_since;
  g_mutex_unlock (&a->lock);
  return since;
}

[[nodiscard]] static Call *
call_start (GbkAsker *a, GError **error)
{
  g_mutex_lock (&a->lock);
  if (a->dead)
    {
      g_propagate_error (error, g_error_copy (a->dead_error));
      g_mutex_unlock (&a->lock);
      return nullptr;
    }
  Call *call = g_new0 (Call, 1);
  call->a = gbk_asker_ref (a);
  call->channel = gbk_peer_channel (a->server);
  call->q = g_async_queue_new_full ((GDestroyNotify) reply_free);
  g_hash_table_insert (a->calls, &call->channel, call);
  g_mutex_unlock (&a->lock);
  return call;
}

static void
call_end (Call *call)
{
  g_mutex_lock (&call->a->lock);
  g_hash_table_remove (call->a->calls, &call->channel);
  g_mutex_unlock (&call->a->lock);
  g_async_queue_unref (call->q);
  gbk_asker_unref (call->a);
  g_free (call);
}

static void
call_send (const Call *call, const GByteArray *frame)
{
  gbk_outbox_put (call->a->out, frame);
}

/* The next answer, checking `src`'s cancellation while waiting. nullptr with *error when
 * cancelled or the connection died. */
[[nodiscard]] static Reply *
call_wait (Call *call, const GbkSource *src, GError **error)
{
  for (;;)
    {
      Reply *r = g_async_queue_timeout_pop (call->q, WAIT_STEP_US);
      if (r != nullptr && r->error != nullptr)
        {
          g_propagate_error (error, g_steal_pointer (&r->error));
          reply_free (r);
          return nullptr;
        }
      if (r != nullptr)
        return r;
      if (src->cancelled != nullptr && src->cancelled (src->cancel_data))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          return nullptr;
        }
    }
}

static GbkFrame
reply_frame (const Reply *r, uint64_t channel)
{
  gsize len;
  const uint8_t *data = g_bytes_get_data (r->payload, &len);
  return (GbkFrame) { r->type, channel, data, len };
}

/* An answer that isn't one of the expected kinds: the other end's FAILED, or a broken one. */
static void
unexpected (const Reply *r, uint64_t channel, GError **error)
{
  GbkFrame f = reply_frame (r, channel);
  if (f.type == GBK_MSG_FAILED && gbk_proto_get_failed (&f, error))
    return;
  g_propagate_error (error, not_understood ());
}

/* ---- the other machine as a copy's source ---- */

typedef struct
{
  GbkSource base;
  GbkAsker *a;            /* referenced */
  uint64_t session;
  bool own_session;       /* opened by us: forgotten when done */
} AskSource;

typedef struct
{
  GbkSourceFile base;
  Call *call;
  GBytes *chunk;          /* the DATA being copied out */
  size_t off;
  uint32_t unacked;       /* bytes copied out since the last CREDIT */
  bool ended;
} AskFile;

static const AskSource *
ask_source (const GbkSource *src)
{
  return (const AskSource *) (const void *) src;
}

/* One request for one answer: the answer of type `want`, or nullptr with *error. */
[[nodiscard]] static Reply *
ask_once (const GbkSource *src, GbkMsgType type, const char *path, GbkMsgType want,
          uint64_t *channel, GError **error)
{
  const AskSource *s = ask_source (src);
  Call *call = call_start (s->a, error);
  if (call == nullptr)
    return nullptr;
  g_autoptr (GByteArray) req = g_byte_array_new ();
  gbk_proto_put_request (req, type, call->channel, s->session, path);
  call_send (call, req);
  Reply *r = call_wait (call, src, error);
  if (r != nullptr && r->type != want)
    {
      unexpected (r, call->channel, error);
      g_clear_pointer (&r, reply_free);
    }
  *channel = call->channel;
  call_end (call);
  return r;
}

[[nodiscard]] static GbkStatEntry *
ask_lstat (GbkSource *src, const char *path, GError **error)
{
  uint64_t ch;
  Reply *r = ask_once (src, GBK_MSG_LSTAT, path, GBK_MSG_STATED, &ch, error);
  if (r == nullptr)
    return nullptr;
  GbkFrame f = reply_frame (r, ch);
  GbkStatEntry *e = gbk_proto_get_stated (&f);
  if (e == nullptr)
    g_propagate_error (error, not_understood ());
  reply_free (r);
  return e;
}

[[nodiscard]] static bool
ask_remove (GbkSource *src, const char *path, GError **error)
{
  uint64_t ch;
  Reply *r = ask_once (src, GBK_MSG_REMOVE, path, GBK_MSG_REMOVED, &ch, error);
  if (r == nullptr)
    return false;
  GbkFrame f = reply_frame (r, ch);
  bool ok = gbk_proto_get_empty (&f);
  if (!ok)
    g_propagate_error (error, not_understood ());
  reply_free (r);
  return ok;
}

[[nodiscard]] static GPtrArray *
ask_list (GbkSource *src, const char *path, GError **error)
{
  const AskSource *s = ask_source (src);
  Call *call = call_start (s->a, error);
  if (call == nullptr)
    return nullptr;
  g_autoptr (GByteArray) req = g_byte_array_new ();
  gbk_proto_put_request (req, GBK_MSG_LIST, call->channel, s->session, path);
  call_send (call, req);
  g_autoptr (GPtrArray) all = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_stat_entry_free);
  g_autoptr (GHashTable) seen = g_hash_table_new (g_str_hash, g_str_equal);
  bool last = false, ok = true;
  while (ok && !last)
    {
      Reply *r = call_wait (call, src, error);
      if (r == nullptr)
        break;
      GbkFrame f = reply_frame (r, call->channel);
      GPtrArray *part = nullptr;
      ok = f.type == GBK_MSG_LISTING && gbk_proto_get_listing (&f, &part, &last);
      if (!ok)
        unexpected (r, call->channel, error);
      for (guint i = 0; ok && i < part->len; i++)
        {
          GbkStatEntry *e = g_ptr_array_index (part, i);
          if (!g_hash_table_add (seen, e->name))
            {
              ok = false;                /* the same name in two parts */
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                   "The other machine listed a name twice");
              break;
            }
          g_ptr_array_add (all, e);
          part->pdata[i] = nullptr;      /* moved */
        }
      if (part != nullptr)
        g_ptr_array_unref (part);
      reply_free (r);
    }
  call_end (call);
  return ok && last ? g_steal_pointer (&all) : nullptr;
}

[[nodiscard]] static GbkSourceFile *
ask_open (GbkSource *src, const char *path, GError **error)
{
  const AskSource *s = ask_source (src);
  Call *call = call_start (s->a, error);
  if (call == nullptr)
    return nullptr;
  g_autoptr (GByteArray) req = g_byte_array_new ();
  gbk_proto_put_read (req, call->channel, s->session, path, READ_WINDOW);
  call_send (call, req);
  AskFile *f = g_new0 (AskFile, 1);
  f->base.source = src;
  f->call = call;
  return &f->base;
}

[[nodiscard]] static ssize_t
ask_read (GbkSourceFile *file, void *buf, size_t len, GError **error)
{
  AskFile *f = (AskFile *) (void *) file;
  while (f->chunk == nullptr || f->off == g_bytes_get_size (f->chunk))
    {
      g_clear_pointer (&f->chunk, g_bytes_unref);
      if (f->ended)
        return 0;
      Reply *r = call_wait (f->call, file->source, error);
      if (r == nullptr)
        return -1;
      GbkFrame frame = reply_frame (r, f->call->channel);
      const uint8_t *data;
      size_t n;
      bool changed;
      bool failed = false;
      if (gbk_proto_get_data (&frame, &data, &n))
        {
          f->chunk = g_bytes_ref (r->payload);
          f->off = 0;
        }
      else if (gbk_proto_get_end (&frame, &changed))
        {
          f->ended = true;
          if (changed)
            {
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "It changed while it was being copied");
              failed = true;
            }
        }
      else
        {
          f->ended = true;               /* FAILED ends it on the other side too */
          unexpected (r, f->call->channel, error);
          failed = true;
        }
      reply_free (r);
      if (failed)
        return -1;
    }
  size_t take = MIN (len, g_bytes_get_size (f->chunk) - f->off);
  memcpy (buf, (const uint8_t *) g_bytes_get_data (f->chunk, nullptr) + f->off, take);
  f->off += take;
  f->unacked += (uint32_t) take;
  if (f->unacked >= READ_WINDOW / 2)
    {
      g_autoptr (GByteArray) credit = g_byte_array_new ();
      gbk_proto_put_credit (credit, f->call->channel, f->unacked);
      call_send (f->call, credit);
      f->unacked = 0;
    }
  return (ssize_t) take;
}

static int
ask_fd (const GbkSourceFile *f G_GNUC_UNUSED)
{
  return -1;
}

static void
ask_close (GbkSourceFile *file)
{
  AskFile *f = (AskFile *) (void *) file;
  if (!f->ended)
    {
      g_autoptr (GByteArray) cancel = g_byte_array_new ();
      gbk_proto_put_empty (cancel, GBK_MSG_CANCEL, f->call->channel);
      call_send (f->call, cancel);
    }
  g_clear_pointer (&f->chunk, g_bytes_unref);
  call_end (f->call);
  g_free (f);
}

static void
ask_free (GbkSource *src)
{
  AskSource *s = (AskSource *) (void *) src;
  if (s->own_session)
    {
      g_autoptr (GByteArray) forget = g_byte_array_new ();
      gbk_proto_put_empty (forget, GBK_MSG_FORGET, s->session);
      gbk_outbox_put (s->a->out, forget);
    }
  use_end (s->a);
  gbk_asker_unref (s->a);
  g_free (s);
}

static const GbkSourceOps ask_ops = {
  .lstat = ask_lstat,
  .list = ask_list,
  .open = ask_open,
  .read = ask_read,
  .fd = ask_fd,
  .close = ask_close,
  .remove = ask_remove,
  .free = ask_free,
};

static GbkSource *
source_new (GbkAsker *a, uint64_t session, bool own)
{
  AskSource *s = g_new0 (AskSource, 1);
  s->base.ops = &ask_ops;
  s->base.local = false;
  s->a = gbk_asker_ref (a);
  s->session = session;
  s->own_session = own;
  use_begin (a);
  return &s->base;
}

GbkSource *
gbk_asker_source (GbkAsker *a, bool record)
{
  uint64_t session = gbk_peer_channel (a->server);
  g_autoptr (GByteArray) open = g_byte_array_new ();
  gbk_proto_put_session (open, session, record);
  gbk_outbox_put (a->out, open);
  return source_new (a, session, true);
}

GbkSource *
gbk_asker_source_in (GbkAsker *a, uint64_t session)
{
  return source_new (a, session, false);
}

/* ---- a job on the other machine ---- */

static GbkChoice
answer_question (const GbkAsk *q, const GbkOpsHooks *h, bool *all)
{
  *all = false;
  switch (q->kind)
    {
    case GBK_ASK_CONFLICT:
      return h->conflict != nullptr ? h->conflict (q->path, q->other, q->other_is_dir, all, h->data)
                                    : GBK_CHOICE_SKIP;
    case GBK_ASK_ERROR:
      return h->error != nullptr ? h->error (q->path, q->error, h->data) : GBK_CHOICE_CANCEL;
    case GBK_ASK_UNTRASHABLE:
      return h->delete_untrashable != nullptr && h->delete_untrashable (q->path, h->data)
                 ? GBK_CHOICE_REPLACE
                 : GBK_CHOICE_SKIP;
    default: gbk_fatal ("bad question %d", (int) q->kind);
    }
}

/* One frame of a running job. True when it has finished (*ok and *error say how). */
[[nodiscard]] static bool
job_frame (const Call *call, const GbkFrame *f, const GbkOpsHooks *h, GPtrArray *created, bool *ok,
           GError **error)
{
  switch (f->type)
    {
    case GBK_MSG_QUESTION:
      {
        GbkAsk q;
        if (!gbk_proto_get_question (f, &q))
          break;
        bool all;
        GbkChoice ch = answer_question (&q, h, &all);
        gbk_ask_clear (&q);
        g_autoptr (GByteArray) answer = g_byte_array_new ();
        gbk_proto_put_answer (answer, call->channel, ch, all);
        call_send (call, answer);
        return false;
      }
    case GBK_MSG_PROGRESS:
      {
        uint64_t done, total;
        g_autofree char *current = nullptr;
        if (!gbk_proto_get_progress (f, &done, &total, &current))
          break;
        if (h->progress != nullptr)
          h->progress (done, total, current, h->data);
        return false;
      }
    case GBK_MSG_CREATED:
      {
        g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
        if (!gbk_proto_get_created (f, names))
          break;
        for (guint i = 0; created != nullptr && i < names->len; i++)
          g_ptr_array_add (created, g_steal_pointer (&names->pdata[i]));
        return false;
      }
    case GBK_MSG_DONE:
      if (!gbk_proto_get_empty (f))
        break;
      *ok = true;
      return true;
    case GBK_MSG_FAILED:
      if (!gbk_proto_get_failed (f, error))
        break;
      return true;
    case GBK_MSG_HELLO:
    case GBK_MSG_SCAN:
    case GBK_MSG_INFO:
    case GBK_MSG_PONG:
    case GBK_MSG_LISTING:
    case GBK_MSG_STATED:
    case GBK_MSG_DATA:
    case GBK_MSG_END:
    case GBK_MSG_REMOVED:
    case GBK_MSG_WATCH:
    case GBK_MSG_UNWATCH:
    case GBK_MSG_GET_INFO:
    case GBK_MSG_PING:
    case GBK_MSG_LIST:
    case GBK_MSG_LSTAT:
    case GBK_MSG_READ:
    case GBK_MSG_CREDIT:
    case GBK_MSG_CANCEL:
    case GBK_MSG_REMOVE:
    case GBK_MSG_SESSION:
    case GBK_MSG_FORGET:
    case GBK_MSG_SOURCES:
    case GBK_MSG_JOB:
    case GBK_MSG_ANSWER:
    default: break;
    }
  g_propagate_error (error, not_understood ());
  return true;
}

bool
gbk_asker_run_job (GbkAsker *a, const GbkJobRequest *r, const GPtrArray *sources,
                   const GbkOpsHooks *hooks, GPtrArray *created, GError **error)
{
  Call *call = call_start (a, error);
  if (call == nullptr)
    return false;
  use_begin (a);
  g_autoptr (GByteArray) req = g_byte_array_new ();
  gbk_proto_put_job (req, call->channel, r, sources);
  call_send (call, req);
  bool ok = false, finished = false, cancel_sent = false;
  while (!finished)
    {
      Reply *reply = g_async_queue_timeout_pop (call->q, WAIT_STEP_US);
      if (reply == nullptr)
        {
          /* Cancelled here: stop it there, and wait for it to say it has stopped. */
          if (!cancel_sent && hooks->cancelled != nullptr && hooks->cancelled (hooks->data))
            {
              g_autoptr (GByteArray) cancel = g_byte_array_new ();
              gbk_proto_put_empty (cancel, GBK_MSG_CANCEL, call->channel);
              call_send (call, cancel);
              cancel_sent = true;
            }
          continue;
        }
      if (reply->error != nullptr)
        {
          g_propagate_error (error, g_steal_pointer (&reply->error));
          finished = true;
        }
      else
        {
          GbkFrame f = reply_frame (reply, call->channel);
          finished = job_frame (call, &f, hooks, created, &ok, error);
        }
      reply_free (reply);
    }
  if (!ok && !cancel_sent)
    {
      /* A broken answer: make sure it stops there (a finished job ignores this). */
      g_autoptr (GByteArray) cancel = g_byte_array_new ();
      gbk_proto_put_empty (cancel, GBK_MSG_CANCEL, call->channel);
      call_send (call, cancel);
    }
  use_end (a);
  call_end (call);
  return ok;
}

/* ---- answering ---- */

/* A file as it was listed: what a move's REMOVE compares against. */
typedef struct
{
  uint32_t mode;
  uint64_t dev, ino, size, nlink;
  int64_t mtime_ns, ctime_ns;
} Token;

typedef struct
{
  GPtrArray *roots;       /* restricted: what may be asked about (char*) */
  bool record;
  GHashTable *tokens;     /* record: path -> Token* */
} Session;

struct GbkAnswerer
{
  gatomicrefcount rc;
  bool restricted;
  GbkOutbox *out;
  GMutex lock;            /* guards everything below */
  GHashTable *sessions;   /* uint64_t* -> Session* */
  GHashTable *reads;      /* &Task.channel -> Task*: READs running (their worker owns them,
                      * and unlists them under the lock before freeing them) */
  int tasks;
  bool stopped;
};

typedef enum
{
  TASK_INFO,
  TASK_LIST,
  TASK_LSTAT,
  TASK_READ,
  TASK_REMOVE,
} TaskKind;

typedef struct
{
  GbkAnswerer *a;         /* referenced */
  TaskKind kind;
  uint64_t channel;
  uint64_t session;
  char *path;             /* as asked */
  char *base;             /* a folder, opened as it is */
  char *rel;              /* `path` from `base`: resolved with `resolve` */
  uint64_t resolve;
  /* READ: how many more bytes may be sent; cancelled by CANCEL or stopping. */
  GMutex lock;
  GCond cond;
  uint64_t credit;
  bool cancelled;
} Task;

static int read_delay_ms;                /* atomic; tests */

void
gbk_answerer_set_read_delay_for_test (int ms)
{
  g_atomic_int_set (&read_delay_ms, ms);
}

static void
session_free (Session *s)
{
  g_clear_pointer (&s->roots, g_ptr_array_unref);
  g_clear_pointer (&s->tokens, g_hash_table_unref);
  g_free (s);
}

GbkAnswerer *
gbk_answerer_new (bool restricted, GbkOutbox *out)
{
  GbkAnswerer *a = g_new0 (GbkAnswerer, 1);
  g_atomic_ref_count_init (&a->rc);
  a->restricted = restricted;
  a->out = gbk_outbox_ref (out);
  g_mutex_init (&a->lock);
  a->sessions
      = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, (GDestroyNotify) session_free);
  a->reads = g_hash_table_new (g_int64_hash, g_int64_equal);
  return a;
}

GbkAnswerer *
gbk_answerer_ref (GbkAnswerer *a)
{
  g_atomic_ref_count_inc (&a->rc);
  return a;
}

void
gbk_answerer_unref (GbkAnswerer *a)
{
  if (a == nullptr || !g_atomic_ref_count_dec (&a->rc))
    return;
  gbk_outbox_unref (a->out);
  g_mutex_clear (&a->lock);
  g_hash_table_unref (a->sessions);
  g_hash_table_unref (a->reads);   /* empty: each running read holds a reference to us */
  g_free (a);
}

static void
task_free (Task *t)
{
  gbk_answerer_unref (t->a);
  g_free (t->path);
  g_free (t->base);
  g_free (t->rel);
  g_mutex_clear (&t->lock);
  g_cond_clear (&t->cond);
  g_free (t);
}

bool
gbk_answerer_takes (GbkMsgType type)
{
  switch (type)
    {
    case GBK_MSG_LIST:
    case GBK_MSG_LSTAT:
    case GBK_MSG_READ:
    case GBK_MSG_CREDIT:
    case GBK_MSG_CANCEL:
    case GBK_MSG_REMOVE:
    case GBK_MSG_SESSION:
    case GBK_MSG_FORGET:
    case GBK_MSG_GET_INFO: return true;
    case GBK_MSG_HELLO:
    case GBK_MSG_SCAN:
    case GBK_MSG_INFO:
    case GBK_MSG_FAILED:
    case GBK_MSG_PONG:
    case GBK_MSG_LISTING:
    case GBK_MSG_STATED:
    case GBK_MSG_DATA:
    case GBK_MSG_END:
    case GBK_MSG_REMOVED:
    case GBK_MSG_QUESTION:
    case GBK_MSG_PROGRESS:
    case GBK_MSG_CREATED:
    case GBK_MSG_DONE:
    case GBK_MSG_WATCH:
    case GBK_MSG_UNWATCH:
    case GBK_MSG_PING:
    case GBK_MSG_SOURCES:
    case GBK_MSG_JOB:
    case GBK_MSG_ANSWER:
    default: return false;
    }
}

/* The task's last frames: it no longer counts, and can't be steered any more. */
static void
task_finish (Task *t, const GByteArray *frames)
{
  GbkAnswerer *a = t->a;
  gbk_outbox_put (a->out, frames);
  g_mutex_lock (&a->lock);
  a->tasks--;
  if (g_hash_table_lookup (a->reads, &t->channel) == t)
    g_hash_table_remove (a->reads, &t->channel);
  g_mutex_unlock (&a->lock);
}

static void
finish_error (Task *t, const GError *error)
{
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_failed (b, t->channel, error);
  task_finish (t, b);
}

static void
finish_errno (Task *t, int err)
{
  g_autoptr (GError) e
      = g_error_new_literal (G_IO_ERROR, (int) g_io_error_from_errno (err), g_strerror (err));
  finish_error (t, e);
}

/* `rel` under `base`, as the task's session allows. -1 with errno. */
static int
open_rel (const Task *t, const char *rel, int flags)
{
  int base = open (t->base, O_PATH | O_DIRECTORY | O_CLOEXEC);
  if (base < 0)
    return -1;
  struct open_how how = { .flags = (unsigned) (flags | O_CLOEXEC), .resolve = t->resolve };
  int fd = (int) openat2 (base, rel, &how, sizeof how);
  int err = errno;
  close (base);
  errno = err;
  return fd;
}

static Token *
token_new (const GbkStatEntry *e)
{
  Token *k = g_new0 (Token, 1);
  k->mode = e->mode;
  k->dev = e->dev;
  k->ino = e->ino;
  k->size = e->size;
  k->nlink = e->nlink;
  k->mtime_ns = e->mtime_ns;
  k->ctime_ns = e->ctime_ns;
  return k;
}

static void
keep_first (GHashTable *tokens, char *path, const GbkStatEntry *e)
{
  if (g_hash_table_contains (tokens, path))
    g_free (path);
  else
    g_hash_table_insert (tokens, path, token_new (e));
}

/* Keep what was listed, for a move's REMOVE: entries of `dir` (nullptr: `one` is `path`).
 * The first listing counts (a copy lists a folder twice: to measure, then to copy): a file
 * that changed after it is kept, even if it changed before it was read. */
static void
record (Task *t, const char *dir, const GPtrArray *entries, const GbkStatEntry *one)
{
  g_mutex_lock (&t->a->lock);
  Session *s = g_hash_table_lookup (t->a->sessions, &t->session);
  if (s != nullptr && s->record)
    {
      if (one != nullptr)
        keep_first (s->tokens, g_strdup (t->path), one);
      for (guint i = 0; entries != nullptr && i < entries->len; i++)
        {
          const GbkStatEntry *e = g_ptr_array_index (entries, i);
          keep_first (s->tokens, g_build_filename (dir, e->name, nullptr), e);
        }
    }
  g_mutex_unlock (&t->a->lock);
}

static void
run_info (Task *t)
{
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkFileInfo) info = gbk_ops_info (t->path, &error);
  g_autoptr (GByteArray) b = g_byte_array_new ();
  if (info == nullptr)
    {
      finish_error (t, error);
      return;
    }
  gbk_proto_put_info (b, t->channel, info);
  task_finish (t, b);
}

static void
run_list (Task *t)
{
  int fd = open_rel (t, t->rel, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  DIR *d = fd >= 0 ? fdopendir (fd) : nullptr;
  if (d == nullptr)
    {
      int err = errno;
      if (fd >= 0)
        close (fd);
      finish_errno (t, err);
      return;
    }
  int dfd = dirfd (d);
  if (dfd < 0)
    {
      int err = errno;
      closedir (d);
      finish_errno (t, err);
      return;
    }
  g_autoptr (GPtrArray) batch
      = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_stat_entry_free);
  const struct dirent *de;
  g_autoptr (GError) stat_error = nullptr;
  int err = 0;
  errno = 0;
  while ((de = readdir (d)) != nullptr)
    {
      if (g_str_equal (de->d_name, ".") || g_str_equal (de->d_name, ".."))
        continue;
      g_autoptr (GError) e = nullptr;
      GbkStatEntry *entry = gbk_stat_entry_at (dfd, de->d_name, de->d_name, &e);
      if (entry == nullptr)
        {
          /* Gone meanwhile: not part of it any more. Anything else fails the listing. */
          if (!g_error_matches (e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            stat_error = g_steal_pointer (&e);
          if (stat_error != nullptr)
            break;
          continue;
        }
      g_ptr_array_add (batch, entry);
      if (batch->len == LIST_BATCH)
        {
          record (t, t->path, batch, nullptr);
          g_autoptr (GByteArray) b = g_byte_array_new ();
          gbk_proto_put_listing (b, t->channel, batch, false);
          gbk_outbox_put (t->a->out, b);
          g_ptr_array_set_size (batch, 0);
        }
      errno = 0;
    }
  if (stat_error == nullptr && errno != 0)
    err = errno;                         /* readdir failed */
  closedir (d);
  /* The asker drops what it has: a listing is whole or nothing. */
  if (stat_error != nullptr)
    {
      finish_error (t, stat_error);
      return;
    }
  if (err != 0)
    {
      finish_errno (t, err);
      return;
    }
  record (t, t->path, batch, nullptr);
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_listing (b, t->channel, batch, true);
  task_finish (t, b);
}

static void
run_lstat (Task *t)
{
  /* O_PATH | O_NOFOLLOW: a link itself, not what it points to. */
  int fd = open_rel (t, t->rel, O_PATH | O_NOFOLLOW);
  if (fd < 0)
    {
      finish_errno (t, errno);
      return;
    }
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkStatEntry) e = gbk_stat_entry_at (fd, "", nullptr, &error);
  close (fd);
  if (e == nullptr)
    {
      finish_error (t, error);
      return;
    }
  record (t, nullptr, nullptr, e);
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_stated (b, t->channel, e);
  task_finish (t, b);
}

static bool
same_file_state (const struct stat *a, const struct stat *b)
{
  return a->st_ino == b->st_ino && a->st_size == b->st_size
         && a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec
         && a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

/* A regular file's bytes, no faster than the asker's credit allows; at the end, whether it
 * changed while being read (then the copy is useless, and the asker says so). */
static void
run_read (Task *t)
{
  /* O_NONBLOCK: a FIFO in its place mustn't hang the open; a regular file ignores it. */
  int fd = open_rel (t, t->rel, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  struct stat before;
  if (fd < 0 || fstat (fd, &before) != 0)
    {
      int err = errno;
      if (fd >= 0)
        close (fd);
      finish_errno (t, err);
      return;
    }
  if (!S_ISREG (before.st_mode))
    {
      close (fd);
      finish_errno (t, EINVAL);
      return;
    }
  g_autofree uint8_t *buf = g_malloc (GBK_PROTO_DATA_MAX);
  for (;;)
    {
      g_mutex_lock (&t->lock);
      while (t->credit == 0 && !t->cancelled)
        g_cond_wait (&t->cond, &t->lock);
      bool stop = t->cancelled;
      size_t take = (size_t) MIN (t->credit, (uint64_t) GBK_PROTO_DATA_MAX);
      g_mutex_unlock (&t->lock);
      if (stop)
        {
          close (fd);
          g_autoptr (GByteArray) nothing = g_byte_array_new ();
          task_finish (t, nothing);      /* nothing more to say */
          return;
        }
      int delay = g_atomic_int_get (&read_delay_ms);
      if (delay > 0)
        g_usleep ((gulong) delay * 1000);
      ssize_t n = read (fd, buf, take);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0)
        {
          int err = errno;
          close (fd);
          finish_errno (t, err);
          return;
        }
      if (n == 0)
        break;
      g_autoptr (GByteArray) b = g_byte_array_new ();
      gbk_proto_put_data (b, t->channel, buf, (size_t) n);
      g_mutex_lock (&t->lock);
      t->credit -= (uint64_t) n;
      g_mutex_unlock (&t->lock);
      gbk_outbox_put (t->a->out, b);
    }
  struct stat after;
  bool changed = fstat (fd, &after) != 0 || !same_file_state (&before, &after);
  close (fd);
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_end (b, t->channel, changed);
  task_finish (t, b);
}

static int64_t
ns (struct timespec ts)
{
  return (int64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* GbkUnchangedFn: is it as listed in the task's session? A folder only has to be the same
 * folder (taking its contents out changes its times), and a file with other links may have
 * a new change time (removing one of them, maybe by this move, changes it). */
static bool
unchanged (const char *path, const struct stat *st, void *data)
{
  const Task *t = data;
  g_mutex_lock (&t->a->lock);
  const Session *s = g_hash_table_lookup (t->a->sessions, &t->session);
  const Token *k = s != nullptr ? g_hash_table_lookup (s->tokens, path) : nullptr;
  bool same = k != nullptr && (k->mode & S_IFMT) == (st->st_mode & S_IFMT)
              && k->dev == (uint64_t) st->st_dev && k->ino == (uint64_t) st->st_ino;
  if (same && !S_ISDIR (st->st_mode))
    same = k->size == (uint64_t) st->st_size && k->mtime_ns == ns (st->st_mtim)
           && (k->ctime_ns == ns (st->st_ctim) || k->nlink > 1);
  g_mutex_unlock (&t->a->lock);
  return same;
}

static void
run_remove (Task *t)
{
  /* The folder it is in: the session's own base, or found below it like everything else. */
  g_autofree char *dir_rel = g_path_get_dirname (t->rel);
  g_autofree char *name = g_path_get_basename (t->rel);
  int dir = g_str_equal (dir_rel, ".") ? open (t->base, O_RDONLY | O_DIRECTORY | O_CLOEXEC)
                                       : open_rel (t, dir_rel, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (dir < 0)
    {
      finish_errno (t, errno);
      return;
    }
  g_autoptr (GError) error = nullptr;
  bool ok = gbk_ops_remove_unchanged (dir, name, t->path, unchanged, t, &error);
  close (dir);
  if (!ok)
    {
      finish_error (t, error);
      return;
    }
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_empty (b, GBK_MSG_REMOVED, t->channel);
  task_finish (t, b);
}

static void *
task_worker (void *data)
{
  Task *t = data;
  switch (t->kind)
    {
    case TASK_INFO: run_info (t); break;
    case TASK_LIST: run_list (t); break;
    case TASK_LSTAT: run_lstat (t); break;
    case TASK_READ: run_read (t); break;
    case TASK_REMOVE: run_remove (t); break;
    default: gbk_fatal ("bad task %d", (int) t->kind);
    }
  task_free (t);
  return nullptr;
}

/* Each part of `rest` ("" or "/a/b") is a real name: no "", "." or "..". */
static bool
plain_names (const char *rest)
{
  if (rest[0] == '\0')
    return true;
  g_auto (GStrv) parts = g_strsplit (rest + 1, "/", -1);
  for (size_t i = 0; parts[i] != nullptr; i++)
    if (parts[i][0] == '\0' || g_str_equal (parts[i], ".") || g_str_equal (parts[i], ".."))
      return false;
  return true;
}

/* Where the task's path is (base, rel, resolve), if its session allows it (lock held). */
[[nodiscard]] static bool
place (GbkAnswerer *a, Task *t, GError **error)
{
  if (!a->restricted)
    {
      t->base = g_path_get_dirname (t->path);
      t->rel = g_path_get_basename (t->path);
      t->resolve = 0;
      if (t->kind == TASK_REMOVE && g_str_equal (t->path, "/"))
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                               "That can't be removed");
          return false;
        }
      return true;
    }
  const Session *s = g_hash_table_lookup (a->sessions, &t->session);
  for (guint i = 0; s != nullptr && i < s->roots->len; i++)
    {
      const char *root = g_ptr_array_index (s->roots, i);
      size_t n = strlen (root);
      if (strncmp (t->path, root, n) != 0 || (t->path[n] != '\0' && t->path[n] != '/')
          || !plain_names (t->path + n))
        continue;
      g_autofree char *name = g_path_get_basename (root);
      t->base = g_path_get_dirname (root);
      t->rel = g_strconcat (name, t->path + n, nullptr);
      t->resolve = STRICT;
      return true;
    }
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                       "The other machine asked for a file this job doesn't include");
  return false;
}

/* Start one task (the request is decoded). Fails it at once if it can't run. */
static void
start (GbkAnswerer *a, TaskKind kind, uint64_t channel, uint64_t session, char *path,
       uint32_t credit)
{
  Task *t = g_new0 (Task, 1);
  t->a = gbk_answerer_ref (a);
  t->kind = kind;
  t->channel = channel;
  t->session = session;
  t->path = path;
  g_mutex_init (&t->lock);
  g_cond_init (&t->cond);
  t->credit = credit;
  g_autoptr (GError) error = nullptr;
  g_mutex_lock (&a->lock);
  bool ok = true;
  if (a->stopped || a->tasks >= TASKS_MAX)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_BUSY, "Too many requests at once");
      ok = false;
    }
  else if (!a->restricted && kind != TASK_INFO && !g_hash_table_contains (a->sessions, &session))
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, "Not a session of this machine");
      ok = false;
    }
  else if (kind == TASK_REMOVE)
    {
      const Session *s = g_hash_table_lookup (a->sessions, &session);
      if (s == nullptr || !s->record)
        {
          g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                               "Only what a move copied can be removed");
          ok = false;
        }
    }
  if (ok)
    ok = place (a, t, &error);
  if (ok)
    {
      a->tasks++;
      if (kind == TASK_READ)
        g_hash_table_replace (a->reads, &t->channel, t);
    }
  g_mutex_unlock (&a->lock);
  if (!ok)
    {
      g_autoptr (GByteArray) b = g_byte_array_new ();
      gbk_proto_put_failed (b, channel, error);
      gbk_outbox_put (a->out, b);
      task_free (t);
      return;
    }
  GThread *thread = g_thread_try_new ("gbk-answer", task_worker, t, &error);
  if (thread == nullptr)
    {
      finish_error (t, error);           /* counts it out and unlists it again */
      task_free (t);
      return;
    }
  g_thread_unref (thread);
}

/* CREDIT and CANCEL for a READ (one already finished: nothing to do). */
static void
steer_read (GbkAnswerer *a, uint64_t channel, uint32_t credit, bool cancel)
{
  g_mutex_lock (&a->lock);
  Task *t = g_hash_table_lookup (a->reads, &channel);
  if (t != nullptr)
    {
      g_mutex_lock (&t->lock);
      if (cancel)
        t->cancelled = true;
      else
        t->credit = MIN (t->credit + credit, (uint64_t) UINT32_MAX * 4);
      g_cond_signal (&t->cond);
      g_mutex_unlock (&t->lock);
    }
  g_mutex_unlock (&a->lock);
}

static Session *
session_new (bool record)
{
  Session *s = g_new0 (Session, 1);
  s->record = record;
  s->tokens = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  return s;
}

bool
gbk_answerer_handle (GbkAnswerer *a, const GbkFrame *f)
{
  switch (f->type)
    {
    case GBK_MSG_LIST:
    case GBK_MSG_LSTAT:
    case GBK_MSG_REMOVE:
      {
        uint64_t session;
        char *path;
        if (!gbk_proto_get_request (f, &session, &path))
          return false;
        TaskKind kind = f->type == GBK_MSG_LIST    ? TASK_LIST
                        : f->type == GBK_MSG_LSTAT ? TASK_LSTAT
                                                   : TASK_REMOVE;
        start (a, kind, f->channel, session, path, 0);
        return true;
      }
    case GBK_MSG_READ:
      {
        uint64_t session;
        char *path;
        uint32_t credit;
        if (!gbk_proto_get_read (f, &session, &path, &credit) || f->channel == 0)
          return false;
        g_mutex_lock (&a->lock);
        bool used = g_hash_table_contains (a->reads, &f->channel);
        g_mutex_unlock (&a->lock);
        if (used)
          {
            g_free (path);
            return false;                /* a channel already reading */
          }
        start (a, TASK_READ, f->channel, session, path, credit);
        return true;
      }
    case GBK_MSG_GET_INFO:
      {
        if (a->restricted)
          return false;                  /* the server never asks the laptop that */
        char *path = gbk_proto_get_path (f);
        if (path == nullptr)
          return false;
        start (a, TASK_INFO, f->channel, 0, path, 0);
        return true;
      }
    case GBK_MSG_CREDIT:
      {
        uint32_t bytes;
        if (!gbk_proto_get_credit (f, &bytes))
          return false;
        steer_read (a, f->channel, bytes, false);
        return true;
      }
    case GBK_MSG_CANCEL:
      if (!gbk_proto_get_empty (f))
        return false;
      steer_read (a, f->channel, 0, true);
      return true;
    case GBK_MSG_SESSION:
      {
        bool rec;
        if (a->restricted || !gbk_proto_get_session (f, &rec))
          return false;                  /* the laptop opens its own sessions */
        g_mutex_lock (&a->lock);
        bool ok = !g_hash_table_contains (a->sessions, &f->channel)
                  && g_hash_table_size (a->sessions) < SESSIONS_MAX;
        if (ok)
          g_hash_table_insert (a->sessions, g_memdup2 (&f->channel, sizeof f->channel),
                               session_new (rec));
        g_mutex_unlock (&a->lock);
        return ok;
      }
    case GBK_MSG_FORGET:
      if (a->restricted || !gbk_proto_get_empty (f))
        return false;
      g_mutex_lock (&a->lock);
      g_hash_table_remove (a->sessions, &f->channel);
      g_mutex_unlock (&a->lock);
      return true;
    case GBK_MSG_HELLO:
    case GBK_MSG_SCAN:
    case GBK_MSG_INFO:
    case GBK_MSG_FAILED:
    case GBK_MSG_PONG:
    case GBK_MSG_LISTING:
    case GBK_MSG_STATED:
    case GBK_MSG_DATA:
    case GBK_MSG_END:
    case GBK_MSG_REMOVED:
    case GBK_MSG_QUESTION:
    case GBK_MSG_PROGRESS:
    case GBK_MSG_CREATED:
    case GBK_MSG_DONE:
    case GBK_MSG_WATCH:
    case GBK_MSG_UNWATCH:
    case GBK_MSG_PING:
    case GBK_MSG_SOURCES:
    case GBK_MSG_JOB:
    case GBK_MSG_ANSWER:
    default: return false;
    }
}

uint64_t
gbk_answerer_open (GbkAnswerer *a, const GPtrArray *roots, bool record)
{
  uint64_t id = gbk_peer_channel (false);
  Session *s = session_new (record);
  s->roots = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < roots->len; i++)
    {
      const char *r = g_ptr_array_index (roots, i);
      /* Absolute, plain names, and never "/" itself (nothing is above it to start from). */
      if (r[0] == '/' && r[1] != '\0' && plain_names (r))
        g_ptr_array_add (s->roots, g_strdup (r));
    }
  g_mutex_lock (&a->lock);
  g_hash_table_insert (a->sessions, g_memdup2 (&id, sizeof id), s);
  g_mutex_unlock (&a->lock);
  return id;
}

void
gbk_answerer_close (GbkAnswerer *a, uint64_t session)
{
  g_mutex_lock (&a->lock);
  g_hash_table_remove (a->sessions, &session);
  g_mutex_unlock (&a->lock);
}

void
gbk_answerer_stop (GbkAnswerer *a)
{
  g_mutex_lock (&a->lock);
  a->stopped = true;
  GHashTableIter it;
  void *t;
  g_hash_table_iter_init (&it, a->reads);
  while (g_hash_table_iter_next (&it, nullptr, &t))
    {
      Task *task = t;
      g_mutex_lock (&task->lock);
      task->cancelled = true;
      g_cond_signal (&task->cond);
      g_mutex_unlock (&task->lock);
    }
  g_mutex_unlock (&a->lock);
}

int
gbk_answerer_busy (GbkAnswerer *a)
{
  g_mutex_lock (&a->lock);
  int n = a->tasks;
  g_mutex_unlock (&a->lock);
  return n;
}

/* ---- running jobs (the server) ---- */

struct GbkJobRunner
{
  gatomicrefcount rc;
  GbkAsker *asker;
  GbkOutbox *out;
  GMutex lock;            /* guards everything below */
  GCond idle;             /* a job finished */
  GHashTable *pending;    /* channel (uint64_t*) -> GPtrArray* of sources: SOURCES so far */
  size_t pending_total;
  GHashTable *running;    /* &Job.channel -> Job*: owned by their threads, which unlist
                        * them under the lock before freeing them */
  bool stopped;
};

typedef struct
{
  GbkJobRunner *r;        /* referenced */
  uint64_t channel;
  GbkJobRequest req;
  GPtrArray *sources;
  GMutex lock;            /* guards the answer */
  GCond cond;
  bool answered;
  GbkChoice answer;
  bool all;
  int cancelled;          /* atomic */
  int64_t last_progress;  /* the job's thread only */
} Job;

static void
runner_unref_internal (GbkJobRunner *r)
{
  if (!g_atomic_ref_count_dec (&r->rc))
    return;
  gbk_asker_unref (r->asker);
  gbk_outbox_unref (r->out);
  g_mutex_clear (&r->lock);
  g_cond_clear (&r->idle);
  g_hash_table_unref (r->pending);
  g_hash_table_unref (r->running);
  g_free (r);
}

static void
job_free (Job *j)
{
  runner_unref_internal (j->r);
  gbk_job_request_clear (&j->req);
  g_ptr_array_unref (j->sources);
  g_mutex_clear (&j->lock);
  g_cond_clear (&j->cond);
  g_free (j);
}

GbkJobRunner *
gbk_job_runner_new (GbkAsker *asker, GbkOutbox *out)
{
  GbkJobRunner *r = g_new0 (GbkJobRunner, 1);
  g_atomic_ref_count_init (&r->rc);
  r->asker = gbk_asker_ref (asker);
  r->out = gbk_outbox_ref (out);
  g_mutex_init (&r->lock);
  g_cond_init (&r->idle);
  r->pending = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free,
                                      (GDestroyNotify) g_ptr_array_unref);
  r->running = g_hash_table_new (g_int64_hash, g_int64_equal);
  return r;
}

void
gbk_job_runner_unref (GbkJobRunner *r)
{
  if (r != nullptr)
    runner_unref_internal (r);
}

bool
gbk_job_runner_takes (GbkMsgType type)
{
  return type == GBK_MSG_SOURCES || type == GBK_MSG_JOB || type == GBK_MSG_ANSWER
         || type == GBK_MSG_CANCEL;
}

static void
job_send (Job *j, const GByteArray *frames)
{
  gbk_outbox_put (j->r->out, frames);
}

static bool
job_cancelled (void *data)
{
  return g_atomic_int_get (&((Job *) data)->cancelled) != 0;
}

/* Ask the laptop and wait for its answer (or for the job to be cancelled). */
static GbkChoice
job_ask (Job *j, GbkAskKind kind, const char *path, const char *other, bool other_is_dir,
         const GError *error, bool *all)
{
  g_mutex_lock (&j->lock);
  j->answered = false;
  g_mutex_unlock (&j->lock);
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_question (b, j->channel, kind, path, other, other_is_dir, error);
  job_send (j, b);
  g_mutex_lock (&j->lock);
  while (!j->answered && !job_cancelled (j))
    g_cond_wait (&j->cond, &j->lock);
  GbkChoice ch = j->answered && !job_cancelled (j) ? j->answer : GBK_CHOICE_CANCEL;
  if (all != nullptr)
    *all = j->answered && j->all;
  g_mutex_unlock (&j->lock);
  return ch;
}

static GbkChoice
job_conflict (const char *src, const char *dst, bool dst_is_dir, bool *apply_all, void *data)
{
  return job_ask (data, GBK_ASK_CONFLICT, src, dst, dst_is_dir, nullptr, apply_all);
}

static GbkChoice
job_error (const char *path, const GError *error, void *data)
{
  return job_ask (data, GBK_ASK_ERROR, path, nullptr, false, error, nullptr);
}

static bool
job_untrashable (const char *path, void *data)
{
  return job_ask (data, GBK_ASK_UNTRASHABLE, path, nullptr, false, nullptr, nullptr)
         == GBK_CHOICE_REPLACE;
}

static void
job_progress (uint64_t done, uint64_t total, const char *current, void *data)
{
  Job *j = data;
  int64_t now = g_get_monotonic_time ();
  if (now - j->last_progress < PROGRESS_EVERY_US)
    return;
  j->last_progress = now;
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_progress (b, j->channel, done, total, current);
  job_send (j, b);
}

static void *
job_worker (void *data)
{
  Job *j = data;
  GbkOpsHooks hooks = { job_conflict, job_error, job_untrashable, job_progress, job_cancelled, j };
  g_autoptr (GbkSource) src = j->req.sources_here
                                  ? gbk_asker_source_in (j->r->asker, j->req.session)
                                  : gbk_source_local ();
  g_autoptr (GPtrArray) created = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GError) error = nullptr;
  bool ok = gbk_ops_run_from (src, j->req.kind, j->sources, j->req.dest, &hooks, created, &error);
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_created (b, j->channel, created);
  if (ok)
    gbk_proto_put_empty (b, GBK_MSG_DONE, j->channel);
  else
    gbk_proto_put_failed (b, j->channel, error);
  job_send (j, b);
  GbkJobRunner *r = j->r;
  g_mutex_lock (&r->lock);
  g_hash_table_remove (r->running, &j->channel);
  g_cond_broadcast (&r->idle);
  g_mutex_unlock (&r->lock);
  job_free (j);
  return nullptr;
}

static void
job_fail (GbkJobRunner *r, uint64_t channel, GIOErrorEnum code, const char *msg)
{
  g_autoptr (GError) e = g_error_new_literal (G_IO_ERROR, (int) code, msg);
  g_autoptr (GByteArray) b = g_byte_array_new ();
  gbk_proto_put_failed (b, channel, e);
  gbk_outbox_put (r->out, b);
}

static bool
job_start (GbkJobRunner *r, uint64_t channel, GbkJobRequest *req)
{
  g_mutex_lock (&r->lock);
  GPtrArray *sources = nullptr;
  void *key = nullptr;
  bool had = g_hash_table_steal_extended (r->pending, &channel, &key, (void **) &sources);
  g_free (key);
  if (had)
    r->pending_total -= sources->len;
  bool busy = r->stopped || g_hash_table_size (r->running) >= JOBS_MAX;
  bool taken = had && g_hash_table_contains (r->running, &channel);
  g_mutex_unlock (&r->lock);
  if (!had || taken)
    {
      if (sources != nullptr)
        g_ptr_array_unref (sources);
      gbk_job_request_clear (req);
      return false;                      /* a job without sources, or the channel in use */
    }
  if (busy)
    {
      g_ptr_array_unref (sources);
      gbk_job_request_clear (req);
      job_fail (r, channel, G_IO_ERROR_BUSY, "Too many jobs at once");
      return true;
    }
  Job *j = g_new0 (Job, 1);
  g_atomic_ref_count_inc (&r->rc);
  j->r = r;
  j->channel = channel;
  j->req = *req;                         /* moved */
  *req = (GbkJobRequest) { 0 };
  j->sources = sources;
  g_mutex_init (&j->lock);
  g_cond_init (&j->cond);
  g_mutex_lock (&r->lock);
  g_hash_table_insert (r->running, &j->channel, j);
  g_mutex_unlock (&r->lock);
  g_autoptr (GError) error = nullptr;
  GThread *t = g_thread_try_new ("gbk-job", job_worker, j, &error);
  if (t == nullptr)
    {
      g_mutex_lock (&r->lock);
      g_hash_table_remove (r->running, &channel);
      g_mutex_unlock (&r->lock);
      job_free (j);
      job_fail (r, channel, G_IO_ERROR_FAILED, error->message);
      return true;
    }
  g_thread_unref (t);
  return true;
}

/* An answer or a cancel for a running job (one already finished: nothing to do). */
static void
steer_job (GbkJobRunner *r, uint64_t channel, const GbkChoice *choice, bool all)
{
  g_mutex_lock (&r->lock);
  Job *j = g_hash_table_lookup (r->running, &channel);
  if (j != nullptr)
    {
      g_mutex_lock (&j->lock);
      if (choice != nullptr)
        {
          j->answer = *choice;
          j->all = all;
          j->answered = true;
        }
      else
        g_atomic_int_set (&j->cancelled, 1);
      g_cond_broadcast (&j->cond);
      g_mutex_unlock (&j->lock);
    }
  g_mutex_unlock (&r->lock);
}

bool
gbk_job_runner_handle (GbkJobRunner *r, const GbkFrame *f)
{
  switch (f->type)
    {
    case GBK_MSG_SOURCES:
      {
        g_autoptr (GPtrArray) got = g_ptr_array_new_with_free_func (g_free);
        if (f->channel == 0 || !gbk_proto_get_sources (f, got))
          return false;
        g_mutex_lock (&r->lock);
        bool ok = r->pending_total + got->len <= JOB_SOURCES_MAX;
        GPtrArray *list = ok ? g_hash_table_lookup (r->pending, &f->channel) : nullptr;
        if (ok && list == nullptr)
          {
            list = g_ptr_array_new_with_free_func (g_free);
            g_hash_table_insert (r->pending, g_memdup2 (&f->channel, sizeof f->channel), list);
          }
        for (guint i = 0; ok && i < got->len; i++)
          g_ptr_array_add (list, g_steal_pointer (&got->pdata[i]));
        if (ok)
          r->pending_total += got->len;
        g_mutex_unlock (&r->lock);
        return ok;
      }
    case GBK_MSG_JOB:
      {
        GbkJobRequest req;
        if (!gbk_proto_get_job (f, &req))
          return false;
        return job_start (r, f->channel, &req);
      }
    case GBK_MSG_ANSWER:
      {
        GbkChoice ch;
        bool all;
        if (!gbk_proto_get_answer (f, &ch, &all))
          return false;
        steer_job (r, f->channel, &ch, all);
        return true;
      }
    case GBK_MSG_CANCEL:
      if (!gbk_proto_get_empty (f))
        return false;
      steer_job (r, f->channel, nullptr, false);
      return true;
    case GBK_MSG_HELLO:
    case GBK_MSG_SCAN:
    case GBK_MSG_INFO:
    case GBK_MSG_FAILED:
    case GBK_MSG_PONG:
    case GBK_MSG_LISTING:
    case GBK_MSG_STATED:
    case GBK_MSG_DATA:
    case GBK_MSG_END:
    case GBK_MSG_REMOVED:
    case GBK_MSG_QUESTION:
    case GBK_MSG_PROGRESS:
    case GBK_MSG_CREATED:
    case GBK_MSG_DONE:
    case GBK_MSG_WATCH:
    case GBK_MSG_UNWATCH:
    case GBK_MSG_GET_INFO:
    case GBK_MSG_PING:
    case GBK_MSG_LIST:
    case GBK_MSG_LSTAT:
    case GBK_MSG_READ:
    case GBK_MSG_CREDIT:
    case GBK_MSG_REMOVE:
    case GBK_MSG_SESSION:
    case GBK_MSG_FORGET:
    default: return false;
    }
}

void
gbk_job_runner_stop (GbkJobRunner *r, int ms)
{
  int64_t deadline = g_get_monotonic_time () + (int64_t) ms * 1000;
  g_mutex_lock (&r->lock);
  r->stopped = true;
  GHashTableIter it;
  void *j;
  g_hash_table_iter_init (&it, r->running);
  while (g_hash_table_iter_next (&it, nullptr, &j))
    {
      Job *job = j;
      g_mutex_lock (&job->lock);
      g_atomic_int_set (&job->cancelled, 1);
      g_cond_broadcast (&job->cond);
      g_mutex_unlock (&job->lock);
    }
  while (g_hash_table_size (r->running) > 0)
    if (!g_cond_wait_until (&r->idle, &r->lock, deadline))
      break;
  g_mutex_unlock (&r->lock);
}
