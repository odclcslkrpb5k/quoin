/* Context menu contents (pure). */
#include "menu.h"

static GbkConfig *config;

static GbkItem *
item (const char *path, const char *mime, bool dir, bool exec, bool script)
{
  GbkItem *it = g_new0 (GbkItem, 1);
  it->loc = gbk_loc_new_local (path);
  it->display = g_strdup (path);
  it->mime = g_strdup (mime);
  it->is_dir = dir;
  it->regular = !dir;
  it->executable = exec;
  it->script = script;
  return it;
}

static char *
describe (GPtrArray *items, GbkMenuTarget target)
{
  static const char *const names[] = {
    [GBK_MENU_SEPARATOR] = "|",
    [GBK_MENU_OPEN] = "open",
    [GBK_MENU_OPEN_TAB] = "open-tab",
    [GBK_MENU_OPEN_WINDOW] = "open-window",
    [GBK_MENU_OPEN_WITH] = "with",
    [GBK_MENU_EXECUTE] = "exec",
    [GBK_MENU_EXECUTE_TERMINAL] = "exec-term",
    [GBK_MENU_TERMINAL_HERE] = "term-here",
    [GBK_MENU_ACTION] = "action",
    [GBK_MENU_SORT_NAME] = "sort-name",
    [GBK_MENU_SORT_MTIME] = "sort-mtime",
    [GBK_MENU_SORT_SIZE] = "sort-size",
    [GBK_MENU_SHOW_HIDDEN] = "hidden",
    [GBK_MENU_CUT] = "cut",
    [GBK_MENU_COPY] = "copy",
    [GBK_MENU_COPY_PATH] = "copy-path",
    [GBK_MENU_PASTE] = "paste",
    [GBK_MENU_PASTE_INTO] = "paste-into",
    [GBK_MENU_TRASH] = "trash",
    [GBK_MENU_DELETE] = "delete",
    [GBK_MENU_RENAME] = "rename",
    [GBK_MENU_EXECUTABLE] = "executable",
    [GBK_MENU_PROPERTIES] = "properties",
    [GBK_MENU_CREATE_FOLDER] = "create-folder",
    [GBK_MENU_CREATE_FILE] = "create-file",
  };
  g_autoptr (GArray) entries = gbk_menu_entries (config, items, target);
  GString *s = g_string_new ("");
  for (guint i = 0; i < entries->len; i++)
    {
      const GbkMenuEntry *e = &g_array_index (entries, GbkMenuEntry, i);
      if (i > 0)
        g_string_append_c (s, ' ');
      if (e->kind == GBK_MENU_ACTION)
        g_string_append_printf (s, "action:%s", e->action->id);
      else
        g_string_append (s, names[e->kind]);
    }
  return g_string_free (s, false);
}

static GPtrArray *
items_of (GbkItem *first, ...)
{
  GPtrArray *a = g_ptr_array_new_with_free_func ((GDestroyNotify) gbk_item_free);
  va_list ap;
  va_start (ap, first);
  for (GbkItem *it = first; it != nullptr; it = va_arg (ap, GbkItem *))
    g_ptr_array_add (a, it);
  va_end (ap);
  return a;
}

enum
{
  ITEMS = GBK_MENU_ON_ITEMS,
  BG = GBK_MENU_ON_BACKGROUND,
  BAR = GBK_MENU_ON_PATHBAR
};

#define assert_menu(items, target, expected) \
  do \
    { \
      g_autoptr (GPtrArray) i_ = items; \
      g_autofree char *d_ = describe (i_, (GbkMenuTarget) (target)); \
      g_assert_cmpstr (d_, ==, expected); \
    } \
  while (0)

