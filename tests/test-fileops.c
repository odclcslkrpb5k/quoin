/* File operations on real temp directories, with scripted answers to every prompt. */
#include "fileops.h"

static char *root;

typedef struct
{
  GbkChoice answer;       /* for conflicts */
  bool apply_all;
  int conflicts;
  int errors;
  GbkChoice error_answer;
  bool delete_untrashable;
  int cancel_after_progress;   /* >0: report cancelled after this many progress calls */
  int progress_calls;
  GbkChoice upload_answer;
  int uploads_asked;
  bool asked_linked;           /* the last upload question was about hard links */
  const char *touch_when_asked; /* changed at the first upload question */
} Script;

static GbkChoice
on_conflict (const char *src G_GNUC_UNUSED, const char *dst G_GNUC_UNUSED,
             bool dst_is_dir G_GNUC_UNUSED, bool *apply_all, void *data)
{
  Script *s = data;
  s->conflicts++;
  *apply_all = s->apply_all;
  return s->answer;
}

static GbkChoice
on_error (const char *path G_GNUC_UNUSED, const GError *error, void *data)
{
  Script *s = data;
  s->errors++;
  g_test_message ("op error: %s", error->message);
  return s->error_answer;
}

static bool
on_untrashable (const char *path G_GNUC_UNUSED, void *data)
{
  return ((Script *) data)->delete_untrashable;
}

static void
on_progress (uint64_t done G_GNUC_UNUSED, uint64_t total G_GNUC_UNUSED,
             const char *current G_GNUC_UNUSED, void *data)
{
  ((Script *) data)->progress_calls++;
}

static GbkChoice
on_upload (const char *target G_GNUC_UNUSED, bool linked, void *data)
{
  Script *s = data;
  s->uploads_asked++;
  s->asked_linked = linked;
  if (s->uploads_asked == 1 && s->touch_when_asked != nullptr)
    {
      g_usleep (20000);
      g_assert_true (g_file_set_contents (s->touch_when_asked, "while asked", -1, nullptr));
    }
  return s->upload_answer;
}

static bool
is_cancelled (void *data)
{
  const Script *s = data;
  return s->cancel_after_progress > 0 && s->progress_calls >= s->cancel_after_progress;
}

static GbkOpsHooks
hooks (Script *s)
{
  return (GbkOpsHooks) { on_conflict,  on_error, on_untrashable, on_progress,
                         is_cancelled, s,        on_upload };
}

/* ---- fixture helpers ---- */

static char *
P (const char *rel)
{
  return g_build_filename (root, rel, nullptr);
}

static void
mk (const char *rel, const char *content)
{
  g_autofree char *p = P (rel);
  g_autofree char *dir = g_path_get_dirname (p);
  g_mkdir_with_parents (dir, 0755);
  if (g_str_has_suffix (rel, "/"))
    g_mkdir_with_parents (p, 0755);
  else
    g_assert_true (g_file_set_contents (p, content, -1, nullptr));
}

static char *
slurp (const char *rel)
{
  g_autofree char *p = P (rel);
  char *data = nullptr;
  if (!g_file_get_contents (p, &data, nullptr, nullptr))
    return g_strdup ("(missing)");
  return data;
}

static bool
exists (const char *rel)
{
  g_autofree char *p = P (rel);
  struct stat st;
  return lstat (p, &st) == 0;
}

static GPtrArray *
paths (const char *first, ...)
{
  GPtrArray *a = g_ptr_array_new_with_free_func (g_free);
  va_list ap;
  va_start (ap, first);
  for (const char *r = first; r != nullptr; r = va_arg (ap, const char *))
    g_ptr_array_add (a, P (r));
  va_end (ap);
  return a;
}

static bool
run (GbkOpKind kind, GPtrArray *src, const char *dest_rel, Script *s, GPtrArray *created)
{
  g_autofree char *dest = dest_rel != nullptr ? P (dest_rel) : nullptr;
  GbkOpsHooks h = hooks (s);
  g_autoptr (GError) error = nullptr;
  bool ok = gbk_ops_run (kind, src, dest, &h, created, &error);
  if (!ok)
    g_test_message ("run failed: %s", error->message);
  return ok;
}

#define assert_content(rel, expected) \
  do \
    { \
      g_autofree char *c_ = slurp (rel); \
      g_assert_cmpstr (c_, ==, expected); \
    } \
  while (0)

/* ---- tests ---- */

