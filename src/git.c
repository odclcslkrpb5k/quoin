#include "git.h"
#include "scanner.h"
#include <poll.h>
#include <spawn.h>
#include <sys/vfs.h>

#define STDERR_TAIL 2048
#define WAIT_POLL_MS 50

extern char **environ;

const char *
gbk_git_state_label (GbkGitState state, bool folder)
{
  switch (state)
    {
    case GBK_GIT_NONE: return folder ? "No changes" : "Unchanged";
    case GBK_GIT_IGNORED: return "Ignored";
    case GBK_GIT_UNTRACKED: return folder ? "Untracked files" : "Untracked";
    case GBK_GIT_ADDED: return folder ? "New files, staged" : "New, staged";
    case GBK_GIT_STAGED: return "Changes staged";
    case GBK_GIT_MODIFIED: return "Changes not staged";
    case GBK_GIT_CONFLICT: return folder ? "Conflicts" : "Conflict";
    case GBK_GIT_N_STATES:
    default: gbk_fatal ("bad git state %d", (int) state);
    }
}

/* ---- parsing ---- */

void
gbk_git_status_free (GbkGitStatus *s)
{
  if (s == nullptr)
    return;
  g_free (s->head);
  g_free (s->oid);
  g_free (s->upstream);
  g_ptr_array_unref (s->paths);
  g_free (s);
}

static void
set_bad (GError **error, const char *what)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unexpected git output: %s", what);
}

[[nodiscard]] static bool
bad (GError **error, const char *what)
{
  set_bad (error, what);
  return false;
}

[[nodiscard]] static GbkGitStatus *
bad_status (GError **error, const char *what)
{
  set_bad (error, what);
  return nullptr;
}

/* A clean relative path: no leading '/', no empty, "." or ".." parts; a trailing '/' (a
 * folder) only when `folder_ok`. */
