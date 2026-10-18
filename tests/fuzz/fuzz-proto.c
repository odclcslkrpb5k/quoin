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
        case GBK_MSG_WATCH:
        case GBK_MSG_GET_INFO:
          {
            g_autofree char *path = gbk_proto_get_path (&f);
            g_assert (path == nullptr || path[0] == '/');
            break;
          }
        case GBK_MSG_UNWATCH:
        case GBK_MSG_PING:
        case GBK_MSG_PONG:
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
