/* Git status: the parser and folder roll-up on fixed output, then real repositories made
 * with the git program (finding them, every state, the live watcher). */
#include "gitwatch.h"
#include "inotify-hub.h"
#include "testgit.h"

/* ---- fixed output ---- */

static GBytes *
records (const char *const *r)
{
  GByteArray *b = g_byte_array_new ();
  for (; *r != nullptr; r++)
    g_byte_array_append (b, (const uint8_t *) *r, (guint) strlen (*r) + 1);
  return g_byte_array_free_to_bytes (b);
}

static GbkGitStatus *
parse (const char *const *r)
{
  g_autoptr (GBytes) b = records (r);
  size_t len;
  const char *data = g_bytes_get_data (b, &len);
  g_autoptr (GError) error = nullptr;
  GbkGitStatus *s = gbk_git_parse (data, len, &error);
  g_assert_no_error (error);
  if (s == nullptr)
    gbk_fatal ("no status");
  return s;
}

static const char *const mixed[] = {
  "# branch.oid 1234567890abcdef1234567890abcdef12345678",
  "# branch.head main",
  "# branch.upstream origin/main",
  "# branch.ab +2 -1",
  "1 .M N... 100644 100644 100644 aaaa bbbb src/a.c",
  "1 M. N... 100644 100644 100644 aaaa bbbb src/b.c",
  "1 MM N... 100644 100644 100644 aaaa bbbb c.txt",
  "1 A. N... 000000 100644 100644 0000 bbbb new file.txt",
  "2 R. N... 100644 100644 100644 aaaa bbbb R100 docs/new name.md",
  "docs/old.md",
  "u UU N... 100644 100644 100644 100644 aaaa bbbb cccc deep/x/conflict.c",
  "? untracked/",
  "? loose.txt",
  "! build/",
  "! src/a.o",
  nullptr,
};

static GbkGitState
in (GHashTable *t, const char *name)
{
  return gbk_git_state_in (t, name, GBK_GIT_NONE);
}

static void
test_parse (void)
{
  g_autoptr (GbkGitStatus) s = parse (mixed);
  g_assert_cmpstr (s->head, ==, "main");
  g_assert_cmpstr (s->upstream, ==, "origin/main");
  g_assert_true (s->has_ab);
  g_assert_cmpuint (s->ahead, ==, 2);
  g_assert_cmpuint (s->behind, ==, 1);
  g_assert_cmpuint (s->n_conflict, ==, 1);
  g_assert_cmpuint (s->n_modified, ==, 2);
  g_assert_cmpuint (s->n_staged, ==, 4);
  g_assert_cmpuint (s->n_untracked, ==, 2);
  g_autofree char *summary = gbk_git_summary (s);
  g_assert_cmpstr (summary, ==, "main ↑2 ↓1 · 1 conflict · 2 modified · 4 staged · 2 untracked");

  GbkGitState inherit;
  g_autoptr (GHashTable) top = gbk_git_folder_states (s, "", &inherit);
  g_assert_cmpint (inherit, ==, GBK_GIT_NONE);
  g_assert_cmpint (in (top, "src"), ==, GBK_GIT_MODIFIED);     /* a.c beats b.c */
  g_assert_cmpint (in (top, "c.txt"), ==, GBK_GIT_MODIFIED);   /* staged and not */
  g_assert_cmpint (in (top, "new file.txt"), ==, GBK_GIT_ADDED);
  g_assert_cmpint (in (top, "docs"), ==, GBK_GIT_STAGED);      /* renamed: new + old */
  g_assert_cmpint (in (top, "deep"), ==, GBK_GIT_CONFLICT);
  g_assert_cmpint (in (top, "untracked"), ==, GBK_GIT_UNTRACKED);
  g_assert_cmpint (in (top, "loose.txt"), ==, GBK_GIT_UNTRACKED);
  g_assert_cmpint (in (top, "build"), ==, GBK_GIT_IGNORED);
  g_assert_cmpuint (g_hash_table_size (top), ==, 8);

  g_autoptr (GHashTable) src = gbk_git_folder_states (s, "src", &inherit);
  g_assert_cmpint (in (src, "a.c"), ==, GBK_GIT_MODIFIED);
  g_assert_cmpint (in (src, "b.c"), ==, GBK_GIT_STAGED);
  g_assert_cmpint (in (src, "a.o"), ==, GBK_GIT_IGNORED);
  g_assert_cmpint (inherit, ==, GBK_GIT_NONE);

  g_autoptr (GHashTable) under = gbk_git_folder_states (s, "untracked/sub", &inherit);
  g_assert_cmpuint (g_hash_table_size (under), ==, 0);
  g_assert_cmpint (inherit, ==, GBK_GIT_UNTRACKED);
  g_autoptr (GHashTable) build = gbk_git_folder_states (s, "build", &inherit);
  g_assert_cmpint (inherit, ==, GBK_GIT_IGNORED);
  g_autoptr (GHashTable) docs = gbk_git_folder_states (s, "docs", &inherit);
  g_assert_cmpint (in (docs, "new name.md"), ==, GBK_GIT_ADDED);

  g_assert_cmpint (gbk_git_state_of (s, ""), ==, GBK_GIT_CONFLICT);
  g_assert_cmpint (gbk_git_state_of (s, "docs"), ==, GBK_GIT_STAGED);
  g_assert_cmpint (gbk_git_state_of (s, "src/a.o"), ==, GBK_GIT_IGNORED);
  g_assert_cmpint (gbk_git_state_of (s, "README"), ==, GBK_GIT_NONE);
  g_assert_cmpint (gbk_git_state_of (s, "build/x/y"), ==, GBK_GIT_IGNORED);
  g_assert_cmpint (gbk_git_state_of (s, "deep/x"), ==, GBK_GIT_CONFLICT);
}

