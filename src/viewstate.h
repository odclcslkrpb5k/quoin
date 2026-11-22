/* The view chosen for each folder (Icons or Compact, from the window's ☰ menu or Ctrl+1 /
 * Ctrl+3), remembered in $XDG_STATE_HOME/quoin/views.ini (~/.local/state/quoin): one line
 * per folder ever switched, keyed by where it is ("/home/me/Applications", "ws1:/srv").
 * A choice there beats quoin.conf's IconFolders. Failing to read or write the file loses
 * only those choices (each folder shows its default), never anything else. Main thread. */
#pragma once

typedef enum
{
  GBK_VIEW_CHOICE_NONE,      /* never switched: the default applies */
  GBK_VIEW_CHOICE_COMPACT,
  GBK_VIEW_CHOICE_ICONS,
} GbkViewChoice;

/* `where`: the folder as gbk_loc_display shows it. */
GbkViewChoice gbk_view_state_get (const char *where);
void gbk_view_state_set (const char *where, GbkViewChoice choice);

/* Tests: forget what was read, so the file is read again. */
void gbk_view_state_forget_for_test (void);
