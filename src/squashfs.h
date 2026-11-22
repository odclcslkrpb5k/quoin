/* Reading files from a squashfs image (format 4.0), as found inside AppImages (appimage.h).
 * Read-only and for untrusted images: every offset and size is checked against the image
 * and against limits below, decompression never makes more than it may, and the whole
 * image's decompressed output is capped, so a hostile image ends in an error, never in a
 * crash or a hang. gzip, zstd and xz compression; others are refused. Fuzzed. */
#pragma once

/* Read exactly `len` bytes at `offset` of the underlying file into `buf`; false if it
 * can't (past the end included). */
typedef bool (*GbkReadAt) (void *ctx, uint64_t offset, void *buf, size_t len);

typedef struct GbkSquash GbkSquash;

/* The image at `base` in a file of `size` bytes. */
[[nodiscard]] GbkSquash *gbk_squash_open (GbkReadAt read, void *ctx, uint64_t base, uint64_t size,
                                          GError **error);
void gbk_squash_free (GbkSquash *s);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkSquash, gbk_squash_free)

/* The names in the image's top folder (char*), in the image's order. */
[[nodiscard]] GPtrArray *gbk_squash_root_names (GbkSquash *s, GError **error);

/* The contents of the regular file at `path` (relative to the top, parts separated by
 * '/'), following symbolic links that stay inside the image; an error if it is bigger
 * than `max` bytes. */
[[nodiscard]] GBytes *gbk_squash_read (GbkSquash *s, const char *path, size_t max, GError **error);

#define GBK_SQUASH_DECOMPRESS_MAX (UINT64_C (64) << 20) /* in all, per image */
#define GBK_SQUASH_ENTRIES_MAX 65536                    /* names read in all, per image */
#define GBK_SQUASH_LINKS_MAX 8                          /* symbolic links followed per path */
#define GBK_SQUASH_STEPS_MAX 256                        /* path parts walked per path */
