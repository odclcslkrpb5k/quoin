/* Running a configured action's command and waiting for it (quoin-serve, for an action on
 * its machine's files: phase 9 of docs/remote-plan.md). The argv is run as it is, never
 * through a shell, in a session of its own, with no terminal: stdin and stdout are
 * /dev/null, and the end of stderr is kept to say why it failed. */
#pragma once
#include "fileops.h"

/* The output kept for a failure's message. */
#define GBK_RUN_TAIL 2048

/* Run `argv` (char*, the program looked up in PATH) in `cwd` and wait for it. Cancelling
 * (hooks->cancelled) ends it: SIGTERM to its whole session, SIGKILL a moment later.
 * hooks->progress hears the program's name once. False with *error unless it exits 0. */
[[nodiscard]] bool gbk_run_command (const GPtrArray *argv, const char *cwd,
                                    const GbkOpsHooks *hooks, GError **error);