static void
test_copy_tree (void)
{
  mk ("src/a.txt", "A");
  mk ("src/dir/b.txt", "B");
  mk ("src/dir/deeper/c.txt", "C");
  mk ("dst/", "");
  g_autofree char *link = P ("src/link");
  g_assert_cmpint (symlink ("a.txt", link), ==, 0);
  g_autofree char *a = P ("src/a.txt");
  chmod (a, 0751);

  Script s = { .answer = GBK_CHOICE_CANCEL };
  g_autoptr (GPtrArray) created = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GPtrArray) src = paths ("src/a.txt", "src/dir", "src/link", (char *) nullptr);
  g_assert_true (run (GBK_OP_COPY, src, "dst", &s, created));
  g_assert_cmpint (s.conflicts, ==, 0);
  g_assert_cmpint (s.errors, ==, 0);
  assert_content ("dst/a.txt", "A");
  assert_content ("dst/dir/deeper/c.txt", "C");
  g_assert_cmpuint (created->len, ==, 3);

  struct stat st;
  g_autofree char *da = P ("dst/a.txt");
  g_assert_cmpint (stat (da, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 07777, ==, 0751);          /* mode preserved */
  g_autofree char *dl = P ("dst/link");
  g_autofree char *target = g_file_read_link (dl, nullptr);
  g_assert_cmpstr (target, ==, "a.txt");                   /* symlink copied as a link */
  g_assert_true (s.progress_calls > 0);
}

static void
test_copy_conflicts (void)
{
  mk ("src/f.txt", "new");
  mk ("dst/f.txt", "old");
  g_autoptr (GPtrArray) src = paths ("src/f.txt", (char *) nullptr);

  Script skip = { .answer = GBK_CHOICE_SKIP };
  g_assert_true (run (GBK_OP_COPY, src, "dst", &skip, nullptr));
  g_assert_cmpint (skip.conflicts, ==, 1);
  assert_content ("dst/f.txt", "old");

  Script both = { .answer = GBK_CHOICE_KEEP_BOTH };
  g_assert_true (run (GBK_OP_COPY, src, "dst", &both, nullptr));
  assert_content ("dst/f.txt", "old");
  assert_content ("dst/f (1).txt", "new");

  Script replace = { .answer = GBK_CHOICE_REPLACE };
  g_assert_true (run (GBK_OP_COPY, src, "dst", &replace, nullptr));
  assert_content ("dst/f.txt", "new");

  Script cancel = { .answer = GBK_CHOICE_CANCEL };
  mk ("src/f.txt", "newer");
  g_assert_false (run (GBK_OP_COPY, src, "dst", &cancel, nullptr));
  assert_content ("dst/f.txt", "new");

  /* No temp files left behind by any of it. */
  g_autofree char *dst = P ("dst");
  g_autoptr (GDir) d = g_dir_open (dst, 0, nullptr);
  const char *name;
  while ((name = g_dir_read_name (d)) != nullptr)
    g_assert_false (g_str_has_prefix (name, ".gbk-tmp-"));
}

static void
test_copy_merge_and_apply_all (void)
{
  mk ("src/d/one", "1");
  mk ("src/d/two", "2");
  mk ("dst/d/one", "old1");
  mk ("dst/d/two", "old2");
  mk ("dst/d/keep", "k");
  g_autoptr (GPtrArray) src = paths ("src/d", (char *) nullptr);
  Script s = { .answer = GBK_CHOICE_REPLACE, .apply_all = true };
  g_assert_true (run (GBK_OP_COPY, src, "dst", &s, nullptr));
  g_assert_cmpint (s.conflicts, ==, 1);    /* asked once (the folder), applied to all */
  assert_content ("dst/d/one", "1");
  assert_content ("dst/d/two", "2");
  assert_content ("dst/d/keep", "k");      /* merge, not wipe */
}

static void
test_copy_onto_itself_and_into_itself (void)
{
  mk ("here/x.txt", "X");
  mk ("here/sub/", "");
  g_autoptr (GPtrArray) file = paths ("here/x.txt", (char *) nullptr);
  Script s = { .answer = GBK_CHOICE_CANCEL };
  g_assert_true (run (GBK_OP_COPY, file, "here", &s, nullptr));   /* paste in same folder */
  g_assert_cmpint (s.conflicts, ==, 0);
  assert_content ("here/x (1).txt", "X");

  g_autoptr (GPtrArray) dir = paths ("here", (char *) nullptr);
  g_assert_false (run (GBK_OP_COPY, dir, "here/sub", &s, nullptr));
  g_assert_false (run (GBK_OP_MOVE, dir, "here/sub", &s, nullptr));
  g_assert_false (run (GBK_OP_COPY, dir, "here", &s, nullptr));
  g_assert_true (exists ("here/x.txt"));
}