static void
test_summaries (void)
{
  const char *const initial[]
      = { "# branch.oid (initial)", "# branch.head main", "? README", nullptr };
  const char *const detached[] = { "# branch.oid abcdef1234567", "# branch.head (detached)",
                                   "1 .D N... 100644 100644 000000 aaaa bbbb gone.c", nullptr };
  const char *const clean[] = { "# branch.oid abcdef1234567", "# branch.head work", nullptr };
  const char *const odd[] = { "# branch.oid abcdef1234567", "# branch.head caf\xe9", nullptr };
  struct
  {
    const char *const *in;
    const char *summary;
  } cases[] = {
    { initial, "main (no commits yet) · 1 untracked" },
    { detached, "detached at abcdef1 · 1 modified" },
    { clean, "work · clean" },
    { odd, "caf\xef\xbf\xbd · clean" },
  };
  for (size_t i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr (GbkGitStatus) s = parse (cases[i].in);
      g_autofree char *summary = gbk_git_summary (s);
      g_assert_cmpstr (summary, ==, cases[i].summary);
    }
}

static void
test_bad_output (void)
{
  const char *const bad[] = {
    "? a",                                                    /* no NUL at the end */
    "1 .X N... 100644 100644 100644 aaaa bbbb a\0",           /* unknown code */
    "1 .M N... 100644 a\0",                                   /* too few fields */
    "? /etc/passwd\0",
    "? a/../b\0",
    "? ./a\0",
    "? a//b\0",
    "? a//\0",                                                  /* found by fuzzing */
    "1 .M N... 100644 100644 100644 aaaa bbbb dir/\0",       /* a changed folder? */
    "2 R. N... 100644 100644 100644 aaaa bbbb R100 new\0",   /* no old path */
    "x thing\0",
    "\0",
    "?x\0",
    "# branch.ab +1\0",
    "# branch.ab +a -1\0",
    "# branch.oid xyz\0",
  };
  for (size_t i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      size_t len = strlen (bad[i]);
      /* Each literal's own NUL ends its last record: include it when the case has one. */
      if (i > 0)
        len += 1;
      g_autoptr (GError) error = nullptr;
      g_autoptr (GbkGitStatus) s = gbk_git_parse (bad[i], len, &error);
      if (s != nullptr)
        gbk_fatal ("bad output %zu parsed", i);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    }
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkGitStatus) empty = gbk_git_parse ("", 0, &error);
  g_assert_no_error (error);
  g_assert_nonnull (empty);
}

