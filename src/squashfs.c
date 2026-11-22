#include "squashfs.h"
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

/* Every function here that returns false or nullptr has set *error. */

#define MAGIC 0x73717368u              /* "hsqs" */
#define SUPERBLOCK_SIZE 96u
#define META_MAX 8192u                 /* a metadata block, uncompressed */
#define BLOCK_MAX (1u << 20)           /* the largest data block the format allows */
#define NO_FRAGMENT 0xFFFFFFFFu
#define BLOCK_UNCOMPRESSED (1u << 24)  /* in a data block's size */
#define BLOCK_SIZE_MASK (BLOCK_UNCOMPRESSED - 1)
#define META_UNCOMPRESSED 0x8000u      /* in a metadata block's header */
#define FRAGMENTS_PER_BLOCK 512u
#define FRAGMENT_ENTRY 16u
#define SYMLINK_MAX 4096u
#define XZ_MEMLIMIT (UINT64_C (64) << 20)

enum
{
  COMP_GZIP = 1,
  COMP_XZ = 4,
  COMP_ZSTD = 6,
};

enum
{
  INODE_DIR = 1,
  INODE_FILE = 2,
  INODE_SYMLINK = 3,
  INODE_LDIR = 8,
  INODE_LFILE = 9,
  INODE_LSYMLINK = 10,
};

struct GbkSquash
{
  GbkReadAt read;
  void *ctx;
  uint64_t base, size;          /* the image: `size` bytes from `base` */
  uint32_t block_size;
  uint16_t comp;
  uint32_t fragments;
  uint64_t root_ref;
  uint64_t inode_table, dir_table, fragment_table;
  uint64_t decompressed;        /* so far, against GBK_SQUASH_DECOMPRESS_MAX */
  uint32_t entries;             /* names read so far, against GBK_SQUASH_ENTRIES_MAX */
};

static void
set_bad (GError **error, const char *what)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Not a readable squashfs image: %s",
               what);
}

[[nodiscard]] static bool
bad (GError **error, const char *what)
{
  set_bad (error, what);
  return false;
}

static uint64_t
le (const uint8_t *p, unsigned bytes)
{
  uint64_t v = 0;
  for (unsigned i = 0; i < bytes; i++)
    v |= (uint64_t) p[i] << (8 * i);
  return v;
}

/* `len` bytes at `off` in the image. */
[[nodiscard]] static bool
read_raw (GbkSquash *s, uint64_t off, void *buf, size_t len, GError **error)
{
  if (off > s->size || len > s->size - off)
    return bad (error, "something past its end");
  if (!s->read (s->ctx, s->base + off, buf, len))
    return bad (error, "it can't be read");
  return true;
}

/* Decompress `in` into `out` (room for `cap` bytes); *got: how much came out. */
[[nodiscard]] static bool
decompress (GbkSquash *s, const uint8_t *in, size_t in_len, uint8_t *out, size_t cap, size_t *got,
            GError **error)
{
  if (s->decompressed + cap > GBK_SQUASH_DECOMPRESS_MAX)
    return bad (error, "too much to decompress");
  switch (s->comp)
    {
    case COMP_GZIP:
      {
        uLongf dst = (uLongf) cap;
        uLong src = (uLong) in_len;
        if (uncompress2 (out, &dst, in, &src) != Z_OK)
          return bad (error, "bad gzip data");
        *got = dst;
        break;
      }
    case COMP_ZSTD:
      {
        size_t r = ZSTD_decompress (out, cap, in, in_len);
        if (ZSTD_isError (r))
          return bad (error, "bad zstd data");
        *got = r;
        break;
      }
    case COMP_XZ:
      {
        uint64_t memlimit = XZ_MEMLIMIT;
        size_t in_pos = 0, out_pos = 0;
        if (lzma_stream_buffer_decode (&memlimit, 0, nullptr, in, &in_pos, in_len, out, &out_pos,
                                       cap)
            != LZMA_OK)
          return bad (error, "bad xz data");
        *got = out_pos;
        break;
      }
    default: return bad (error, "unsupported compression");
    }
  s->decompressed += *got;
  return true;
}

