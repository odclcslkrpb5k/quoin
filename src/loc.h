/* A location: a folder or file on some machine. This one (local) or, later, another machine
 * reached over SSH (docs/remote-plan.md).
 *
 * Why a type and not a path string: both machines have the same layout, so a remote path
 * that reached local file code would be a valid LOCAL path ("delete /home/u/x" on the
 * workstation would delete the laptop's /home/u/x). Everything above the file code carries
 * GbkLoc; the file code (scanner, fileops, launch) takes plain paths; the one way from one to
 * the other is gbk_loc_as_local(), which makes every caller handle "not on this machine".
 *
 * Immutable and atomically refcounted: share freely, across threads too. */
#pragma once

typedef struct GbkLoc GbkLoc;

/* A location on this machine. `path` is made absolute (relative to the current directory)
 * and canonical: no ".", "..", repeated or trailing slashes. */
[[nodiscard]] GbkLoc *gbk_loc_new_local (const char *path);
/* `path` on the same machine as `like` (made canonical; relative to `like` if relative). */
[[nodiscard]] GbkLoc *gbk_loc_new_like (const GbkLoc *like, const char *path);
GbkLoc *gbk_loc_ref (GbkLoc *loc);
void gbk_loc_unref (GbkLoc *loc);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkLoc, gbk_loc_unref)

bool gbk_loc_is_local (const GbkLoc *loc);
/* The path for local file APIs. False (and *out = nullptr) if the location is not on this
 * machine. *out is valid while `loc` is. */
[[nodiscard]] bool gbk_loc_as_local (const GbkLoc *loc, const char **out);

/* The path on its own machine, as bytes. For display and for building other locations
 * only: never hand it to a file API (tools/check-source.py limits where it may be called;
 * file code gets its path from gbk_loc_as_local). */
const char *gbk_loc_path (const GbkLoc *loc);

bool gbk_loc_is_root (const GbkLoc *loc);
/* The containing folder; the root's parent is the root. */
[[nodiscard]] GbkLoc *gbk_loc_parent (const GbkLoc *loc);
/* `name` inside `dir`. `name` is one filename component (not empty, no '/', not "." or "..");
 * anything else is a fatal programming error (names come from validated entries). */
[[nodiscard]] GbkLoc *gbk_loc_child (const GbkLoc *dir, const char *name);
/* Last component as raw bytes ("/" for the root). g_free. */
[[nodiscard]] char *gbk_loc_name (const GbkLoc *loc);
/* Last component as UTF-8 for showing. g_free. */
[[nodiscard]] char *gbk_loc_display_name (const GbkLoc *loc);
/* The whole location as UTF-8 for showing (title bar, tooltips, messages). g_free. */
[[nodiscard]] char *gbk_loc_display (const GbkLoc *loc);

bool gbk_loc_equal (const GbkLoc *a, const GbkLoc *b);
/* `loc` is `root` or inside it, on the same machine ("/a/bc" is not inside "/a/b"). */
bool gbk_loc_is_under (const GbkLoc *loc, const GbkLoc *root);
/* `loc` is directly inside `dir`. */
bool gbk_loc_is_child_of (const GbkLoc *loc, const GbkLoc *dir);

/* A GPtrArray that owns GbkLoc references. */
[[nodiscard]] GPtrArray *gbk_loc_array_new (void);
/* Locations for local paths (char*), in order. */
[[nodiscard]] GPtrArray *gbk_loc_array_from_local (const GPtrArray *paths);
/* The local paths (char*, owned) of `locs`, or nullptr if any is not on this machine. */
[[nodiscard]] GPtrArray *gbk_loc_array_as_local (const GPtrArray *locs);

/* Tests: a location on another machine, so the "not on this machine" branches can be
 * exercised before remote connections exist. */
[[nodiscard]] GbkLoc *gbk_loc_new_remote_for_test (const char *host, const char *path);