static void
test_fs_rules (void)
{
  g_assert_true (gbk_git_fs_refused (0x6969, "/home/u/x"));         /* NFS */
  g_assert_true (gbk_git_fs_refused (0x65735546, "/home/u/x"));     /* FUSE */
  g_assert_true (gbk_git_fs_refused (0x4D44, "/home/u/x"));         /* FAT */
  g_assert_true (gbk_git_fs_refused (0xFF534D42, "/home/u/x"));     /* CIFS */
  g_assert_false (gbk_git_fs_refused (0xEF53, "/home/u/x"));        /* ext4 */
  g_assert_false (gbk_git_fs_refused (0x9123683E, "/home/u/x"));    /* btrfs */
  g_assert_true (gbk_git_fs_refused (0xEF53, "/media/u/stick/x"));
  g_assert_true (gbk_git_fs_refused (0xEF53, "/run/media/u/stick"));
  g_assert_false (gbk_git_fs_refused (0xEF53, "/mediathek/x"));
}

/* ---- real repositories ---- */

static char *tmp;

static void
write_file (const char *dir, const char *name, const char *text)
{
  g_autofree char *path = g_build_filename (dir, name, nullptr);
  g_autofree char *parent = g_path_get_dirname (path);
  g_assert_cmpint (g_mkdir_with_parents (parent, 0755), ==, 0);
  g_autoptr (GError) error = nullptr;
  if (!g_file_set_contents (path, text, -1, &error))
    gbk_fatal ("write: %s", error->message);
}

/* A repository with something in every state. */
static char *
make_repo (const char *name)
{
  char *top = g_build_filename (tmp, name, nullptr);
  g_assert_cmpint (g_mkdir_with_parents (top, 0755), ==, 0);
  GIT (top, "init", "-q");
  write_file (top, "conflict.txt", "base\n");
  write_file (top, "clean.txt", "clean\n");
  write_file (top, "modified.txt", "one\n");
  write_file (top, "staged.txt", "one\n");
  write_file (top, "both.txt", "one\n");
  write_file (top, "deep/a/b/file.txt", "one\n");
  write_file (top, ".gitignore", "*.log\nignored/\n");
  GIT (top, "add", "-A");
  GIT (top, "commit", "-q", "-m", "first");
  GIT (top, "checkout", "-q", "-b", "other");
  write_file (top, "conflict.txt", "theirs\n");
  GIT (top, "commit", "-q", "-am", "theirs");
  GIT (top, "checkout", "-q", "main");
  write_file (top, "conflict.txt", "ours\n");
  GIT (top, "commit", "-q", "-am", "ours");
  GIT_MAY_FAIL (top, "merge", "-q", "other");   /* conflicts */
  write_file (top, "modified.txt", "two\n");
  write_file (top, "staged.txt", "two\n");
  GIT (top, "add", "staged.txt");
  write_file (top, "both.txt", "two\n");
  GIT (top, "add", "both.txt");
  write_file (top, "both.txt", "three\n");
  write_file (top, "new.txt", "new\n");
  GIT (top, "add", "new.txt");
  write_file (top, "untracked.txt", "u\n");
  write_file (top, "fresh/inner.txt", "u\n");
  write_file (top, "debug.log", "x\n");
  write_file (top, "ignored/x.txt", "x\n");
  write_file (top, "deep/a/b/file.txt", "two\n");
  return top;
}

