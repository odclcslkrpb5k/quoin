/* Input: arbitrary bytes as `git status --porcelain=v2 -z --branch` output. */
#include "git.h"
#include "fuzz.h"

static void
check_folder (const GbkGitStatus *s, const char *rel)
{
  GbkGitState inherit;
  g_autoptr (GHashTable) t = gbk_git_folder_states (s, rel, &inherit);
  g_assert (inherit < GBK_GIT_N_STATES);
  GHashTableIter it;
  void *name, *state;
  g_hash_table_iter_init (&it, t);
  while (g_hash_table_iter_next (&it, &name, &state))
    {
      g_assert (((const char *) name)[0] != '\0');
      g_assert (strchr (name, '/') == nullptr);
      g_assert (GPOINTER_TO_UINT (state) < GBK_GIT_N_STATES);
      g_assert (gbk_git_state_in (t, name, GBK_GIT_NONE) > GBK_GIT_NONE);
    }
}

int
LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkGitStatus) s = gbk_git_parse ((const char *) data, size, &error);
  if (s == nullptr)
    {
      g_assert (error != nullptr);
      return 0;
    }
  g_autofree char *summary = gbk_git_summary (s);
  g_assert (g_utf8_validate (summary, -1, nullptr));
  check_folder (s, "");
  for (guint i = 0; i < s->paths->len && i < 8; i++)
    {
      const GbkGitPath *p = g_ptr_array_index (s->paths, i);
      g_assert (p->state > GBK_GIT_NONE && p->state < GBK_GIT_N_STATES);
      g_autofree char *rel = g_strdup (p->path);
      size_t n = strlen (rel);
      if (n > 0 && rel[n - 1] == '/')
        rel[n - 1] = '\0';
      g_assert (gbk_git_state_of (s, rel) < GBK_GIT_N_STATES);
      g_autofree char *dir = g_path_get_dirname (rel);
      check_folder (s, g_str_equal (dir, ".") ? "" : dir);
    }
  return 0;
}
