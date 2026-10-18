/* File clipboard formats, shared with other file managers and apps.
 *   x-special/gnome-copied-files: "copy" or "cut", then one file:// URI per line
 *   text/uri-list:                URIs separated by CRLF; '#' lines are comments
 *   text/plain:                   paths, one per line (for pasting into terminals/editors)
 * Parsing handles input from any application: it's fuzzed. */
#pragma once
#include "loc.h"

/* Build the clipboard payloads for absolute `paths`. Free with g_free. */
[[nodiscard]] char *gbk_clip_format_gnome (GPtrArray *paths, bool cut);
[[nodiscard]] char *gbk_clip_format_uri_list (GPtrArray *paths);
[[nodiscard]] char *gbk_clip_format_text (GPtrArray *paths);

/* Parse clipboard data (need not be NUL-terminated). Returns absolute local paths (char*,
 * owned); non-file URIs and garbage lines are skipped. `gnome`: data is in the
 * gnome-copied-files format; *cut tells whether it was a cut. Never nullptr. */
[[nodiscard]] GPtrArray *gbk_clip_parse (const char *data, size_t len, bool gnome, bool *cut);

/* Between Quoin windows only (x-special/quoin-locations): locations on any machine.
 * "copy" or "cut", then a line per location: host (empty: this machine), a tab, the path,
 * both %-escaped. Other applications never see this target: they get files only for what
 * is on this machine. Parsing skips bad lines and refuses invalid hosts. Never nullptr. */
[[nodiscard]] char *gbk_clip_format_locs (const GPtrArray *locs, bool cut);
[[nodiscard]] GPtrArray *gbk_clip_parse_locs (const char *data, size_t len, bool *cut);
