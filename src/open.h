/* What "open" means for an item, and doing it.
 *
 * Rules (docs/claude-notes.md):
 *   folder                              -> enter it (the window handles this)
 *   executable + script extension       -> run in the terminal (TerminalRun), output visible
 *   executable, not a script            -> run directly (binaries, AppImages, no extension)
 *   anything else (incl. non-exec script) -> the default application for its MIME type
 * The context menu adds explicit "Execute" / "Execute in Terminal" for executables and for
 * scripts (run via their interpreter when not executable).
 *
 * Launching, running, actions and the terminal need local files: for a location on another
 * machine they fail with G_IO_ERROR_NOT_SUPPORTED, never touching a local file of the same
 * path.
 *
 * Everything works on a snapshot (GbkItem) taken when the user acted, never on live model
 * entries: if the file vanished meanwhile, launching fails with an error, nothing worse. */
#pragma once

#include "config.h"
#include "dirmodel.h"

typedef struct
{
  GbkLoc *loc;
  char *display;        /* UTF-8, for messages */
  char *mime;
  bool is_dir;
  bool regular;         /* regular file (or a symlink to one) */
  bool executable;      /* regular file with an exec bit */
  bool script;          /* extension listed in [Interpreters] */
} GbkItem;

void gbk_item_free (GbkItem *it);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkItem, gbk_item_free)

/* Snapshot of a model entry. `dir` is the directory containing it. */
[[nodiscard]] GbkItem *gbk_item_new (const GbkConfig *config, const GbkTypeTable *types,
                                     const GbkLoc *dir, const GbkEntry *e);
/* The directory itself as an item (right-click on empty space). */
[[nodiscard]] GbkItem *gbk_item_new_dir (GbkLoc *dir);

typedef enum
{
  GBK_OPEN_ENTER,          /* navigate into it */
  GBK_OPEN_RUN,            /* execute directly */
  GBK_OPEN_RUN_TERMINAL,   /* execute in the terminal */
  GBK_OPEN_DEFAULT,        /* default application for its type */
} GbkOpenKind;

GbkOpenKind gbk_open_plan (const GbkItem *it);

/* argv to execute `it` (directly, or the %c part for the terminal): [path] when executable,
 * interpreter + [path] for a non-executable script. nullptr if it can't be executed. */
[[nodiscard]] char **gbk_open_exec_argv (const GbkConfig *config, const GbkItem *it);
bool gbk_open_can_execute (const GbkItem *it);

/* Execute `it`, in the terminal or not. `cwd` is the directory being viewed. */
[[nodiscard]] bool gbk_open_execute (const GbkConfig *config, const GbkItem *it, bool terminal,
                                     const GbkLoc *cwd, GError **error);

/* Open items with the default application for each one's type (one launch per app).
 * `launch` may be nullptr (no startup notification). */
[[nodiscard]] bool gbk_open_default (GPtrArray *items, GAppLaunchContext *launch, GError **error);

/* Open items with a specific application. */
[[nodiscard]] bool gbk_open_with (GAppInfo *app, GPtrArray *items, GAppLaunchContext *launch,
                                  GError **error);

/* Run a configured action on items (directories included when the action allows). */
[[nodiscard]] bool gbk_open_action (const GbkAction *a, GPtrArray *items, const GbkLoc *cwd,
                                    GError **error);

/* F4 / "Open Terminal Here". On another machine: the configured TerminalRun runs ssh to a
 * shell in that folder there. */
[[nodiscard]] bool gbk_open_terminal (const GbkConfig *config, const GbkLoc *dir, GError **error);
/* "Log in in a terminal": TerminalRun runs ssh to `host` once, interactively, so a person
 * can unlock a key or accept a host key; connections after it work by themselves. */
[[nodiscard]] bool gbk_open_ssh_login (const GbkConfig *config, const char *host, GError **error);