static void
test_move (void)
{
  mk ("src/m.txt", "M");
  mk ("src/md/inner", "I");
  mk ("dst/", "");
  g_autoptr (GPtrArray) src = paths ("src/m.txt", "src/md", (char *) nullptr);
  Script s = { .answer = GBK_CHOICE_CANCEL };
  g_autoptr (GPtrArray) created = g_ptr_array_new_with_free_func (g_free);
  g_assert_true (run (GBK_OP_MOVE, src, "dst", &s, created));
  g_assert_false (exists ("src/m.txt"));
  g_assert_false (exists ("src/md"));
  assert_content ("dst/m.txt", "M");
  assert_content ("dst/md/inner", "I");
  g_assert_cmpuint (created->len, ==, 2);

  /* Conflict: keep both. */
  mk ("src/m.txt", "M2");
  Script both = { .answer = GBK_CHOICE_KEEP_BOTH };
  g_autoptr (GPtrArray) again = paths ("src/m.txt", (char *) nullptr);
  g_assert_true (run (GBK_OP_MOVE, again, "dst", &both, nullptr));
  assert_content ("dst/m.txt", "M");
  assert_content ("dst/m (1).txt", "M2");
}

/* Across filesystems a move is copy, flush, delete. The copy must reach the disk before the
 * source goes; if it can't, the source is kept. */
static void
test_move_across_filesystems (void)
{
  mk ("src/f.txt", "F");
  mk ("src/d/inner", "I");
  mk ("src/r.txt", "new");
  mk ("dst/r.txt", "old");
  gbk_ops_force_exdev_for_test (true);
  int before = gbk_ops_flushes ();
  g_autoptr (GPtrArray) src = paths ("src/f.txt", "src/d", "src/r.txt", (char *) nullptr);
  Script s = { .answer = GBK_CHOICE_REPLACE };
  g_assert_true (run (GBK_OP_MOVE, src, "dst", &s, nullptr));
  g_assert_cmpint (gbk_ops_flushes () - before, ==, 3);   /* one per item, before its delete */
  g_assert_cmpint (s.errors, ==, 0);
  g_assert_false (exists ("src/f.txt"));
  g_assert_false (exists ("src/d"));
  g_assert_false (exists ("src/r.txt"));
  assert_content ("dst/f.txt", "F");
  assert_content ("dst/d/inner", "I");
  assert_content ("dst/r.txt", "new");

  /* The flush fails: the copy stays, and so does the original. */
  mk ("src/g.txt", "G");
  mk ("src/h.txt", "H");
  mk ("dst/h.txt", "old");
  gbk_ops_fail_flush_for_test (EIO);
  g_autoptr (GPtrArray) again = paths ("src/g.txt", "src/h.txt", (char *) nullptr);
  Script skip = { .answer = GBK_CHOICE_REPLACE, .error_answer = GBK_CHOICE_SKIP };
  g_assert_true (run (GBK_OP_MOVE, again, "dst", &skip, nullptr));
  gbk_ops_fail_flush_for_test (0);
  gbk_ops_force_exdev_for_test (false);
  g_assert_cmpint (skip.errors, ==, 2);
  assert_content ("src/g.txt", "G");
  assert_content ("dst/g.txt", "G");
  assert_content ("src/h.txt", "H");
  assert_content ("dst/h.txt", "H");
}

static void
test_delete_does_not_follow_symlinks (void)
{
  mk ("victim/precious.txt", "keep me");
  mk ("trash-me/a/b", "x");
  g_autofree char *link = P ("trash-me/a/link-to-victim");
  g_autofree char *victim = P ("victim");
  g_assert_cmpint (symlink (victim, link), ==, 0);

  g_autoptr (GPtrArray) src = paths ("trash-me", (char *) nullptr);
  Script s = { 0 };
  g_assert_true (run (GBK_OP_DELETE, src, nullptr, &s, nullptr));
  g_assert_false (exists ("trash-me"));
  assert_content ("victim/precious.txt", "keep me");
}

static void
test_trash (void)
{
  mk ("t/gone.txt", "bye");
  g_autoptr (GPtrArray) src = paths ("t/gone.txt", (char *) nullptr);
  Script s = { .error_answer = GBK_CHOICE_SKIP };
  g_assert_true (run (GBK_OP_TRASH, src, nullptr, &s, nullptr));
  g_assert_false (exists ("t/gone.txt"));
  g_assert_cmpint (s.errors, ==, 0);
  g_autofree char *trashed
      = g_build_filename (g_get_user_data_dir (), "Trash", "files", "gone.txt", nullptr);
  g_assert_true (g_file_test (trashed, G_FILE_TEST_EXISTS));
}