static void
test_real (void)
{
  g_autofree char *top = make_repo ("real");
  g_autofree char *found_top = nullptr, *gitdir = nullptr;
  g_autofree char *deeper = g_build_filename (top, "deep", "a", nullptr);
  g_assert_true (gbk_git_find (deeper, &found_top, &gitdir));
  g_assert_cmpstr (found_top, ==, top);
  g_autofree char *expect_gitdir = g_build_filename (top, ".git", nullptr);
  g_assert_cmpstr (gitdir, ==, expect_gitdir);

  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkGitStatus) s = gbk_git_status (top, nullptr, -1, GBK_GIT_TIMEOUT_MS, &error);
  g_assert_no_error (error);
  if (s == nullptr)
    gbk_fatal ("no status");
  GbkGitState inherit;
  g_autoptr (GHashTable) t = gbk_git_folder_states (s, "", &inherit);
  g_assert_cmpint (in (t, "conflict.txt"), ==, GBK_GIT_CONFLICT);
  g_assert_cmpint (in (t, "clean.txt"), ==, GBK_GIT_NONE);
  g_assert_cmpint (in (t, "modified.txt"), ==, GBK_GIT_MODIFIED);
  g_assert_cmpint (in (t, "staged.txt"), ==, GBK_GIT_STAGED);
  g_assert_cmpint (in (t, "both.txt"), ==, GBK_GIT_MODIFIED);
  g_assert_cmpint (in (t, "new.txt"), ==, GBK_GIT_ADDED);
  g_assert_cmpint (in (t, "untracked.txt"), ==, GBK_GIT_UNTRACKED);
  g_assert_cmpint (in (t, "fresh"), ==, GBK_GIT_UNTRACKED);
  g_assert_cmpint (in (t, "debug.log"), ==, GBK_GIT_IGNORED);
  g_assert_cmpint (in (t, "ignored"), ==, GBK_GIT_IGNORED);
  g_assert_cmpint (in (t, "deep"), ==, GBK_GIT_MODIFIED);
  g_assert_cmpint (in (t, ".git"), ==, GBK_GIT_NONE);
  g_autofree char *summary = gbk_git_summary (s);
  g_assert_true (g_str_has_prefix (summary, "main · 1 conflict · "));

  /* Inside folders, and just one path. */
  g_autoptr (GHashTable) fresh = gbk_git_folder_states (s, "fresh", &inherit);
  g_assert_cmpint (inherit, ==, GBK_GIT_UNTRACKED);
  g_autoptr (GbkGitStatus) one = gbk_git_status (top, "deep/a", -1, GBK_GIT_TIMEOUT_MS, &error);
  g_assert_no_error (error);
  if (one == nullptr)
    gbk_fatal ("no status");
  g_assert_cmpuint (one->paths->len, ==, 1);
  g_assert_cmpint (gbk_git_state_of (one, "deep/a"), ==, GBK_GIT_MODIFIED);

  /* The last commit to touch a file; none for an untracked one. */
  GbkGitCommit c;
  g_assert_true (gbk_git_last_commit (top, "clean.txt", GBK_GIT_INFO_TIMEOUT_MS, &c, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (c.subject, ==, "first");
  g_assert_cmpstr (c.author, ==, "Test");
  g_assert_cmpint (c.time, >, 0);
  g_assert_nonnull (c.hash);
  gbk_git_commit_clear (&c);
  g_assert_false (gbk_git_last_commit (top, "untracked.txt", GBK_GIT_INFO_TIMEOUT_MS, &c, &error));
  g_assert_no_error (error);
  g_assert_null (c.hash);
}

static void
test_find (void)
{
  g_autofree char *top = make_repo ("find");
  g_autofree char *found = nullptr, *gitdir = nullptr;
  /* Not a work tree: inside .git, or no repository at all. */
  g_autofree char *inside = g_build_filename (top, ".git", "refs", nullptr);
  g_assert_false (gbk_git_find (inside, &found, &gitdir));
  g_assert_null (found);
  g_autofree char *outside = g_build_filename (tmp, "plain", nullptr);
  g_assert_cmpint (g_mkdir_with_parents (outside, 0755), ==, 0);
  g_assert_false (gbk_git_find (outside, &found, &gitdir));
  g_assert_false (gbk_git_find ("relative/path", &found, &gitdir));

  /* A linked work tree: its .git is a file naming its git directory. */
  g_autofree char *wt = g_build_filename (tmp, "linked", nullptr);
  GIT (top, "worktree", "add", "-q", "--detach", wt, "HEAD");
  g_assert_true (gbk_git_find (wt, &found, &gitdir));
  g_assert_cmpstr (found, ==, wt);
  g_autofree char *expect = g_build_filename (top, ".git", "worktrees", "linked", nullptr);
  g_assert_cmpstr (gitdir, ==, expect);
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkGitStatus) s = gbk_git_status (wt, nullptr, -1, GBK_GIT_TIMEOUT_MS, &error);
  g_assert_no_error (error);
  g_assert_nonnull (s);
  g_free (found);
  g_free (gitdir);

  /* A broken .git file stops the search, as it stops git. */
  g_autofree char *broken = g_build_filename (tmp, "broken", "sub", nullptr);
  g_assert_cmpint (g_mkdir_with_parents (broken, 0755), ==, 0);
  g_autofree char *broken_top = g_build_filename (tmp, "broken", nullptr);
  write_file (broken_top, ".git", "not a gitdir line\n");
  g_assert_false (gbk_git_find (broken, &found, &gitdir));
}