static bool
valid_path (const char *p, bool folder_ok)
{
  size_t len = strlen (p);
  if (len == 0 || p[0] == '/')
    return false;
  if (p[len - 1] == '/')
    {
      if (!folder_ok || len == 1 || p[len - 2] == '/')
        return false;
      len--;
    }
  const char *part = p;
  const char *end = p + len;
  while (part < end)
    {
      const char *slash = memchr (part, '/', (size_t) (end - part));
      size_t n = slash != nullptr ? (size_t) (slash - part) : (size_t) (end - part);
      if (n == 0 || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.'))
        return false;
      part += n + 1;
    }
  return true;
}

static void
add_path (GbkGitStatus *s, GbkGitState state, const char *path)
{
  size_t len = strlen (path);
  GbkGitPath *p = g_malloc (sizeof *p + len + 1);
  p->state = state;
  memcpy (p->path, path, len + 1);
  g_ptr_array_add (s->paths, p);
}

/* The rest of `rec` after `n` space-separated fields, or nullptr if they aren't there. */
static const char *
skip_fields (const char *rec, int n)
{
  const char *p = rec;
  for (int i = 0; i < n; i++)
    {
      const char *space = strchr (p, ' ');
      if (space == nullptr || space == p)
        return nullptr;
      p = space + 1;
    }
  return p;
}

static bool
valid_xy (const char *xy)
{
  static const char codes[] = ".MTADRCU";
  return xy[0] != '\0' && xy[1] != '\0' && xy[2] == ' ' && strchr (codes, xy[0]) != nullptr
         && strchr (codes, xy[1]) != nullptr;
}

static bool
valid_oid (const char *v)
{
  size_t n = strlen (v);
  if (n < 4 || n > 64)
    return false;
  for (size_t i = 0; i < n; i++)
    if (!g_ascii_isxdigit (v[i]))
      return false;
  return true;
}

[[nodiscard]] static bool
parse_count (const char *v, char sign, uint64_t *out)
{
  if (v[0] != sign)
    return false;
  return g_ascii_string_to_unsigned (v + 1, 10, 0, G_MAXUINT32, out, nullptr);
}

[[nodiscard]] static bool
parse_header (GbkGitStatus *s, const char *rec, GError **error)
{
  const char *key = rec + 2;
  const char *space = strchr (key, ' ');
  if (space == nullptr || space[1] == '\0')
    return true;                  /* a header we don't use, such as "# stash 2" */
  g_autofree char *name = g_strndup (key, (size_t) (space - key));
  const char *value = space + 1;
  if (g_str_equal (name, "branch.oid"))
    {
      g_clear_pointer (&s->oid, g_free);
      if (!g_str_equal (value, "(initial)"))
        {
          if (!valid_oid (value))
            return bad (error, "commit id");
          s->oid = g_strdup (value);
        }
    }
  else if (g_str_equal (name, "branch.head"))
    {
      g_clear_pointer (&s->head, g_free);
      if (!g_str_equal (value, "(detached)"))
        s->head = g_strdup (value);
    }
  else if (g_str_equal (name, "branch.upstream"))
    {
      g_free (s->upstream);
      s->upstream = g_strdup (value);
    }
  else if (g_str_equal (name, "branch.ab"))
    {
      g_auto (GStrv) ab = g_strsplit (value, " ", -1);
      if (g_strv_length (ab) != 2 || !parse_count (ab[0], '+', &s->ahead)
          || !parse_count (ab[1], '-', &s->behind))
        return bad (error, "ahead/behind");
      s->has_ab = true;
    }
  return true;
}

/* A changed entry ("1", "2" or "u"): its state from the index (X) and work tree (Y) codes. */
static GbkGitState
changed_state (GbkGitStatus *s, char type, const char *xy)
{
  if (type == 'u')
    {
      s->n_conflict++;
      return GBK_GIT_CONFLICT;
    }
  if (xy[0] != '.')
    s->n_staged++;
  if (xy[1] != '.')
    {
      s->n_modified++;
      return GBK_GIT_MODIFIED;
    }
  if (xy[0] == 'A' || type == '2')
    return GBK_GIT_ADDED;
  return xy[0] != '.' ? GBK_GIT_STAGED : GBK_GIT_NONE;
}

GbkGitStatus *
gbk_git_parse (const char *buf, size_t len, GError **error)
{
  g_autoptr (GbkGitStatus) s = g_new0 (GbkGitStatus, 1);
  s->paths = g_ptr_array_new_with_free_func (g_free);
  if (len > 0 && buf[len - 1] != '\0')
    return bad_status (error, "unterminated");
  const char *end = buf + len;
  const char *p = buf;
  while (p < end)
    {
      const char *rec = p;
      p += strlen (rec) + 1;          /* every record ends in a NUL (checked above) */
      char type = rec[0];
      if (type != '\0' && rec[1] != ' ')
        return bad_status (error, "record");
      switch (type)
        {
        case '#':
          if (!parse_header (s, rec, error))
            return nullptr;
          break;
        case '1':
        case '2':
        case 'u':
          {
            int fields = type == '1' ? 8 : type == '2' ? 9 : 10;
            const char *path = skip_fields (rec, fields);
            if (path == nullptr || !valid_xy (rec + 2) || !valid_path (path, false))
              return bad_status (error, "changed entry");
            GbkGitState state = changed_state (s, type, rec + 2);
            if (state != GBK_GIT_NONE)
              add_path (s, state, path);
            if (type == '2')
              {
                /* The path it was renamed or copied from: gone from its folder, which
                 * has a staged change now. */
                const char *orig = p;
                if (orig >= end || !valid_path (orig, false))
                  return bad_status (error, "rename");
                p += strlen (orig) + 1;
                add_path (s, GBK_GIT_STAGED, orig);
              }
            break;
          }
        case '?':
        case '!':
          if (!valid_path (rec + 2, true))
            return bad_status (error, "path");
          if (type == '?')
            s->n_untracked++;
          add_path (s, type == '?' ? GBK_GIT_UNTRACKED : GBK_GIT_IGNORED, rec + 2);
          break;
        default: return bad_status (error, "record type");
        }
    }
  return g_steal_pointer (&s);
}

/* ---- what it means for a folder ---- */

GbkGitState
gbk_git_state_in (GHashTable *states, const char *name, GbkGitState otherwise)
{
  void *found;
  if (!g_hash_table_lookup_extended (states, name, nullptr, &found))
    return otherwise;
  unsigned v = GPOINTER_TO_UINT (found);
  if (v >= GBK_GIT_N_STATES)
    gbk_fatal ("bad git state %u", v);
  return (GbkGitState) v;
}

static void
raise_state (GHashTable *t, const char *name, size_t len, GbkGitState state)
{
  g_autofree char *key = g_strndup (name, len);
  if (state > gbk_git_state_in (t, key, GBK_GIT_NONE))
    g_hash_table_insert (t, g_steal_pointer (&key), GUINT_TO_POINTER (state));
}

GHashTable *
gbk_git_folder_states (const GbkGitStatus *s, const char *rel, GbkGitState *inherit)
{
  GHashTable *t = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nullptr);
  g_autofree char *prefix = rel[0] != '\0' ? g_strconcat (rel, "/", nullptr) : g_strdup ("");
  size_t plen = strlen (prefix);
  *inherit = GBK_GIT_NONE;
  for (guint i = 0; i < s->paths->len; i++)
    {
      const GbkGitPath *e = g_ptr_array_index (s->paths, i);
      size_t elen = strlen (e->path);
      bool folder = e->path[elen - 1] == '/';
      if (folder && elen <= plen && strncmp (prefix, e->path, elen) == 0)
        {
          *inherit = e->state; /* this folder is in an untracked or ignored one */
          continue;
        }
      if (strncmp (e->path, prefix, plen) != 0)
        continue;
      const char *rest = e->path + plen;
      const char *slash = strchr (rest, '/');
      if (slash == nullptr)
        raise_state (t, rest, strlen (rest), e->state);
      else if (slash[1] == '\0' || e->state != GBK_GIT_IGNORED)
        raise_state (t, rest, (size_t) (slash - rest), e->state);
      /* else: something ignored deeper down says nothing about the folder holding it */
    }
  return t;
}