/* ---- metadata: a stream of blocks of at most 8 KiB each ---- */

typedef struct
{
  GbkSquash *s;
  uint64_t next;                /* the next block's place in the image */
  uint8_t buf[META_MAX];
  size_t len, pos;
} Meta;

[[nodiscard]] static bool
meta_load (Meta *m, GError **error)
{
  uint8_t head[2];
  if (!read_raw (m->s, m->next, head, 2, error))
    return false;
  uint16_t h = (uint16_t) le (head, 2);
  size_t size = h & ~META_UNCOMPRESSED;
  if (size == 0 || size > META_MAX)
    return bad (error, "a metadata block of a bad size");
  uint8_t raw[META_MAX];
  if (!read_raw (m->s, m->next + 2, raw, size, error))
    return false;
  if (h & META_UNCOMPRESSED)
    {
      memcpy (m->buf, raw, size);
      m->len = size;
    }
  else if (!decompress (m->s, raw, size, m->buf, META_MAX, &m->len, error))
    return false;
  if (m->len == 0)
    return bad (error, "an empty metadata block");
  m->next += 2 + size;
  m->pos = 0;
  return true;
}

[[nodiscard]] static bool
meta_seek (Meta *m, GbkSquash *s, uint64_t table, uint64_t block, size_t offset, GError **error)
{
  m->s = s;
  if (block > s->size || table > s->size - block)
    return bad (error, "a metadata reference past its end");
  m->next = table + block;
  if (!meta_load (m, error))
    return false;
  if (offset > m->len)
    return bad (error, "an offset past its metadata block");
  m->pos = offset;
  return true;
}

[[nodiscard]] static bool
meta_read (Meta *m, void *dst, size_t n, GError **error)
{
  uint8_t *out = dst;
  while (n > 0)
    {
      if (m->pos == m->len && !meta_load (m, error))
        return false;
      size_t take = MIN (n, m->len - m->pos);
      memcpy (out, m->buf + m->pos, take);
      m->pos += take;
      out += take;
      n -= take;
    }
  return true;
}

[[nodiscard]] static bool
meta_u16 (Meta *m, uint16_t *v, GError **error)
{
  uint8_t b[2];
  if (!meta_read (m, b, 2, error))
    return false;
  *v = (uint16_t) le (b, 2);
  return true;
}

[[nodiscard]] static bool
meta_u32 (Meta *m, uint32_t *v, GError **error)
{
  uint8_t b[4];
  if (!meta_read (m, b, 4, error))
    return false;
  *v = (uint32_t) le (b, 4);
  return true;
}

[[nodiscard]] static bool
meta_u64 (Meta *m, uint64_t *v, GError **error)
{
  uint8_t b[8];
  if (!meta_read (m, b, 8, error))
    return false;
  *v = le (b, 8);
  return true;
}

/* ---- inodes ---- */

typedef struct
{
  uint16_t type;                /* INODE_DIR, INODE_FILE or INODE_SYMLINK; others as read */
  uint32_t dir_block, dir_size; /* directory */
  uint16_t dir_offset;
  uint64_t blocks_start, file_size; /* file */
  uint32_t fragment, fragment_offset;
  char *target;                 /* symbolic link */
} Inode;

static void
inode_clear (Inode *n)
{
  g_free (n->target);
  n->target = nullptr;
}