static void
test_cancel_and_timeout (void)
{
  g_autofree char *top = make_repo ("cancel");
  int fd = eventfd (1, EFD_CLOEXEC | EFD_NONBLOCK);     /* already cancelled */
  g_assert_cmpint (fd, >=, 0);
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkGitStatus) s = gbk_git_status (top, nullptr, fd, GBK_GIT_TIMEOUT_MS, &error);
  g_assert_null (s);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  close (fd);
  g_clear_error (&error);
  g_autoptr (GbkGitStatus) late = gbk_git_status (top, nullptr, -1, 0, &error);
  g_assert_null (late);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
  g_clear_error (&error);
  /* Not a repository after all: git's own complaint. */
  g_autofree char *plain = g_build_filename (tmp, "not-a-repo", nullptr);
  g_assert_cmpint (g_mkdir_with_parents (plain, 0755), ==, 0);
  g_autoptr (GbkGitStatus) none = gbk_git_status (plain, nullptr, -1, GBK_GIT_TIMEOUT_MS, &error);
  g_assert_null (none);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
}

/* ---- the watcher ---- */

/* The next result from `w` within a few seconds. */
static GbkGitResult *
next_result (GbkGitWatch *w)
{
  int64_t deadline = g_get_monotonic_time () + INT64_C (10) * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < deadline)
    {
      GbkGitResult *r = gbk_git_watch_take (w);
      if (r != nullptr)
        return r;
      g_usleep (10000);
    }
  gbk_fatal ("no git result");
}

static void
wait_workers_gone (void)
{
  int64_t deadline = g_get_monotonic_time () + INT64_C (10) * G_USEC_PER_SEC;
  while (gbk_git_watch_live_workers () > 0 && g_get_monotonic_time () < deadline)
    g_usleep (10000);
  g_assert_cmpint (gbk_git_watch_live_workers (), ==, 0);
}