GbkGitState
gbk_git_state_of (const GbkGitStatus *s, const char *rel)
{
  if (rel[0] == '\0')
    {
      GbkGitState most = GBK_GIT_NONE;
      for (guint i = 0; i < s->paths->len; i++)
        {
          const GbkGitPath *e = g_ptr_array_index (s->paths, i);
          if (e->state != GBK_GIT_IGNORED && e->state > most)
            most = e->state;
        }
      return most;
    }
  g_autofree char *dir = g_path_get_dirname (rel);
  g_autofree char *name = g_path_get_basename (rel);
  GbkGitState inherit;
  g_autoptr (GHashTable) t = gbk_git_folder_states (s, g_str_equal (dir, ".") ? "" : dir, &inherit);
  return gbk_git_state_in (t, name, inherit);
}

char *
gbk_git_branch (const GbkGitStatus *s)
{
  GString *out = g_string_new ("");
  if (s->head != nullptr)
    {
      g_autofree char *head = g_utf8_make_valid (s->head, -1); /* a name is only bytes */
      g_string_append (out, head);
    }
  else if (s->oid != nullptr)
    g_string_append_printf (out, "detached at %.7s", s->oid);
  else
    g_string_append (out, "detached");
  if (s->oid == nullptr && s->head != nullptr)
    g_string_append (out, " (no commits yet)");
  if (s->has_ab && s->ahead > 0)
    g_string_append_printf (out, " ↑%" G_GUINT64_FORMAT, s->ahead);
  if (s->has_ab && s->behind > 0)
    g_string_append_printf (out, " ↓%" G_GUINT64_FORMAT, s->behind);
  return g_string_free (out, false);
}

char *
gbk_git_summary (const GbkGitStatus *s)
{
  GString *out = g_string_new ("");
  g_autofree char *branch = gbk_git_branch (s);
  g_string_append (out, branch);
  const struct
  {
    unsigned n;
    const char *one, *many;
  } parts[] = {
    { s->n_conflict, "conflict", "conflicts" },
    { s->n_modified, "modified", "modified" },
    { s->n_staged, "staged", "staged" },
    { s->n_untracked, "untracked", "untracked" },
  };
  bool any = false;
  for (size_t i = 0; i < G_N_ELEMENTS (parts); i++)
    if (parts[i].n > 0)
      {
        g_string_append_printf (out, " · %u %s", parts[i].n,
                                parts[i].n == 1 ? parts[i].one : parts[i].many);
        any = true;
      }
  if (!any)
    g_string_append (out, " · clean");
  return g_string_free (out, false);
}