[[nodiscard]] static bool
read_dir_inode (Meta *m, Inode *n, bool large, GError **error)
{
  uint32_t a, b, c, d;
  uint16_t h1, h2;
  if (large)
    {
      /* links, size, block, parent, index count, offset (then an index we don't need) */
      if (!meta_u32 (m, &a, error) || !meta_u32 (m, &b, error) || !meta_u32 (m, &c, error)
          || !meta_u32 (m, &d, error) || !meta_u16 (m, &h1, error) || !meta_u16 (m, &h2, error))
        return false;
      n->dir_size = b;
      n->dir_block = c;
    }
  else
    {
      /* block, links, size, offset, parent */
      if (!meta_u32 (m, &a, error) || !meta_u32 (m, &b, error) || !meta_u16 (m, &h1, error)
          || !meta_u16 (m, &h2, error) || !meta_u32 (m, &c, error))
        return false;
      n->dir_block = a;
      n->dir_size = h1;
    }
  n->type = INODE_DIR;
  n->dir_offset = h2;
  return true;
}

[[nodiscard]] static bool
read_file_inode (Meta *m, Inode *n, bool large, GError **error)
{
  uint32_t a, b, c, d;
  if (large)
    {
      /* start, size, sparse, links, fragment, offset in it, xattr */
      uint64_t start, size, sparse;
      if (!meta_u64 (m, &start, error) || !meta_u64 (m, &size, error)
          || !meta_u64 (m, &sparse, error) || !meta_u32 (m, &a, error) || !meta_u32 (m, &b, error)
          || !meta_u32 (m, &c, error) || !meta_u32 (m, &d, error))
        return false;
      n->blocks_start = start;
      n->file_size = size;
      n->fragment = b;
      n->fragment_offset = c;
    }
  else
    {
      /* start, fragment, offset in it, size */
      if (!meta_u32 (m, &a, error) || !meta_u32 (m, &b, error) || !meta_u32 (m, &c, error)
          || !meta_u32 (m, &d, error))
        return false;
      n->blocks_start = a;
      n->fragment = b;
      n->fragment_offset = c;
      n->file_size = d;
    }
  n->type = INODE_FILE;
  return true;
}

[[nodiscard]] static bool
read_symlink_inode (Meta *m, Inode *n, GError **error)
{
  uint32_t links, len;
  if (!meta_u32 (m, &links, error) || !meta_u32 (m, &len, error))
    return false;
  if (len == 0 || len > SYMLINK_MAX)
    return bad (error, "a symbolic link of a bad length");
  n->target = g_malloc (len + 1);
  if (!meta_read (m, n->target, len, error))
    return false;
  n->target[len] = '\0';
  if (strlen (n->target) != len)
    return bad (error, "a symbolic link with a NUL in it");
  n->type = INODE_SYMLINK;
  return true;
}

/* The inode `ref` points at; for a file, `m` is left at its list of block sizes. */
[[nodiscard]] static bool
read_inode (GbkSquash *s, uint64_t ref, Inode *n, Meta *m, GError **error)
{
  memset (n, 0, sizeof *n);
  if ((ref & 0xffff) >= META_MAX)
    return bad (error, "a bad inode reference");
  if (!meta_seek (m, s, s->inode_table, ref >> 16, ref & 0xffff, error))
    return false;
  uint8_t head[16];             /* type, mode, uid, gid, mtime, number */
  if (!meta_read (m, head, sizeof head, error))
    return false;
  uint16_t type = (uint16_t) le (head, 2);
  switch (type)
    {
    case INODE_DIR: return read_dir_inode (m, n, false, error);
    case INODE_LDIR: return read_dir_inode (m, n, true, error);
    case INODE_FILE: return read_file_inode (m, n, false, error);
    case INODE_LFILE: return read_file_inode (m, n, true, error);
    case INODE_SYMLINK:
    case INODE_LSYMLINK: return read_symlink_inode (m, n, error);
    default:
      n->type = type;                  /* a device, a pipe, a socket: nothing to read */
      return true;
    }
}

/* ---- directories ---- */

typedef bool (*EntryFn) (const char *name, uint64_t ref, void *data); /* false: stop */