static void
test_menus (void)
{
  assert_menu (
      items_of (item ("/d/a.zip", "application/zip", false, false, false), (GbkItem *) nullptr),
      ITEMS,
      "open with | cut copy copy-path | trash delete rename | executable"
      " | action:extract action:compress | properties");
  assert_menu (items_of (item ("/d/go.sh", "application/x-shellscript", false, true, true),
                         (GbkItem *) nullptr),
               ITEMS,
               "open with exec exec-term | cut copy copy-path | trash delete rename | executable"
               " | action:compress | properties");
  assert_menu (items_of (item ("/d/go.sh", "application/x-shellscript", false, false, true),
                         (GbkItem *) nullptr),
               ITEMS,
               "open with exec exec-term | cut copy copy-path | trash delete rename | executable"
               " | action:compress | properties");
  assert_menu (items_of (item ("/d/prog", "application/x-executable", false, true, false),
                         (GbkItem *) nullptr),
               ITEMS,
               "open with exec exec-term | cut copy copy-path | trash delete rename | executable"
               " | action:compress | properties");
  /* A folder: no Executable (it means "can enter" there; Properties has it). */
  assert_menu (
      items_of (item ("/d/sub", "inode/directory", true, false, false), (GbkItem *) nullptr), ITEMS,
      "open open-tab open-window term-here | cut copy paste-into copy-path | trash delete rename"
      " | action:compress | properties");
  /* Two zips: same type, extract applies to both; no Execute or Properties for several. */
  assert_menu (items_of (item ("/d/a.zip", "application/zip", false, false, false),
                         item ("/d/b.zip", "application/zip", false, false, false),
                         (GbkItem *) nullptr),
               ITEMS,
               "open with | cut copy copy-path | trash delete | executable | action:extract "
               "action:compress");
  /* Mixed: no Open With, and extract doesn't apply to the text file. */
  assert_menu (items_of (item ("/d/a.zip", "application/zip", false, false, false),
                         item ("/d/t.txt", "text/plain", false, false, false), (GbkItem *) nullptr),
               ITEMS, "open | cut copy copy-path | trash delete | executable | action:compress");
  /* Two folders: open both in tabs or windows; no Terminal Here or Paste Into for two. */
  assert_menu (
      items_of (item ("/d/s1", "inode/directory", true, false, false),
                item ("/d/s2", "inode/directory", true, false, false), (GbkItem *) nullptr),
      ITEMS, "open open-tab open-window | cut copy copy-path | trash delete | action:compress");
  /* A folder and a file: no new tab/window, no Executable. */
  assert_menu (items_of (item ("/d/s1", "inode/directory", true, false, false),
                         item ("/d/t.txt", "text/plain", false, false, false), (GbkItem *) nullptr),
               ITEMS, "open | cut copy copy-path | trash delete | action:compress");
  /* Path bar: a folder the tab is inside, so nothing that moves, renames or deletes it. */
  assert_menu (items_of (item ("/d", "inode/directory", true, false, false), (GbkItem *) nullptr),
               BAR,
               "open open-tab open-window term-here | copy paste-into copy-path | action:compress "
               "| properties");
  /* Background: the current directory. */
  assert_menu (items_of (item ("/d", "inode/directory", true, false, false), (GbkItem *) nullptr),
               BG,
               "create-folder create-file | paste copy-path | term-here | action:compress"
               " | sort-name sort-mtime sort-size | hidden | properties");
}

static void
test_no_actions_no_dangling_separator (void)
{
  gbk_config_free (config);
  config = gbk_config_new_default ();
  assert_menu (items_of (item ("/d/t.txt", "text/plain", false, false, false), (GbkItem *) nullptr),
               ITEMS,
               "open with | cut copy copy-path | trash delete rename | executable | properties");
  assert_menu (
      items_of (item ("/d", "inode/directory", true, false, false), (GbkItem *) nullptr), BG,
      "create-folder create-file | paste copy-path | term-here | sort-name sort-mtime sort-size"
      " | hidden | properties");
}

static GbkItem *
remote_item (const char *path, const char *mime, bool dir)
{
  GbkItem *it = item (path, mime, dir, !dir, false);
  gbk_loc_unref (it->loc);
  it->loc = gbk_loc_new_remote ("ws", path);
  return it;
}

/* On another machine, only browsing: nothing that changes, runs or opens files. */
static void
test_remote_menus (void)
{
  assert_menu (items_of (remote_item ("/d/a.zip", "application/zip", false), (GbkItem *) nullptr),
               ITEMS, "copy-path | properties");
  assert_menu (items_of (remote_item ("/d/sub", "inode/directory", true), (GbkItem *) nullptr),
               ITEMS, "open open-tab open-window term-here | copy-path | properties");
  assert_menu (items_of (remote_item ("/d", "inode/directory", true), (GbkItem *) nullptr), BG,
               "copy-path | term-here | sort-name sort-mtime sort-size | hidden | properties");
  assert_menu (items_of (remote_item ("/d", "inode/directory", true), (GbkItem *) nullptr), BAR,
               "open open-tab open-window term-here | copy-path | properties");
  /* Every kind is decided (an unknown one is fatal), and files open nowhere yet. */
  int works = 0;
  for (int k = GBK_MENU_SEPARATOR; k <= GBK_MENU_SHOW_HIDDEN; k++)
    works += gbk_menu_works_remote ((GbkMenuKind) k, true) ? 1 : 0;
  g_assert_cmpint (works, ==,
                   10);   /* separator, tab, window, terminal, path, details, 3 sorts, hidden */
  g_assert_false (gbk_menu_works_remote (GBK_MENU_OPEN, true));
  g_assert_true (gbk_menu_works_remote (GBK_MENU_OPEN, false));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  const char conf[] = "[Action extract]\nName=Extract\nTypes=application/zip;\nCommand=x %F\n"
                      "[Action compress]\nName=Compress\nTypes=*\nDirectories=true\nCommand=c %F\n";
  g_autoptr (GPtrArray) errors = g_ptr_array_new_with_free_func (g_free);
  config = gbk_config_load_data (conf, strlen (conf), errors);
  g_assert_cmpuint (errors->len, ==, 0);
  g_test_add_func ("/menu/contents", test_menus);
  g_test_add_func ("/menu/no-actions", test_no_actions_no_dangling_separator);
  g_test_add_func ("/menu/remote", test_remote_menus);
  int status = g_test_run ();
  gbk_config_free (config);
  return status;
}
