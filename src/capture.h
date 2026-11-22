/* Running a program to its end and keeping what it prints: git (git.h) and the AppImage
 * reader (appicons.h). Never through a shell. stdin is /dev/null; the program gets a
 * session of its own and default signals, and is killed with everything it started when
 * it is cancelled, takes too long or prints too much. Blocking: worker threads only. */
#pragma once

typedef struct
{
  GByteArray *out;      /* all of stdout */
  GByteArray *err_tail; /* the last GBK_CAPTURE_ERR_TAIL bytes of stderr */
  int status;           /* as waitpid gives it */
} GbkCaptured;

void gbk_captured_clear (GbkCaptured *c);

/* Run `argv` (nullptr-terminated; argv[0] looked up in PATH unless it has a '/') with
 * `envp` (nullptr: this process's), in `cwd` (nullptr: here). Errors: it couldn't start
 * (from errno: G_IO_ERROR_NOT_FOUND when there's no such program), G_IO_ERROR_CANCELLED
 * when `cancel_fd` (an eventfd, or -1) became readable, G_IO_ERROR_TIMED_OUT after
 * `timeout_ms`, G_IO_ERROR_NO_SPACE when stdout passed `out_max` bytes. Otherwise true,
 * whatever its exit status (in out->status). */
[[nodiscard]] bool gbk_capture (const char *const *argv, const char *const *envp, const char *cwd,
                                int cancel_fd, int timeout_ms, size_t out_max, GbkCaptured *out,
                                GError **error);

/* The last line of what it said on stderr (valid UTF-8; "" if nothing). g_free. */
[[nodiscard]] char *gbk_captured_last_line (const GbkCaptured *c);

#define GBK_CAPTURE_ERR_TAIL 2048