/* ---- finding the work tree ---- */

const char *
gbk_git_relative (const char *path, const char *top)
{
  if (!gbk_path_is_under (path, top))
    gbk_fatal ("a path outside its work tree");
  if (g_str_equal (path, top))
    return "";
  return path + strlen (top) + (g_str_equal (top, "/") ? 0 : 1);
}

bool
gbk_git_fs_refused (int64_t f_type, const char *path)
{
  /* Network, FUSE (sshfs, ntfs-3g, ...) and removable-media filesystems (linux/magic.h). */
  static const uint32_t refused[] = {
    0x6969, /* NFS */
    0x517B, /* SMB */
    0xFF534D42, /* CIFS */
    0xFE534D42, /* SMB2 */
    0x65735546, /* FUSE */
    0x01021997, /* 9P */
    0x5346414F, /* AFS */
    0x00C36400, /* Ceph */
    0x73757245, /* Coda */
    0x564C, /* NCP */
    0x4D44, /* FAT */
    0x2011BAB0, /* exFAT */
    0x5346544E, /* NTFS (old driver) */
    0x7366746E, /* NTFS (ntfs3) */
    0x9660, /* ISO 9660 */
    0x15013346, /* UDF */
    0x4244, /* HFS */
    0x482B, /* HFS+ */
  };
  for (size_t i = 0; i < G_N_ELEMENTS (refused); i++)
    if ((uint32_t) f_type == refused[i])
      return true;
  return gbk_path_is_under (path, "/media") || gbk_path_is_under (path, "/run/media");
}

/* Owned by this user, on a filesystem git may be run on. */
static bool
trusted (const char *path)
{
  struct stat st;
  struct statfs fs;
  return stat (path, &st) == 0 && S_ISDIR (st.st_mode) && st.st_uid == geteuid ()
         && statfs (path, &fs) == 0 && !gbk_git_fs_refused ((int64_t) fs.f_type, path);
}

/* A .git file (a linked work tree or a submodule): "gitdir: <path>". */
static char *
read_gitfile (const char *dir, const char *file)
{
  g_autofree char *text = nullptr;
  size_t len;
  if (!g_file_get_contents (file, &text, &len, nullptr) || len > 4096 || strlen (text) != len
      || !g_str_has_prefix (text, "gitdir: "))
    return nullptr; /* too long, a NUL inside, or not a .git file */
  g_strchomp (text);
  const char *target = text + strlen ("gitdir: ");
  if (target[0] == '\0')
    return nullptr;
  return g_canonicalize_filename (target, dir);
}

bool
gbk_git_find (const char *dir, char **top, char **gitdir)
{
  *top = nullptr;
  *gitdir = nullptr;
  if (dir[0] != '/')
    return false;
  g_auto (GStrv) parts = g_strsplit (dir, "/", -1);
  for (guint i = 0; parts[i] != nullptr; i++)
    if (g_str_equal (parts[i], ".git"))
      return false; /* inside a git directory: not a work tree */
  struct stat start;
  if (stat (dir, &start) != 0)
    return false;
  g_autofree char *d = g_canonicalize_filename (dir, nullptr);
  for (;;)
    {
      struct stat st;
      /* Like git, stop at the edge of the filesystem. */
      if (stat (d, &st) != 0 || st.st_dev != start.st_dev)
        return false;
      g_autofree char *dotgit = g_build_filename (d, ".git", nullptr);
      if (lstat (dotgit, &st) == 0)
        {
          g_autofree char *git = S_ISDIR (st.st_mode)   ? g_strdup (dotgit)
                                 : S_ISREG (st.st_mode) ? read_gitfile (d, dotgit)
                                                        : nullptr;
          if (git == nullptr || !trusted (d) || !trusted (git))
            return false;
          *top = g_steal_pointer (&d);
          *gitdir = g_steal_pointer (&git);
          return true;
        }
      if (g_str_equal (d, "/"))
        return false;
      char *parent = g_path_get_dirname (d);
      g_free (d);
      d = parent;
    }
}

