/* Tests: run the git program, with a fixed identity and nothing from the user's config. */
#pragma once

/* `git <args...>` in `dir` (nullptr-terminated). Fatal if it fails, unless `may_fail`. */
static void
testgit_run (bool may_fail, const char *dir, ...)
{
  g_autoptr (GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
  const char *const fixed[] = { "git", "-c", "user.name=Test", "-c", "user.email=test@example.com",
                                "-c", "init.defaultBranch=main", "-c", "commit.gpgsign=false",
                                "-c", "advice.detachedHead=false" };
  for (size_t i = 0; i < G_N_ELEMENTS (fixed); i++)
    g_ptr_array_add (argv, g_strdup (fixed[i]));
  va_list ap;
  va_start (ap, dir);
  const char *arg;
  while ((arg = va_arg (ap, const char *)) != nullptr)
    g_ptr_array_add (argv, g_strdup (arg));
  va_end (ap);
  g_ptr_array_add (argv, nullptr);
  int status;
  g_autoptr (GError) error = nullptr;
  g_autofree char *err = nullptr;
  if (!g_spawn_sync (dir, (char **) argv->pdata, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                     nullptr, &err, &status, &error))
    gbk_fatal ("git: %s", error->message);
  if (!may_fail && !g_spawn_check_wait_status (status, nullptr))
    gbk_fatal ("git %s failed: %s", (const char *) g_ptr_array_index (argv, G_N_ELEMENTS (fixed)),
               err);
}

#define GIT(dir, ...) testgit_run (false, dir, __VA_ARGS__, (const char *) nullptr)
#define GIT_MAY_FAIL(dir, ...) testgit_run (true, dir, __VA_ARGS__, (const char *) nullptr)