static void
test_watch (void)
{
  g_autofree char *top = make_repo ("watch");
  GbkGitWatch *w = gbk_git_watch_start (top);
  g_autoptr (GbkGitResult) first = next_result (w);
  g_assert_true (first->repo);
  g_assert_null (first->error);
  g_assert_cmpint (in (first->names, "untracked.txt"), ==, GBK_GIT_UNTRACKED);
  g_assert_true (g_str_has_prefix (first->summary, "main · "));

  /* Staging in another program: seen through the index changing, with no poke. */
  int before = gbk_git_watch_runs ();
  GIT (top, "add", "untracked.txt");
  g_autoptr (GbkGitResult) staged = next_result (w);
  g_assert_cmpint (in (staged->names, "untracked.txt"), ==, GBK_GIT_ADDED);
  g_assert_cmpint (gbk_git_watch_runs (), >, before);

  /* A change in the folder: seen when poked. */
  write_file (top, "clean.txt", "changed\n");
  gbk_git_watch_poke (w);
  g_autoptr (GbkGitResult) poked = next_result (w);
  g_assert_cmpint (in (poked->names, "clean.txt"), ==, GBK_GIT_MODIFIED);
  gbk_git_watch_stop (w);

  /* A folder inside: its own contents; outside any repository: nothing to show. */
  g_autofree char *deep = g_build_filename (top, "deep", "a", nullptr);
  GbkGitWatch *inner = gbk_git_watch_start (deep);
  g_autoptr (GbkGitResult) r = next_result (inner);
  g_assert_cmpint (in (r->names, "b"), ==, GBK_GIT_MODIFIED);
  gbk_git_watch_stop (inner);
  g_autofree char *plain = g_build_filename (tmp, "watch-plain", nullptr);
  g_assert_cmpint (g_mkdir_with_parents (plain, 0755), ==, 0);
  GbkGitWatch *none = gbk_git_watch_start (plain);
  g_autoptr (GbkGitResult) nothing = next_result (none);
  g_assert_false (nothing->repo);
  g_assert_cmpuint (g_hash_table_size (nothing->names), ==, 0);
  gbk_git_watch_stop (none);
  wait_workers_gone ();
}

/* Stopping while git runs: the worker kills it and leaves on its own. */
static void
test_stop_early (void)
{
  g_autofree char *top = make_repo ("stop");
  for (int i = 0; i < 20; i++)
    {
      GbkGitWatch *w = gbk_git_watch_start (top);
      if (i % 2 == 0)
        g_usleep (2000);
      gbk_git_watch_stop (w);
    }
  wait_workers_gone ();
}

/* No git program at all: nothing to show, not an error on every folder. */
static void
test_no_git (void)
{
  g_autofree char *top = make_repo ("no-git");
  g_autofree char *path = g_strdup (g_getenv ("PATH"));
  g_setenv ("PATH", "/nonexistent", true);
  GbkGitWatch *w = gbk_git_watch_start (top);
  g_autoptr (GbkGitResult) r = next_result (w);
  gbk_git_watch_stop (w);
  g_autofree char *clean = g_build_filename (top, "clean.txt", nullptr);
  GbkGitFileInfo info;
  gbk_git_file_info (clean, false, &info);
  g_setenv ("PATH", path, true);
  g_assert_false (r->repo);
  g_assert_false (info.known);
  g_assert_null (info.error);
  gbk_git_file_info_clear (&info);
  gbk_git_file_info (clean, false, &info);       /* and with git back: */
  g_assert_true (info.known);
  g_assert_cmpint (info.state, ==, GBK_GIT_NONE);
  g_assert_cmpstr (info.commit.subject, ==, "first");
  gbk_git_file_info_clear (&info);
  wait_workers_gone ();
}

static void
rm_rf (const char *path)
{
  g_autoptr (GDir) d = g_dir_open (path, 0, nullptr);
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    {
      g_autofree char *p = g_build_filename (path, name, nullptr);
      struct stat st;
      if (lstat (p, &st) == 0 && S_ISDIR (st.st_mode))
        rm_rf (p);
      else
        unlink (p);
    }
  rmdir (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_autoptr (GError) error = nullptr;
  tmp = g_dir_make_tmp ("gbk-git-XXXXXX", &error);
  g_assert_no_error (error);
  g_test_add_func ("/git/parse", test_parse);
  g_test_add_func ("/git/summaries", test_summaries);
  g_test_add_func ("/git/bad-output", test_bad_output);
  g_test_add_func ("/git/fs-rules", test_fs_rules);
  g_test_add_func ("/git/real", test_real);
  g_test_add_func ("/git/find", test_find);
  g_test_add_func ("/git/cancel-and-timeout", test_cancel_and_timeout);
  g_test_add_func ("/git/watch", test_watch);
  g_test_add_func ("/git/stop-early", test_stop_early);
  g_test_add_func ("/git/no-git", test_no_git);
  int status = g_test_run ();
  rm_rf (tmp);
  g_free (tmp);
  return status;
}