/* Each entry of directory `dir` to `fn`, until it says stop. */
[[nodiscard]] static bool
walk_dir (GbkSquash *s, const Inode *dir, EntryFn fn, void *data, GError **error)
{
  if (dir->dir_size < 3)
    return bad (error, "a directory of a bad size");
  uint64_t left = dir->dir_size - 3;   /* the format counts "." and ".." as 3 bytes */
  if (left == 0)
    return true;
  g_autofree Meta *m = g_new (Meta, 1);
  if (!meta_seek (m, s, s->dir_table, dir->dir_block, dir->dir_offset, error))
    return false;
  while (left > 0)
    {
      uint32_t count, start, number;
      if (left < 12)
        return bad (error, "a directory cut short");
      if (!meta_u32 (m, &count, error) || !meta_u32 (m, &start, error)
          || !meta_u32 (m, &number, error))
        return false;
      left -= 12;
      if (count >= 256)
        return bad (error, "a directory header for too many names");
      for (uint32_t i = 0; i <= count; i++)
        {
          uint16_t offset, inode_delta, type, name_size;
          if (left < 8)
            return bad (error, "a directory cut short");
          if (!meta_u16 (m, &offset, error) || !meta_u16 (m, &inode_delta, error)
              || !meta_u16 (m, &type, error) || !meta_u16 (m, &name_size, error))
            return false;
          left -= 8;
          size_t len = (size_t) name_size + 1;
          if (len > 256 || left < len)
            return bad (error, "a bad name in a directory");
          char name[257];
          if (!meta_read (m, name, len, error))
            return false;
          name[len] = '\0';
          left -= len;
          if (strlen (name) != len || strchr (name, '/') != nullptr || g_str_equal (name, ".")
              || g_str_equal (name, ".."))
            return bad (error, "a bad name in a directory");
          if (++s->entries > GBK_SQUASH_ENTRIES_MAX)
            return bad (error, "too many names");
          if (!fn (name, ((uint64_t) start << 16) | offset, data))
            return true;
        }
    }
  return true;
}

static bool
collect (const char *name, uint64_t ref G_GNUC_UNUSED, void *data)
{
  g_ptr_array_add (data, g_strdup (name));
  return true;
}

typedef struct
{
  const char *want;
  uint64_t ref;
  bool found;
} Find;

static bool
find_one (const char *name, uint64_t ref, void *data)
{
  Find *f = data;
  if (!g_str_equal (name, f->want))
    return true;
  f->ref = ref;
  f->found = true;
  return false;
}

GPtrArray *
gbk_squash_root_names (GbkSquash *s, GError **error)
{
  g_autofree Meta *m = g_new (Meta, 1);
  Inode root;
  bool ok = read_inode (s, s->root_ref, &root, m, error);
  inode_clear (&root);
  if (!ok)
    return nullptr;
  if (root.type != INODE_DIR)
    {
      set_bad (error, "its top isn't a directory");
      return nullptr;
    }
  g_autoptr (GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  if (!walk_dir (s, &root, collect, names, error))
    return nullptr;
  return g_steal_pointer (&names);
}

/* ---- paths ---- */

/* The parts of `path` in front of the queue `parts` (char*, last first). */
static void
push_parts (GPtrArray *parts, const char *path)
{
  g_auto (GStrv) split = g_strsplit (path, "/", -1);
  for (guint n = g_strv_length (split); n > 0; n--)
    if (split[n - 1][0] != '\0')
      g_ptr_array_add (parts, g_strdup (split[n - 1]));
}

/* The regular file at `path`: its inode in *n, `m` at its block list. Symbolic links are
 * followed, relative to their folder; nothing may lead outside the image's top. */
[[nodiscard]] static bool
resolve (GbkSquash *s, const char *path, Inode *n, Meta *m, GError **error)
{
  g_autoptr (GArray) dirs = g_array_new (false, false, sizeof (uint64_t)); /* refs from the top */
  g_array_append_val (dirs, s->root_ref);
  g_autoptr (GPtrArray) parts = g_ptr_array_new_with_free_func (g_free);  /* next part last */
  push_parts (parts, path);
  int links = 0, steps = 0;
  memset (n, 0, sizeof *n);
  while (parts->len > 0)
    {
      if (++steps > GBK_SQUASH_STEPS_MAX)
        return bad (error, "a path too long to follow");
      g_autofree char *part = g_ptr_array_steal_index (parts, parts->len - 1);
      if (g_str_equal (part, "."))
        continue;
      if (g_str_equal (part, ".."))
        {
          if (dirs->len <= 1)
            return bad (error, "a path leading outside it");
          g_array_set_size (dirs, dirs->len - 1);
          continue;
        }
      Inode dir;
      bool ok = read_inode (s, g_array_index (dirs, uint64_t, dirs->len - 1), &dir, m, error);
      inode_clear (&dir);
      if (!ok)
        return false;
      if (dir.type != INODE_DIR)
        return bad (error, "a path through something that isn't a directory");
      Find f = { .want = part };
      if (!walk_dir (s, &dir, find_one, &f, error))
        return false;
      if (!f.found)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Not in the image: %s", path);
          return false;
        }
      inode_clear (n);
      if (!read_inode (s, f.ref, n, m, error))
        return false;
      if (n->type == INODE_SYMLINK)
        {
          if (++links > GBK_SQUASH_LINKS_MAX)
            return bad (error, "too many symbolic links");
          if (n->target[0] == '/')
            g_array_set_size (dirs, 1);  /* from the image's top, not this machine's */
          push_parts (parts, n->target);
          continue;
        }
      if (n->type == INODE_DIR)
        {
          g_array_append_val (dirs, f.ref);
          continue;
        }
      if (parts->len > 0 || n->type != INODE_FILE)
        return bad (error, "a path to something that isn't a file");
    }
  if (n->type != INODE_FILE)
    return bad (error, "a path to something that isn't a file");
  return true;
}