/* ---- running git ---- */

/* This process's environment without GIT_* (a GIT_DIR there would point git elsewhere),
 * plus what keeps git from writing or asking anything. */
static GPtrArray *
git_environment (void)
{
  GPtrArray *env = g_ptr_array_new_with_free_func (g_free);
  for (char **e = environ; *e != nullptr; e++)
    if (!g_str_has_prefix (*e, "GIT_"))
      g_ptr_array_add (env, g_strdup (*e));
  g_ptr_array_add (env, g_strdup ("GIT_OPTIONAL_LOCKS=0"));
  g_ptr_array_add (env, g_strdup ("GIT_TERMINAL_PROMPT=0"));
  g_ptr_array_add (env, nullptr);
  return env;
}

[[nodiscard]] static bool
set_errno_error (GError **error, int err)
{
  g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (err), "%s", g_strerror (err));
  return false;
}

[[nodiscard]] static bool
spawn_git (const char *top, const GPtrArray *args, int out_fd, int err_fd, pid_t *pid,
           GError **error)
{
  g_autoptr (GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);
  static const char *const base[] = {
    "git", "--no-optional-locks", "-c", "core.fsmonitor=false", "-c", "log.showSignature=false",
  };
  for (size_t i = 0; i < G_N_ELEMENTS (base); i++)
    g_ptr_array_add (argv, g_strdup (base[i]));
  for (guint i = 0; i < args->len; i++)
    g_ptr_array_add (argv, g_strdup (g_ptr_array_index (args, i)));
  g_ptr_array_add (argv, nullptr);
  g_autoptr (GPtrArray) env = git_environment ();
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init (&fa);
  posix_spawnattr_init (&attr);
  posix_spawn_file_actions_addopen (&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2 (&fa, out_fd, 1);
  posix_spawn_file_actions_adddup2 (&fa, err_fd, 2);
  posix_spawn_file_actions_addchdir_np (&fa, top);
  /* A session of its own, so one signal ends whatever it started too. */
  sigset_t none, defaults;
  sigemptyset (&none);
  sigemptyset (&defaults);
  sigaddset (&defaults, SIGPIPE);
  posix_spawnattr_setsigmask (&attr, &none);
  posix_spawnattr_setsigdefault (&attr, &defaults);
  posix_spawnattr_setflags (&attr,
                            POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  int err = posix_spawnp (pid, "git", &fa, &attr, (char *const *) argv->pdata,
                          (char *const *) env->pdata);
  posix_spawnattr_destroy (&attr);
  posix_spawn_file_actions_destroy (&fa);
  if (err != 0)
    g_set_error (error, G_IO_ERROR, (int) g_io_error_from_errno (err), "Couldn't run git: %s",
                 g_strerror (err));
  return err == 0;
}

/* Read what's there; false at end of file (or an error). */
static bool
drain (int fd, GByteArray *into, size_t keep_last)
{
  uint8_t buf[65536];
  for (;;)
    {
      ssize_t n = read (fd, buf, sizeof buf);
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && errno == EAGAIN)
        return true;
      if (n <= 0)
        return false;
      g_byte_array_append (into, buf, (guint) n);
      if (keep_last > 0 && into->len > keep_last)
        g_byte_array_remove_range (into, 0, into->len - (guint) keep_last);
    }
}

typedef enum
{
  RUN_DONE,
  RUN_CANCELLED,
  RUN_TIMED_OUT,
  RUN_TOO_BIG,
} RunEnd;

/* Run git in `top` with `args`; its output in *out. */
[[nodiscard]] static bool
run_git (const char *top, const GPtrArray *args, int cancel_fd, int timeout_ms, GByteArray **out,
         GError **error)
{
  int o[2], e[2];
  if (pipe2 (o, O_CLOEXEC) != 0)
    return set_errno_error (error, errno);
  if (pipe2 (e, O_CLOEXEC) != 0)
    {
      int err = errno;
      close (o[0]);
      close (o[1]);
      return set_errno_error (error, err);
    }
  pid_t pid;
  bool started = spawn_git (top, args, o[1], e[1], &pid, error);
  close (o[1]);
  close (e[1]);
  if (!started)
    {
      close (o[0]);
      close (e[0]);
      return false;
    }
  fcntl (o[0], F_SETFL, O_NONBLOCK);
  fcntl (e[0], F_SETFL, O_NONBLOCK);
  g_autoptr (GByteArray) data = g_byte_array_new ();
  g_autoptr (GByteArray) said = g_byte_array_new ();
  int64_t deadline = g_get_monotonic_time () + (int64_t) timeout_ms * 1000;
  bool out_open = true, err_open = true;
  RunEnd end = RUN_DONE;
  int status = 0;
  bool reaped = false;
  while (end == RUN_DONE && !reaped)
    {
      int64_t left_ms = (deadline - g_get_monotonic_time ()) / 1000;
      if (left_ms <= 0)
        {
          end = RUN_TIMED_OUT;
          break;
        }
      struct pollfd p[3] = {
        { .fd = out_open ? o[0] : -1, .events = POLLIN },
        { .fd = err_open ? e[0] : -1, .events = POLLIN },
        { .fd = cancel_fd, .events = POLLIN },
      };
      /* With both pipes closed, only the exit is left to wait for: look now and then. */
      int wait_ms = out_open || err_open ? (int) MIN (left_ms, 1000) : WAIT_POLL_MS;
      int n = poll (p, 3, wait_ms);
      if (n < 0 && errno != EINTR)
        {
          end = RUN_CANCELLED; /* can't wait any more: stop it */
          break;
        }
      if (cancel_fd >= 0 && (p[2].revents & POLLIN))
        end = RUN_CANCELLED;
      if (out_open && (p[0].revents & (POLLIN | POLLHUP | POLLERR)))
        out_open = drain (o[0], data, 0);
      if (err_open && (p[1].revents & (POLLIN | POLLHUP | POLLERR)))
        err_open = drain (e[0], said, STDERR_TAIL);
      if (data->len > GBK_GIT_OUTPUT_MAX)
        end = RUN_TOO_BIG;
      if (!out_open && !err_open && end == RUN_DONE)
        {
          pid_t w = waitpid (pid, &status, WNOHANG);
          if (w == pid)
            reaped = true;
          else if (w < 0 && errno != EINTR)
            {
              int err = errno;
              close (o[0]);
              close (e[0]);
              return set_errno_error (error, err);
            }
        }
    }
  close (o[0]);
  close (e[0]);
  if (!reaped)
    {
      kill (-pid, SIGKILL);
      while (waitpid (pid, &status, 0) < 0 && errno == EINTR)
        ;
    }
  switch (end)
    {
    case RUN_CANCELLED:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return false;
    case RUN_TIMED_OUT:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "git took more than %d s",
                   timeout_ms / 1000);
      return false;
    case RUN_TOO_BIG:
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Too many changes to show");
      return false;
    case RUN_DONE: break;
    default: gbk_fatal ("bad run end %d", (int) end);
    }
  if (!WIFEXITED (status) || WEXITSTATUS (status) != 0)
    {
      /* git's last line says why ("fatal: ..."). */
      g_autofree char *text = g_utf8_make_valid ((const char *) said->data, (gssize) said->len);
      g_strstrip (text);
      const char *last = strrchr (text, '\n');
      last = last != nullptr ? last + 1 : text;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "git failed%s%s",
                   last[0] != '\0' ? ": " : "", last);
      return false;
    }
  *out = g_steal_pointer (&data);
  return true;
}

