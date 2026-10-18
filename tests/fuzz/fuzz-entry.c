/* Input: raw filename bytes (byte 0 also picks kind/mode). Entries are built from whatever
 * the kernel hands back, so any byte sequence must be handled. */
#include "entry.h"
#include "fuzz.h"

static GbkTypeTable *types;

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  if (types == nullptr)
    {
      const char globs[] = "50:text/plain:*.txt\n50:application/x-compressed-tar:*.tar.gz\n";
      GbkTypeBuilder *b = gbk_type_builder_new ();
      gbk_type_builder_load_globs2 (b, globs, strlen (globs));
      types = gbk_type_builder_finish (b);   /* lives for the process */
    }
  if (size == 0)
    return 0;

  GbkEntryInfo info = {
    .kind = (GbkFileKind) (data[0] % 7),
    .mode = data[0] & 0111u,
    .mtime_ns = (int64_t) size,
  };
  const char *name = (const char *) data + 1;
  size_t len = size - 1;
  g_autoptr (GbkEntry) e = gbk_entry_new (name, len, &info, types);

  bool valid = len >= 1 && len <= 255 && memchr (name, '/', len) == nullptr
               && memchr (name, '\0', len) == nullptr && !(len == 1 && name[0] == '.')
               && !(len == 2 && memcmp (name, "..", 2) == 0);
  g_assert ((e != nullptr) == valid);
  if (e == nullptr)
    return 0;

  g_assert (e->name_len == len && memcmp (gbk_entry_name (e), name, len) == 0);
  g_assert (gbk_entry_name (e)[len] == '\0');
  g_assert (g_utf8_validate (gbk_entry_display (e), -1, nullptr));
  g_assert (g_utf8_validate (gbk_entry_key (e), -1, nullptr));
  g_assert (strlen (gbk_entry_key (e)) == e->key_len);
  g_assert (e->type < gbk_type_table_size (types));
  return 0;
}