static void
test_errors_and_cancel (void)
{
  mk ("src/ok1", "1");
  mk ("src/ok2", "2");
  mk ("dst/", "");
  g_autoptr (GPtrArray) src = paths ("src/missing", "src/ok1", "src/ok2", (char *) nullptr);

  Script skip = { .error_answer = GBK_CHOICE_SKIP };
  g_assert_true (run (GBK_OP_COPY, src, "dst", &skip, nullptr));
  g_assert_cmpint (skip.errors, ==, 1);
  g_assert_true (exists ("dst/ok1"));

  Script stop = { .error_answer = GBK_CHOICE_CANCEL };
  g_autoptr (GPtrArray) src2 = paths ("src/missing", "src/ok1", (char *) nullptr);
  mk ("dst2/", "");
  g_assert_false (run (GBK_OP_COPY, src2, "dst2", &stop, nullptr));
  g_assert_false (exists ("dst2/ok1"));    /* stopped before it */

  /* Cancel mid-copy of a big file: no partial file at the destination. */
  g_autofree char *big = P ("src/big");
  {
    g_autofree char *chunk = g_malloc0 (1 << 20);
    FILE *f = fopen (big, "wb");
    g_assert_nonnull (f);
    for (int i = 0; i < 64; i++)
      fwrite (chunk, 1, 1 << 20, f);
    fclose (f);
  }
  mk ("dst3/", "");
  g_autoptr (GPtrArray) src3 = paths ("src/big", (char *) nullptr);
  Script cancel = { .cancel_after_progress = 3 };
  g_assert_false (run (GBK_OP_COPY, src3, "dst3", &cancel, nullptr));
  g_assert_false (exists ("dst3/big"));
  g_autofree char *dst3 = P ("dst3");
  g_autoptr (GDir) d = g_dir_open (dst3, 0, nullptr);
  g_assert_null (g_dir_read_name (d));      /* temp file cleaned up too */
}

static void
test_rename (void)
{
  mk ("r/a.txt", "A");
  mk ("r/b.txt", "B");
  g_autofree char *a = P ("r/a.txt");
  g_autoptr (GError) e1 = nullptr;
  g_assert_false (gbk_ops_rename (a, "b.txt", &e1));       /* never replaces */
  g_assert_error (e1, G_IO_ERROR, G_IO_ERROR_EXISTS);
  assert_content ("r/b.txt", "B");

  g_autoptr (GError) e2 = nullptr;
  g_assert_false (gbk_ops_rename (a, "x/y", &e2));
  g_assert_false (gbk_ops_rename (a, "..", nullptr));
  g_assert_false (gbk_ops_rename (a, "", nullptr));
  g_assert_true (gbk_ops_rename (a, "c.txt", nullptr));
  assert_content ("r/c.txt", "A");
  g_assert_false (exists ("r/a.txt"));
}

static void
test_free_name (void)
{
  mk ("n/x.tar.gz", "");
  mk ("n/x (1).tar.gz", "");
  mk ("n/.bashrc", "");
  g_autofree char *dir = P ("n");
  g_autofree char *n1 = gbk_ops_free_name (dir, "x.tar.gz");
  g_assert_cmpstr (n1, ==, "x (2).tar.gz");
  g_autofree char *n2 = gbk_ops_free_name (dir, ".bashrc");
  g_assert_cmpstr (n2, ==, ".bashrc (1)");
  g_autofree char *n3 = gbk_ops_free_name (dir, "plain");
  g_assert_cmpstr (n3, ==, "plain (1)");
}

/* Copies keep what cp -a keeps: mode, times (files, folders, links), xattrs, hard links
 * within the copy, special files. */
