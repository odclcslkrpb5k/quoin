/* User configuration: ~/.config/quoin/quoin.conf, GLib key-file format.
 *
 * Strict by design: unknown sections or keys, bad values, and bad command templates are
 * errors. Any error rejects the whole file (defaults are used) and every problem is
 * reported, so a typo can never half-apply. See docs/example.conf. */
#pragma once

#include "command.h"
#include "dirmodel.h"
#include "filetype.h"

typedef struct
{
  char *id;             /* from [Action <id>] */
  char *name;           /* menu label */
  char **types; /* MIME patterns (e.g. image/png, image/<any>, or a lone star); may be empty */
  bool directories; /* also offered for folders */
  GbkCommand *command; /* placeholders: d f F */
  /* For files on another machine, run there (no display there, so not a GUI program);
   * nullptr: the action isn't offered for them. Placeholders: d f F, as paths there. */
  GbkCommand *remote_command;
} GbkAction;

typedef struct
{
  GbkCommand *terminal; /* F4: open a terminal here. Placeholders: d */
  GbkCommand *terminal_run; /* run something in a terminal. Placeholders: d c */
  bool show_hidden;
  bool full_path_title;  /* the window title is the folder's full path, or only its name */
  GbkSortMode sort;
  GHashTable *interpreters; /* lowercased ext -> char** argv prefix; marks scripts */
  GPtrArray *type_overrides; /* char* pairs: ext, mime, ext, mime, ... */
  GPtrArray *actions; /* GbkAction*, in file order */
} GbkConfig;

/* Built-in defaults. */
[[nodiscard]] GbkConfig *gbk_config_new_default (void);

/* Parse key-file data. On any error, returns the defaults and fills `errors` (char*
 * messages, never empty in that case). `errors` may be nullptr. */
[[nodiscard]] GbkConfig *gbk_config_load_data (const char *data, size_t len, GPtrArray *errors);

/* Load from a path. A missing file is not an error: defaults. */
[[nodiscard]] GbkConfig *gbk_config_load_file (const char *path, GPtrArray *errors);

/* $XDG_CONFIG_HOME/quoin/quoin.conf. If only the file from before the rename exists
 * (gbk-file/gbk-file.conf), it's moved there first; if that fails, its path is returned. */
char *gbk_config_default_path (void);
char *gbk_config_path_in (const char *config_home); /* the same, under `config_home` */

void gbk_config_free (GbkConfig *c);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GbkConfig, gbk_config_free)

/* Interpreter argv prefix for a filename's extension (longest match, case-insensitive), or
 * nullptr if it isn't a script. */
const char *const *gbk_config_interpreter (const GbkConfig *c, const char *name);

/* Apply [Types] overrides to a type table builder. */
void gbk_config_apply_types (const GbkConfig *c, GbkTypeBuilder *b);

/* Does `action` apply to an entry of this MIME type / kind? */
bool gbk_action_matches (const GbkAction *a, const char *mime, bool is_dir);
