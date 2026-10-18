/* Input: what a server (or a broken or hostile one) might send: an optional magic line,
 * then frames. Every decoder must refuse what it can't use; whatever is accepted is applied
 * to a folder model, whose invariants are checked after each message. */
#include "dirmodel.h"
#include "fuzz.h"
#include "proto.h"

static GbkTypeTable *types;

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  if (types == nullptr)
    types = gbk_type_builder_finish (gbk_type_builder_new ());   /* lives for the process */

  uint32_t version;
  g_autofree char *build = nullptr;
  size_t used = 0;
  if (gbk_proto_magic (data, size, &version, &build, &used) == 1)
    {
      g_assert (used <= size && build != nullptr);
      data += used;
      size -= used;
    }

  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  while (size > 0)
    {
      GbkFrame f;
      int r = gbk_proto_next_frame (data, size, &f, &used);
      if (r != 1)
        break;
      g_assert (used >= 13 && used <= size && f.len == used - 13);
      switch (f.type)
        {
        case GBK_MSG_SCAN:
          {
            GbkScanMsg *msg = gbk_proto_get_scan (&f, types);
            if (msg != nullptr)
              {
                gbk_dir_model_apply_for_test (m, msg);
                gbk_dir_model_check_order (m);
              }
            break;
          }
        case GBK_MSG_HELLO:
          {
            GbkHello h;
            if (gbk_proto_get_hello (&f, &h))
              {
                g_assert (h.home[0] == '/');
                gbk_hello_clear (&h);
              }
            break;
          }
        case GBK_MSG_INFO:
          {
            g_autoptr (GbkFileInfo) info = gbk_proto_get_info (&f);
            if (info != nullptr && info->owner != nullptr)
              g_assert (g_utf8_validate (info->owner, -1, nullptr));
            break;
          }
        case GBK_MSG_FAILED:
          {
            g_autoptr (GError) e = nullptr;
            if (gbk_proto_get_failed (&f, &e))
              g_assert (e != nullptr && g_utf8_validate (e->message, -1, nullptr));
            break;
          }
        case GBK_MSG_LISTING:
          {
            GPtrArray *entries;
            bool last;
            if (gbk_proto_get_listing (&f, &entries, &last))
              {
                for (guint i = 0; i < entries->len; i++)
                  {
                    const GbkStatEntry *e = g_ptr_array_index (entries, i);
                    g_assert (gbk_entry_name_valid (e->name, strlen (e->name)));
                    g_assert (S_ISLNK (e->mode) == (e->target != nullptr));
                  }
                g_ptr_array_unref (entries);
              }
            break;
          }
        case GBK_MSG_STATED:
          {
            g_autoptr (GbkStatEntry) e = gbk_proto_get_stated (&f);
            g_assert (e == nullptr || e->name == nullptr);
            break;
          }
        case GBK_MSG_DATA:
          {
            const uint8_t *d;
            size_t n;
            if (gbk_proto_get_data (&f, &d, &n))
              g_assert (n <= GBK_PROTO_DATA_MAX);
            break;
          }
        case GBK_MSG_END:
          {
            bool changed;
            GbkFileToken t;
            g_assert (!gbk_proto_get_end (&f, &changed, &t) || f.len == 49);
            break;
          }
        case GBK_MSG_READ:
          {
            g_autofree char *path = nullptr;
            uint64_t session;
            uint32_t credit;
            if (gbk_proto_get_read (&f, &session, &path, &credit))
              g_assert (path[0] == '/');
            break;
          }
        case GBK_MSG_LIST:
        case GBK_MSG_LSTAT:
        case GBK_MSG_REMOVE:
          {
            g_autofree char *path = nullptr;
            uint64_t session;
            if (gbk_proto_get_request (&f, &session, &path))
              g_assert (path[0] == '/');
            break;
          }
        case GBK_MSG_SESSION:
          {
            bool record;
            g_assert (!gbk_proto_get_session (&f, &record) || f.len == 1);
            break;
          }
        case GBK_MSG_SOURCES:
        case GBK_MSG_CREATED:
          {
            g_autoptr (GPtrArray) got = g_ptr_array_new_with_free_func (g_free);
            bool ok = f.type == GBK_MSG_SOURCES ? gbk_proto_get_sources (&f, got)
                                                : gbk_proto_get_created (&f, got);
            g_assert (ok == (got->len > 0));
            for (guint i = 0; i < got->len; i++)
              {
                const char *str = g_ptr_array_index (got, i);
                g_assert (f.type == GBK_MSG_SOURCES ? str[0] == '/'
                                                    : gbk_entry_name_valid (str, strlen (str)));
              }
            break;
          }
        case GBK_MSG_JOB:
          {
            GbkJobRequest job;
            if (gbk_proto_get_job (&f, &job))
              {
                g_assert ((job.dest != nullptr)
                          == (job.kind == GBK_OP_COPY || job.kind == GBK_OP_MOVE));
                g_assert (job.dest == nullptr || job.dest[0] == '/');
                gbk_job_request_clear (&job);
              }
            break;
          }
        case GBK_MSG_QUESTION:
          {
            GbkAsk q;
            if (gbk_proto_get_question (&f, &q))
              {
                g_assert (q.path[0] == '/');
                g_assert ((q.other != nullptr) == (q.kind == GBK_ASK_CONFLICT));
                g_assert ((q.error != nullptr) == (q.kind == GBK_ASK_ERROR));
                g_assert (q.error == nullptr || g_utf8_validate (q.error->message, -1, nullptr));
                gbk_ask_clear (&q);
              }
            break;
          }
        case GBK_MSG_ANSWER:
          {
            GbkChoice ch;
            bool all;
            if (gbk_proto_get_answer (&f, &ch, &all))
              g_assert (ch <= GBK_CHOICE_CANCEL);
            break;
          }
        case GBK_MSG_PROGRESS:
          {
            uint64_t d, t;
            g_autofree char *current = nullptr;
            if (gbk_proto_get_progress (&f, &d, &t, &current))
              g_assert (current != nullptr);
            break;
          }
        case GBK_MSG_CREDIT:
          {
            uint32_t bytes;
            g_assert (!gbk_proto_get_credit (&f, &bytes) || f.len == 4);
            break;
          }
        case GBK_MSG_UPLOADED:
          {
            GbkFileToken t;
            g_assert (!gbk_proto_get_uploaded (&f, &t) || f.len == 48);
            break;
          }
        case GBK_MSG_WATCH:
        case GBK_MSG_GET_INFO:
        case GBK_MSG_RESOLVE:
        case GBK_MSG_RESOLVED:
          {
            g_autofree char *path = gbk_proto_get_path (&f);
            g_assert (path == nullptr || path[0] == '/');
            break;
          }
        case GBK_MSG_UNWATCH:
        case GBK_MSG_PING:
        case GBK_MSG_PONG:
        case GBK_MSG_CANCEL:
        case GBK_MSG_REMOVED:
        case GBK_MSG_FORGET:
        case GBK_MSG_DONE:
        default:
          {
            bool empty = gbk_proto_get_empty (&f);
            g_assert (!empty || f.len == 0);
            break;
          }
        }
      data += used;
      size -= used;
    }
  return 0;
}