static void
test_copy_keeps_metadata (void)
{
  mk ("m/src/f.txt", "data");
  mk ("m/src/sub/", nullptr);
  mk ("m/dst/", nullptr);
  g_autofree char *f = P ("m/src/f.txt");
  g_autofree char *sub = P ("m/src/sub");
  g_autofree char *srcdir = P ("m/src");
  g_assert_cmpint (chmod (f, 0751), ==, 0);
  g_assert_cmpint (chmod (sub, 02750), ==, 0);
  g_autofree char *hard = P ("m/src/hard.txt");
  g_assert_cmpint (link (f, hard), ==, 0);
  g_autofree char *lnk = P ("m/src/link");
  g_assert_cmpint (symlink ("f.txt", lnk), ==, 0);
  g_autofree char *fifo = P ("m/src/fifo");
  g_assert_cmpint (mkfifo (fifo, 0640), ==, 0);
  bool have_xattr
      = setxattr (f, "user.gbk", "v1", 2, 0) == 0 && setxattr (sub, "user.gbk", "v2", 2, 0) == 0;
  if (!have_xattr)
    g_test_message ("filesystem without user xattrs: not checking them");

  const struct timespec old[2]
      = { { .tv_sec = 1000000000 }, { .tv_sec = 1100000000, .tv_nsec = 123 } };
  g_assert_cmpint (utimensat (AT_FDCWD, f, old, 0), ==, 0);
  g_assert_cmpint (utimensat (AT_FDCWD, lnk, old, AT_SYMLINK_NOFOLLOW), ==, 0);
  g_assert_cmpint (utimensat (AT_FDCWD, sub, old, 0), ==, 0);
  g_assert_cmpint (utimensat (AT_FDCWD, srcdir, old, 0), ==, 0);   /* last: the rest touch it */

  Script s = { 0 };
  g_autoptr (GPtrArray) src = paths ("m/src", nullptr);
  g_assert_true (run (GBK_OP_COPY, src, "m/dst", &s, nullptr));
  g_assert_cmpint (s.errors, ==, 0);

  struct stat a, b;
  g_autofree char *cf = P ("m/dst/src/f.txt");
  g_assert_cmpint (lstat (cf, &a), ==, 0);
  g_assert_cmpint (a.st_mode & 07777, ==, 0751);
  g_assert_cmpint (a.st_mtim.tv_sec, ==, 1100000000);
  g_assert_cmpint (a.st_mtim.tv_nsec, ==, 123);
  g_assert_cmpint (a.st_atim.tv_sec, ==, 1000000000);
  g_assert_cmpuint (a.st_uid, ==, geteuid ());
  /* The hard link is still one: same inode as the copied f.txt, not a second copy. */
  g_autofree char *ch = P ("m/dst/src/hard.txt");
  g_assert_cmpint (lstat (ch, &b), ==, 0);
  g_assert_true (a.st_ino == b.st_ino && a.st_dev == b.st_dev);
  g_assert_cmpuint (b.st_nlink, ==, 2);
  /* Folders: mode (setgid included) and times, set after their contents. */
  g_autofree char *csub = P ("m/dst/src/sub");
  g_assert_cmpint (lstat (csub, &a), ==, 0);
  g_assert_cmpint (a.st_mode & 07777, ==, 02750);
  g_assert_cmpint (a.st_mtim.tv_sec, ==, 1100000000);
  g_autofree char *cdir = P ("m/dst/src");
  g_assert_cmpint (lstat (cdir, &a), ==, 0);
  g_assert_cmpint (a.st_mtim.tv_sec, ==, 1100000000);
  /* The symlink: a link, with its own times. */
  g_autofree char *clnk = P ("m/dst/src/link");
  g_assert_cmpint (lstat (clnk, &a), ==, 0);
  g_assert_true (S_ISLNK (a.st_mode));
  g_assert_cmpint (a.st_mtim.tv_sec, ==, 1100000000);
  /* The FIFO is a FIFO (opening it to read would block forever). */
  g_autofree char *cfifo = P ("m/dst/src/fifo");
  g_assert_cmpint (lstat (cfifo, &a), ==, 0);
  g_assert_true (S_ISFIFO (a.st_mode));
  g_assert_cmpint (a.st_mode & 0777, ==, 0640);
  if (have_xattr)
    {
      char v[8];
      g_assert_cmpint (getxattr (cf, "user.gbk", v, sizeof v), ==, 2);
      g_assert_cmpmem (v, 2, "v1", 2);
      g_assert_cmpint (getxattr (csub, "user.gbk", v, sizeof v), ==, 2);
      g_assert_cmpmem (v, 2, "v2", 2);
    }
}

static void
test_numbered_name (void)
{
  const char *cases[][2] = {
    { "a.txt", "a (3).txt" },
    { "x.tar.gz", "x (3).tar.gz" },
    { ".bashrc", ".bashrc (3)" },
    { "New Folder", "New Folder (3)" },
    { "", " (3)" },
    { "a.", "a (3)." },
  };
  for (size_t i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autofree char *got = gbk_ops_numbered_name (cases[i][0], 3);
      g_assert_cmpstr (got, ==, cases[i][1]);
    }
}

static mode_t
mode_of (const char *rel)
{
  g_autofree char *p = P (rel);
  struct stat st;
  g_assert_cmpint (lstat (p, &st), ==, 0);
  return st.st_mode;
}

