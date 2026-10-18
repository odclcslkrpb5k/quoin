/* Force-included first in every translation unit (see meson.build, -include).
 *
 * 1. Pulls in every system header the project is allowed to use, so they are all
 *    parsed before anything is poisoned.
 * 2. Poisons APIs we have banned. Using one is a compile error, not a review comment.
 *    Need a header that isn't here? Add it above the poison block, never below. */
#pragma once

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

/* Each of these turns a check into nothing; under G_DISABLE_ASSERT, g_assert_not_reached()
 * becomes __builtin_unreachable(): undefined behaviour where an abort was meant. */
#if defined(G_DISABLE_ASSERT) || defined(G_DISABLE_CHECKS) || defined(G_DISABLE_CAST_CHECKS)
#error "G_DISABLE_ASSERT, G_DISABLE_CHECKS and G_DISABLE_CAST_CHECKS must never be defined"
#endif

#include <glib.h>
#include <glib-unix.h>
#include <glib-object.h>
#include <gtk/gtk.h>

#include "gbk-config.h"
#include "fatal.h"
#ifdef GBK_ANALYZER
#include "analyzer-glib.h"
#endif

/* --- Unbounded or error-swallowing libc string/number functions. ---
 * Use g_strdup_printf / GString / g_strlcpy / g_ascii_strtoll with error checks. */
#pragma GCC poison strcpy strcat strncpy strncat sprintf vsprintf gets strtok
#pragma GCC poison atoi atol atoll atof

/* --- Anything that hands a string to a shell. ---
 * Commands are always parsed to argv and spawned directly (no injection via filenames). */
#pragma GCC poison system popen
#pragma GCC poison g_spawn_command_line_async g_spawn_command_line_sync

/* --- Thread cancellation / exit: unwinding would skip every cleanup in C code. Threads
 * finish by returning from their function. (Also why -fexceptions buys us nothing.) --- */
#pragma GCC poison pthread_cancel pthread_exit g_thread_exit

/* --- Nested main loops. Code running inside one (dialog callbacks, model updates, window
 * destruction...) can pull the world out from under the caller: reentrancy crashes. Use
 * asynchronous "response" handlers instead. The clipboard's wait_* functions spin one too. */
#pragma GCC poison gtk_dialog_run gtk_main g_main_loop_run
#pragma GCC poison gtk_clipboard_wait_for_contents gtk_clipboard_wait_for_text
#pragma GCC poison gtk_clipboard_wait_for_uris gtk_clipboard_wait_for_targets
#pragma GCC poison gtk_clipboard_wait_for_image gtk_clipboard_wait_for_rich_text
#pragma GCC poison gtk_clipboard_wait_is_text_available gtk_clipboard_wait_is_uris_available
#pragma GCC poison gtk_clipboard_wait_is_target_available gtk_clipboard_wait_is_image_available
#pragma GCC poison gtk_clipboard_wait_is_rich_text_available

/* --- g_error: use gbk_fatal (fatal.h). --- */
#undef g_error
#pragma GCC poison g_error

/* --- Stack allocation of runtime size. --- */
#undef alloca
#pragma GCC poison alloca
#undef g_alloca
#undef g_alloca0
#undef g_newa
#undef g_newa0
#pragma GCC poison g_alloca g_alloca0 g_newa g_newa0

/* --- Raw signal connections and main-loop sources. ---
 * A callback that outlives the object it points at is the classic GTK crash. Use
 * gbk_connect() / gbk_idle_add() / gbk_timeout_add() / gbk_fd_watch() from lifetime.h: every
 * callback has an owner and is disconnected/removed automatically when the owner is destroyed. Only
 * lifetime.c may use the raw APIs. Macros must be #undef'd before poisoning, because GCC does not
 * flag a poisoned name inside a macro defined before the poison. */
#ifndef GBK_LIFETIME_IMPL
#undef g_signal_connect
#undef g_signal_connect_after
#undef g_signal_connect_swapped
#pragma GCC poison g_signal_connect g_signal_connect_after g_signal_connect_swapped
#pragma GCC poison g_signal_connect_data g_signal_connect_object
#pragma GCC poison g_signal_connect_closure g_signal_connect_closure_by_id
#pragma GCC poison g_idle_add g_idle_add_full g_idle_add_once
#pragma GCC poison g_timeout_add g_timeout_add_full g_timeout_add_once
#pragma GCC poison g_timeout_add_seconds g_timeout_add_seconds_full g_timeout_add_seconds_once
#pragma GCC poison g_source_attach g_source_set_callback g_source_set_closure
#pragma GCC poison g_unix_fd_add g_unix_fd_add_full g_unix_signal_add g_unix_signal_add_full
#pragma GCC poison g_io_add_watch g_io_add_watch_full g_child_watch_add g_child_watch_add_full
#pragma GCC poison gtk_clipboard_request_contents gtk_clipboard_request_text
#pragma GCC poison gtk_clipboard_request_rich_text gtk_clipboard_request_image
#pragma GCC poison gtk_clipboard_request_uris gtk_clipboard_request_targets
#pragma GCC poison gdk_threads_add_idle gdk_threads_add_idle_full
#pragma GCC poison gdk_threads_add_timeout gdk_threads_add_timeout_full
#pragma GCC poison gdk_threads_add_timeout_seconds gdk_threads_add_timeout_seconds_full
#endif