/* ---- file contents ---- */

/* A data block: `disk` as recorded (size, and whether it's compressed) at `*at`, unpacked to
 * exactly `want` bytes at `out`. */
[[nodiscard]] static bool
read_block (GbkSquash *s, uint64_t *at, uint32_t disk, uint8_t *out, size_t want, GError **error)
{
  size_t size = disk & BLOCK_SIZE_MASK;
  if (size == 0)
    {
      memset (out, 0, want);           /* sparse: all zeros, nothing stored */
      return true;
    }
  if (size > s->block_size)
    return bad (error, "a data block of a bad size");
  g_autofree uint8_t *raw = g_malloc (size);
  if (!read_raw (s, *at, raw, size, error))
    return false;
  *at += size;
  if (disk & BLOCK_UNCOMPRESSED)
    {
      if (size != want)
        return bad (error, "a data block of the wrong size");
      memcpy (out, raw, size);
      return true;
    }
  size_t got;
  if (!decompress (s, raw, size, out, want, &got, error))
    return false;
  if (got != want)
    return bad (error, "a data block of the wrong size");
  return true;
}

/* The tail of a file: `len` bytes at `offset` in fragment block `index`. */
[[nodiscard]] static bool
read_fragment (GbkSquash *s, uint32_t index, uint32_t offset, uint8_t *out, size_t len,
               GError **error)
{
  if (index >= s->fragments)
    return bad (error, "a missing fragment");
  /* A table of where each metadata block of fragment entries is, then the entry. */
  uint8_t where[8];
  if (!read_raw (s, s->fragment_table + (uint64_t) (index / FRAGMENTS_PER_BLOCK) * 8, where, 8,
                 error))
    return false;
  g_autofree Meta *m = g_new (Meta, 1);
  if (!meta_seek (m, s, 0, le (where, 8), (size_t) (index % FRAGMENTS_PER_BLOCK) * FRAGMENT_ENTRY,
                  error))
    return false;
  uint64_t start;
  uint32_t disk, unused;
  if (!meta_u64 (m, &start, error) || !meta_u32 (m, &disk, error) || !meta_u32 (m, &unused, error))
    return false;
  size_t size = disk & BLOCK_SIZE_MASK;
  if (size == 0 || size > s->block_size)
    return bad (error, "a fragment block of a bad size");
  g_autofree uint8_t *raw = g_malloc (size);
  if (!read_raw (s, start, raw, size, error))
    return false;
  g_autofree uint8_t *block = g_malloc (s->block_size);
  size_t got = size;
  if (disk & BLOCK_UNCOMPRESSED)
    memcpy (block, raw, size);
  else if (!decompress (s, raw, size, block, s->block_size, &got, error))
    return false;
  if (offset > got || len > got - offset)
    return bad (error, "a fragment too short");
  memcpy (out, block + offset, len);
  return true;
}