static void
test_create (void)
{
  mode_t mask = umask (022);
  umask (mask);
  mk ("c/", nullptr);
  g_autofree char *dir = P ("c");
  g_assert_true (gbk_ops_create (dir, "New Folder", true, nullptr));
  g_assert_true (S_ISDIR (mode_of ("c/New Folder")));
  g_assert_cmpint (mode_of ("c/New Folder") & 0777, ==, 0777 & ~mask);
  g_assert_true (gbk_ops_create (dir, "New File", false, nullptr));
  g_assert_true (S_ISREG (mode_of ("c/New File")));
  g_assert_cmpint (mode_of ("c/New File") & 0777, ==, 0666 & ~mask);
  assert_content ("c/New File", "");

  /* Never replaces anything: file, folder or symlink. */
  mk ("c/keep.txt", "precious");
  g_autoptr (GError) e1 = nullptr;
  g_assert_false (gbk_ops_create (dir, "keep.txt", false, &e1));
  g_assert_error (e1, G_IO_ERROR, G_IO_ERROR_EXISTS);
  assert_content ("c/keep.txt", "precious");
  g_autoptr (GError) e2 = nullptr;
  g_assert_false (gbk_ops_create (dir, "New Folder", true, &e2));
  g_assert_error (e2, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_autofree char *link = P ("c/link");
  g_assert_cmpint (symlink ("elsewhere", link), ==, 0);
  g_autoptr (GError) e3 = nullptr;
  g_assert_false (gbk_ops_create (dir, "link", false, &e3));
  g_assert_false (exists ("c/elsewhere"));        /* didn't create through the link */

  g_autoptr (GError) e4 = nullptr;
  g_assert_false (gbk_ops_create (dir, "a/b", false, &e4));
  g_assert_error (e4, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME);
}

static void
test_permissions (void)
{
  mk ("p/s.sh", "#!/bin/sh\n");
  g_autofree char *f = P ("p/s.sh");
  g_assert_cmpint (chmod (f, 0640), ==, 0);
  /* On: execute wherever there's read. Off: no execute anywhere. */
  g_assert_true (gbk_ops_set_executable (f, true, nullptr));
  g_assert_cmpint (mode_of ("p/s.sh") & 07777, ==, 0750);
  g_assert_true (gbk_ops_set_executable (f, true, nullptr));      /* already: no change */
  g_assert_cmpint (mode_of ("p/s.sh") & 07777, ==, 0750);
  g_assert_cmpint (chmod (f, 0711), ==, 0);
  g_assert_true (gbk_ops_set_executable (f, false, nullptr));
  g_assert_cmpint (mode_of ("p/s.sh") & 07777, ==, 0600);

  /* Through a symlink: the target changes. Folders aren't "executable" files. */
  g_autofree char *link = P ("p/link");
  g_assert_cmpint (symlink ("s.sh", link), ==, 0);
  g_assert_true (gbk_ops_set_executable (link, true, nullptr));
  g_assert_cmpint (mode_of ("p/s.sh") & 0777, ==, 0700);
  mk ("p/d/", nullptr);
  g_autofree char *d = P ("p/d");
  g_autoptr (GError) e = nullptr;
  g_assert_false (gbk_ops_set_executable (d, true, &e));
  g_assert_error (e, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE);

  /* rwx bits only: setgid survives. */
  g_assert_cmpint (chmod (d, 02755), ==, 0);
  g_assert_true (gbk_ops_set_permissions (d, 0700, nullptr));
  g_assert_cmpint (mode_of ("p/d") & 07777, ==, 02700);
  g_autofree char *gone = P ("p/gone");
  g_autoptr (GError) e2 = nullptr;
  g_assert_false (gbk_ops_set_permissions (gone, 0644, &e2));
  g_assert_error (e2, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void
test_info (void)
{
  mk ("i/f.txt", "hello");
  g_autofree char *f = P ("i/f.txt");
  g_assert_cmpint (chmod (f, 0640), ==, 0);
  const struct timespec ts[2]
      = { { .tv_sec = 1000000000, .tv_nsec = 5 }, { .tv_sec = 1500000000, .tv_nsec = 7 } };
  g_assert_cmpint (utimensat (AT_FDCWD, f, ts, 0), ==, 0);
  g_autoptr (GbkFileInfo) i = gbk_ops_info (f, nullptr);
  g_assert_nonnull (i);
  g_assert_false (i->symlink);
  g_assert_true (S_ISREG (i->mode));
  g_assert_cmpint (i->mode & 0777, ==, 0640);
  g_assert_cmpuint (i->size, ==, 5);
  g_assert_cmpint (i->atime_ns, ==, INT64_C (1000000000000000005));
  g_assert_cmpint (i->mtime_ns, ==, INT64_C (1500000000000000007));
  g_assert_cmpuint (i->uid, ==, geteuid ());
  g_assert_cmpuint (i->gid, ==, getegid ());

  /* A symlink: its target, plus where it points. */
  g_autofree char *link = P ("i/link");
  g_assert_cmpint (symlink ("f.txt", link), ==, 0);
  g_autoptr (GbkFileInfo) l = gbk_ops_info (link, nullptr);
  g_assert_true (l->symlink);
  g_assert_false (l->dangling);
  g_assert_cmpstr (l->link_target, ==, "f.txt");
  g_assert_true (S_ISREG (l->mode));
  g_assert_cmpuint (l->size, ==, 5);

  /* Dangling: the link itself. */
  g_autofree char *bad = P ("i/bad");
  g_assert_cmpint (symlink ("nowhere", bad), ==, 0);
  g_autoptr (GbkFileInfo) b = gbk_ops_info (bad, nullptr);
  g_assert_true (b->dangling);
  g_assert_true (S_ISLNK (b->mode));
  g_assert_cmpstr (b->link_target, ==, "nowhere");

  g_autofree char *gone = P ("i/gone");
  g_autoptr (GError) e = nullptr;
  g_assert_null (gbk_ops_info (gone, &e));
  g_assert_error (e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

/* Upload `edit_rel` over `target_rel` given what it was (`base`); the name written, or "". */
[[nodiscard]] static char *
upload (const char *edit_rel, const char *target_rel, const GbkFileToken *base, Script *s,
        GbkFileToken *token, GError **error)
{
  g_autofree char *edit = P (edit_rel);
  g_autofree char *target = P (target_rel);
  g_autoptr (GbkSource) here = gbk_source_local ();
  GbkOpsHooks h = hooks (s);
  g_autofree char *written = nullptr;
  if (!gbk_ops_upload (here, edit, target, base, &h, &written, token, error))
    return nullptr;
  return written != nullptr ? g_steal_pointer (&written) : g_strdup ("");
}

static GbkFileToken
token_of (const char *rel)
{
  g_autofree char *p = P (rel);
  struct stat st;
  g_assert_cmpint (lstat (p, &st), ==, 0);
  return gbk_file_token_of (&st);
}

static bool
temp_left (const char *dir_rel)
{
  g_autofree char *dir = P (dir_rel);
  g_autoptr (GDir) d = g_dir_open (dir, 0, nullptr);
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    if (g_str_has_prefix (name, ".gbk-tmp-"))
      return true;
  return false;
}

/* An edited file goes back over the one it came from only if that is still as it was;
 * otherwise (or with other hard links) it asks, and every answer keeps both versions safe. */
static void
test_upload (void)
{
  mk ("up/t.txt", "old");
  mk ("edit/t.txt", "new");
  g_autofree char *target = P ("up/t.txt");
  g_assert_cmpint (chmod (target, 0640), ==, 0);
  GbkFileToken base = token_of ("up/t.txt");
  Script s = { .upload_answer = GBK_CHOICE_SKIP };
  GbkFileToken token;
  g_autoptr (GError) error = nullptr;

  /* Unchanged there: replaced, keeping its permissions; the new state comes back. */
  g_autofree char *w1 = upload ("edit/t.txt", "up/t.txt", &base, &s, &token, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (w1, ==, "t.txt");
  g_assert_cmpint (s.uploads_asked, ==, 0);
  g_autofree char *c1 = slurp ("up/t.txt");
  g_assert_cmpstr (c1, ==, "new");
  GbkFileToken now = token_of ("up/t.txt");
  g_assert_true (gbk_file_token_same (&token, &now));
  struct stat st;
  g_assert_cmpint (stat (target, &st), ==, 0);
  g_assert_cmpint (st.st_mode & 07777, ==, 0640);
  g_assert_false (temp_left ("up"));

  /* Changed there since: asked. Don't Upload leaves theirs; Keep Both puts ours beside it;
   * Replace replaces it. */
  base = token;
  g_usleep (20000);
  mk ("up/t.txt", "theirs");
  g_autofree char *w2 = upload ("edit/t.txt", "up/t.txt", &base, &s, &token, &error);
  g_assert_cmpstr (w2, ==, "");
  g_assert_cmpint (s.uploads_asked, ==, 1);
  g_assert_false (s.asked_linked);
  g_autofree char *c2 = slurp ("up/t.txt");
  g_assert_cmpstr (c2, ==, "theirs");
  g_assert_false (temp_left ("up"));
  s.upload_answer = GBK_CHOICE_KEEP_BOTH;
  g_autofree char *w3 = upload ("edit/t.txt", "up/t.txt", &base, &s, &token, &error);
  g_assert_cmpstr (w3, ==, "t (1).txt");
  g_autofree char *c3 = slurp ("up/t (1).txt");
  g_assert_cmpstr (c3, ==, "new");
  g_autofree char *c3b = slurp ("up/t.txt");
  g_assert_cmpstr (c3b, ==, "theirs");
  s.upload_answer = GBK_CHOICE_REPLACE;
  g_autofree char *w4 = upload ("edit/t.txt", "up/t.txt", &base, &s, &token, &error);
  g_assert_cmpstr (w4, ==, "t.txt");
  g_autofree char *c4 = slurp ("up/t.txt");
  g_assert_cmpstr (c4, ==, "new");

  /* Other hard links: asked even though unchanged; Replace separates them. */
  g_autofree char *other = P ("up/other-name");
  g_assert_cmpint (link (target, other), ==, 0);
  base = token_of ("up/t.txt");
  mk ("edit/t.txt", "newer");
  s.uploads_asked = 0;
  g_autofree char *w5 = upload ("edit/t.txt", "up/t.txt", &base, &s, &token, &error);
  g_assert_cmpstr (w5, ==, "t.txt");
  g_assert_cmpint (s.uploads_asked, ==, 1);
  g_assert_true (s.asked_linked);
  g_autofree char *c5 = slurp ("up/other-name");
  g_assert_cmpstr (c5, ==, "new");

  /* Changed again while the question was up: asked again, never overwritten unseen. */
  base = token_of ("up/t.txt");
  g_usleep (20000);
  mk ("up/t.txt", "changed before");
  s.uploads_asked = 0;
  s.touch_when_asked = target;
  g_autofree char *w6 = upload ("edit/t.txt", "up/t.txt", &base, &s, &token, &error);
  g_assert_cmpstr (w6, ==, "t.txt");
  g_assert_cmpint (s.uploads_asked, ==, 2);
  s.touch_when_asked = nullptr;

  /* Gone there: made again. No longer a file: refused. */
  g_assert_cmpint (unlink (target), ==, 0);
  g_autofree char *w7 = upload ("edit/t.txt", "up/t.txt", &base, &s, &token, &error);
  g_assert_cmpstr (w7, ==, "t.txt");
  g_autofree char *c7 = slurp ("up/t.txt");
  g_assert_cmpstr (c7, ==, "newer");
  g_assert_cmpint (unlink (target), ==, 0);
  mk ("up/t.txt/", nullptr);
  g_autofree char *w8 = upload ("edit/t.txt", "up/t.txt", &base, &s, &token, &error);
  g_assert_null (w8);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE);
  g_assert_false (temp_left ("up"));
}

static void
fresh_root (void)
{
  /* Each test gets its own directory under the session root. */
  static int n;
  g_autofree char *base = g_strdup_printf ("t%d", n++);
  g_autofree char *dir = g_build_filename (g_getenv ("GBK_TEST_ROOT"), base, nullptr);
  g_mkdir_with_parents (dir, 0755);
  g_free (root);
  root = g_steal_pointer (&dir);
}

#define ADD(path, fn) g_test_add_func (path, fn##_wrapped)
#define WRAP(fn) \
  static void fn##_wrapped (void) \
  { \
    fresh_root (); \
    fn (); \
  }
WRAP (test_copy_tree)
WRAP (test_copy_conflicts)
WRAP (test_copy_merge_and_apply_all)
WRAP (test_copy_onto_itself_and_into_itself)
WRAP (test_move)
WRAP (test_move_across_filesystems)
WRAP (test_delete_does_not_follow_symlinks)
WRAP (test_trash)
WRAP (test_errors_and_cancel)
WRAP (test_rename)
WRAP (test_free_name)
WRAP (test_copy_keeps_metadata)
WRAP (test_numbered_name)
WRAP (test_create)
WRAP (test_permissions)
WRAP (test_info)
WRAP (test_upload)

int
main (int argc, char **argv)
{
  /* Private data dir so trash lands somewhere we own (and on the same filesystem). */
  g_autofree char *session = g_dir_make_tmp ("gbk-ops-XXXXXX", nullptr);
  g_autofree char *data = g_build_filename (session, "data", nullptr);
  g_mkdir_with_parents (data, 0700);
  g_setenv ("XDG_DATA_HOME", data, true);
  g_setenv ("GBK_TEST_ROOT", session, true);

  g_test_init (&argc, &argv, nullptr);
  ADD ("/fileops/copy-keeps-metadata", test_copy_keeps_metadata);
  ADD ("/fileops/numbered-name", test_numbered_name);
  ADD ("/fileops/create", test_create);
  ADD ("/fileops/permissions", test_permissions);
  ADD ("/fileops/info", test_info);
  ADD ("/fileops/copy-tree", test_copy_tree);
  ADD ("/fileops/copy-conflicts", test_copy_conflicts);
  ADD ("/fileops/copy-merge-apply-all", test_copy_merge_and_apply_all);
  ADD ("/fileops/onto-and-into-itself", test_copy_onto_itself_and_into_itself);
  ADD ("/fileops/move", test_move);
  ADD ("/fileops/move-across-filesystems", test_move_across_filesystems);
  ADD ("/fileops/delete-no-follow", test_delete_does_not_follow_symlinks);
  ADD ("/fileops/trash", test_trash);
  ADD ("/fileops/errors-and-cancel", test_errors_and_cancel);
  ADD ("/fileops/rename", test_rename);
  ADD ("/fileops/free-name", test_free_name);
  ADD ("/fileops/upload", test_upload);
  int status = g_test_run ();
  g_free (root);
  return status;
}
