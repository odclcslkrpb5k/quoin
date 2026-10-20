/* Context menu contents, computed from a selection snapshot. Pure: no GTK, so it's tested
 * directly. The window turns entries into a GtkMenu and performs them.
 *
 * On items:      Open, Open in New Tab / Window (all folders), Open With (files only),
 *                Execute / Execute in Terminal (one runnable file), Open Terminal Here (one
 *                folder), clipboard and file operations, Copy Full Path, Executable (all
 *                regular files), configured actions that match every item, Properties (one).
 * On empty space (the current directory as a folder): Create Folder / File, Paste, Copy
 *                Full Path, Open Terminal Here, actions that accept directories, the sort
 *                modes and Show Hidden Files, Properties.
 * On a path bar button (one folder, an ancestor of or the current one): opening, Copy,
 *                Paste Into Folder, Copy Full Path, actions and Properties; nothing that
 *                moves, renames or deletes it, since the tab is inside it. */
#pragma once

#include "open.h"

typedef enum
{
  GBK_MENU_SEPARATOR,
  GBK_MENU_OPEN,
  GBK_MENU_OPEN_TAB,           /* folders, each in a new tab */
  GBK_MENU_OPEN_WINDOW,        /* folders, each in a new window */
  GBK_MENU_OPEN_WITH,          /* submenu: the window lists the files' type's applications
                       * (one type) and Other Application… */
  GBK_MENU_EXECUTE,
  GBK_MENU_EXECUTE_TERMINAL,
  GBK_MENU_TERMINAL_HERE,
  GBK_MENU_ACTION,             /* `action` */
  GBK_MENU_CUT,
  GBK_MENU_COPY,
  GBK_MENU_COPY_PATH,          /* the items' full paths as text, one per line */
  GBK_MENU_PASTE,              /* into the current directory (background menu) */
  GBK_MENU_PASTE_INTO,         /* into the one selected folder */
  GBK_MENU_TRASH,
  GBK_MENU_DELETE,
  GBK_MENU_RENAME,
  GBK_MENU_EXECUTABLE,         /* check item: on when every item is executable */
  GBK_MENU_PROPERTIES,         /* one item */
  GBK_MENU_CREATE_FOLDER,      /* in the current directory */
  GBK_MENU_CREATE_FILE,
  GBK_MENU_SORT_NAME,
  GBK_MENU_SORT_MTIME,
  GBK_MENU_SORT_SIZE,
  GBK_MENU_SHOW_HIDDEN,
} GbkMenuKind;

typedef struct
{
  GbkMenuKind kind;
  GbkAction *action;           /* GBK_MENU_ACTION; borrowed from the config */
} GbkMenuEntry;

typedef enum
{
  GBK_MENU_ON_ITEMS,           /* the selection */
  GBK_MENU_ON_BACKGROUND,      /* empty space: `items` is the current directory alone */
  GBK_MENU_ON_PATHBAR,         /* a path bar button: `items` is that directory alone */
} GbkMenuTarget;

/* Returns GbkMenuEntry elements; never starts/ends with or doubles a separator. */
/* Does `kind` work on another machine yet (docs/remote-plan.md: browsing, opening read-only
 * copies and copying to this machine; nothing that changes or runs anything there)? The
 * menu and the window's shortcuts both ask this. */
bool gbk_menu_works_remote (GbkMenuKind kind);

/* For items on another machine (any of them), only what gbk_menu_works_remote allows. */
[[nodiscard]] GArray *gbk_menu_entries (const GbkConfig *config, GPtrArray *items,
                                        GbkMenuTarget target);

/* The common MIME type of all items, or nullptr if they differ or include folders. */
const char *gbk_menu_common_mime (GPtrArray *items);