GBytes *
gbk_squash_read (GbkSquash *s, const char *path, size_t max, GError **error)
{
  g_autofree Meta *m = g_new (Meta, 1);
  Inode n;
  bool ok = resolve (s, path, &n, m, error);
  inode_clear (&n);
  if (!ok)
    return nullptr;
  if (n.file_size > max)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Too big: %s", path);
      return nullptr;
    }
  size_t size = (size_t) n.file_size;
  bool fragment = n.fragment != NO_FRAGMENT;
  size_t blocks = size / s->block_size + (!fragment && size % s->block_size != 0 ? 1 : 0);
  g_autofree uint8_t *data = g_malloc (MAX (size, 1));
  uint64_t at = n.blocks_start;
  size_t done = 0;
  for (size_t i = 0; i < blocks; i++)
    {
      uint32_t disk;
      if (!meta_u32 (m, &disk, error))
        return nullptr;
      size_t want = MIN ((size_t) s->block_size, size - done);
      if (!read_block (s, &at, disk, data + done, want, error))
        return nullptr;
      done += want;
    }
  if (done < size
      && (!fragment
          || !read_fragment (s, n.fragment, n.fragment_offset, data + done, size - done, error)))
    {
      if (!fragment)
        set_bad (error, "a file cut short");
      return nullptr;
    }
  return g_bytes_new_take (g_steal_pointer (&data), size);
}

/* ---- the image ---- */

GbkSquash *
gbk_squash_open (GbkReadAt read, void *ctx, uint64_t base, uint64_t size, GError **error)
{
  if (base > size)
    {
      set_bad (error, "it starts past the end of the file");
      return nullptr;
    }
  g_autoptr (GbkSquash) s = g_new0 (GbkSquash, 1);
  s->read = read;
  s->ctx = ctx;
  s->base = base;
  s->size = size - base;
  uint8_t sb[SUPERBLOCK_SIZE];
  if (!read_raw (s, 0, sb, sizeof sb, error))
    return nullptr;
  if (le (sb, 4) != MAGIC)
    {
      set_bad (error, "no squashfs magic");
      return nullptr;
    }
  s->block_size = (uint32_t) le (sb + 12, 4);
  s->fragments = (uint32_t) le (sb + 16, 4);
  s->comp = (uint16_t) le (sb + 20, 2);
  uint16_t block_log = (uint16_t) le (sb + 22, 2);
  uint16_t major = (uint16_t) le (sb + 28, 2);
  s->root_ref = le (sb + 32, 8);
  s->inode_table = le (sb + 64, 8);
  s->dir_table = le (sb + 72, 8);
  s->fragment_table = le (sb + 80, 8);
  if (major != 4)
    {
      set_bad (error, "not version 4");
      return nullptr;
    }
  if (s->block_size < 4096 || s->block_size > BLOCK_MAX || block_log > 20
      || (UINT32_C (1) << block_log) != s->block_size)
    {
      set_bad (error, "a bad block size");
      return nullptr;
    }
  if (s->comp != COMP_GZIP && s->comp != COMP_ZSTD && s->comp != COMP_XZ)
    {
      set_bad (error, "unsupported compression");
      return nullptr;
    }
  if (s->inode_table >= s->size || s->dir_table >= s->size)
    {
      set_bad (error, "tables past its end");
      return nullptr;
    }
  return g_steal_pointer (&s);
}

void
gbk_squash_free (GbkSquash *s)
{
  g_free (s);
}