GbkGitStatus *
gbk_git_status (const char *top, const char *pathspec, int cancel_fd, int timeout_ms,
                GError **error)
{
  g_autoptr (GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
  static const char *const status[] = {
    "status",   "--porcelain=v2",           "-z",
    "--branch", "--untracked-files=normal", "--ignored=traditional",
  };
  for (size_t i = 0; i < G_N_ELEMENTS (status); i++)
    g_ptr_array_add (args, g_strdup (status[i]));
  if (pathspec != nullptr && pathspec[0] != '\0')
    {
      g_ptr_array_add (args, g_strdup ("--"));
      g_ptr_array_add (args, g_strconcat (":(literal)", pathspec, nullptr));
    }
  g_autoptr (GByteArray) out = nullptr;
  if (!run_git (top, args, cancel_fd, timeout_ms, &out, error))
    return nullptr;
  return gbk_git_parse ((const char *) out->data, out->len, error);
}

void
gbk_git_commit_clear (GbkGitCommit *c)
{
  g_clear_pointer (&c->hash, g_free);
  g_clear_pointer (&c->author, g_free);
  g_clear_pointer (&c->subject, g_free);
  c->time = 0;
}

bool
gbk_git_last_commit (const char *top, const char *rel, int timeout_ms, GbkGitCommit *out,
                     GError **error)
{
  memset (out, 0, sizeof *out);
  g_autoptr (GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (args, g_strdup ("log"));
  g_ptr_array_add (args, g_strdup ("-1"));
  g_ptr_array_add (args, g_strdup ("--format=%h%x00%an%x00%at%x00%s"));
  if (rel[0] != '\0')
    {
      g_ptr_array_add (args, g_strdup ("--"));
      g_ptr_array_add (args, g_strconcat (":(literal)", rel, nullptr));
    }
  g_autoptr (GByteArray) text = nullptr;
  if (!run_git (top, args, -1, timeout_ms, &text, error))
    return false;
  if (text->len == 0)
    return false; /* no commit changed it */
  g_byte_array_append (text, (const uint8_t *) "", 1);
  const char *p = (const char *) text->data;
  const char *end = p + text->len - 1;
  const char *fields[4];
  for (int i = 0; i < 4; i++)
    {
      if (p > end)
        return bad (error, "log");
      fields[i] = p;
      p += strlen (p) + 1;
    }
  uint64_t t;
  /* Up to the year 9999, so it converts to nanoseconds without overflowing. */
  if (!g_ascii_string_to_unsigned (fields[2], 10, 0, UINT64_C (253402300799), &t, nullptr))
    return bad (error, "commit time");
  out->hash = g_utf8_make_valid (fields[0], -1);
  out->author = g_utf8_make_valid (fields[1], -1);
  out->time = (int64_t) t;
  out->subject = g_utf8_make_valid (fields[3], -1);
  g_strchomp (out->subject);
  return true;
}

void
gbk_git_file_info_clear (GbkGitFileInfo *info)
{
  g_clear_pointer (&info->error, g_free);
  g_clear_pointer (&info->branch, g_free);
  gbk_git_commit_clear (&info->commit);
  info->known = false;
  info->state = GBK_GIT_NONE;
}

void
gbk_git_file_info (const char *path, bool folder, GbkGitFileInfo *out)
{
  memset (out, 0, sizeof *out);
  g_autofree char *abs = g_canonicalize_filename (path, nullptr);
  g_autofree char *dir = folder ? g_strdup (abs) : g_path_get_dirname (abs);
  g_autofree char *top = nullptr, *gitdir = nullptr;
  if (!gbk_git_find (dir, &top, &gitdir))
    return;
  out->known = true;
  const char *rel = gbk_git_relative (abs, top);
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkGitStatus) s = gbk_git_status (top, rel, -1, GBK_GIT_INFO_TIMEOUT_MS, &error);
  if (s == nullptr)
    {
      out->known = !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND); /* no git */
      out->error = out->known ? g_strdup (error->message) : nullptr;
      return;
    }
  out->state = gbk_git_state_of (s, rel);
  out->branch = gbk_git_branch (s);
  /* No commit to show for something untracked or ignored, or before the first commit. */
  if (s->oid != nullptr && out->state != GBK_GIT_UNTRACKED && out->state != GBK_GIT_IGNORED
      && !gbk_git_last_commit (top, rel, GBK_GIT_INFO_TIMEOUT_MS, &out->commit, &error)
      && error != nullptr)
    out->error = g_strdup (error->message);
}
