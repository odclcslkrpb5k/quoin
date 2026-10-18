/* Properties window for one file or folder: name, location, type, size, link target,
 * dates (modified / accessed / changed / created), owner and group, and the read / write /
 * execute permissions for owner, group and others, changed with Apply.
 *
 * Details are read on a worker (an info job), so a hung network mount can't freeze it.
 * Not modal; closes with its parent window, Escape or Close. */
#pragma once
#include "loc.h"

#define GBK_TYPE_PROPS (gbk_props_get_type ())
G_DECLARE_FINAL_TYPE (GbkProps, gbk_props, GBK, PROPS, GtkWindow)

/* `mime` is the type from the extension table (shown for files). */
GbkProps *gbk_props_new (GtkWindow *parent, GbkLoc *loc, const char *mime);

/* For tests. */
bool gbk_props_loaded (const GbkProps *p);                          /* details are on screen */
const char *gbk_props_value (GbkProps *p,
                             const char *field); /* "Modified" etc.; nullptr if absent */
/* who: 0 owner, 1 group, 2 others; what: 0 read, 1 write, 2 execute. */
GtkToggleButton *gbk_props_permission (GbkProps *p, int who, int what);
GtkButton *gbk_props_apply_button (GbkProps *p);
const char *gbk_props_octal (GbkProps *p);
