/* The file clipboard (Ctrl+C / Ctrl+X / Ctrl+V), interoperable with other file managers. */
#pragma once

#include "loc.h"

/* Put `locs` (GbkLoc*) on the CLIPBOARD as a copy or a cut. Other Quoin windows get them
 * all; other apps get file:// URIs only when every one is on this machine (else just
 * host:/path text). Returns false only if it couldn't be done. */
[[nodiscard]] bool gbk_clipboard_set_files (GtkWidget *widget, const GPtrArray *locs, bool cut);

/* Put plain text on the CLIPBOARD (replacing any files we put there). */
void gbk_clipboard_set_text (GtkWidget *widget, const char *text);

/* Read files from the CLIPBOARD (asynchronously). `fn` runs only if `owner` is still alive;
 * `locs` (GbkLoc*; from another Quoin window possibly on other machines) is empty if the
 * clipboard holds no files. */
typedef void (*GbkFilesFunc) (GPtrArray *locs, bool cut, void *data);
void gbk_clipboard_get_files (GObject *owner, GtkWidget *widget, GbkFilesFunc fn, void *data);

/* After a cut has been pasted: clear the clipboard if we still own it. */
void gbk_clipboard_clear_if_ours (GtkWidget *widget);
