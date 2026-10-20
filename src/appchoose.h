/* Choosing an application to open files with: GTK's own chooser (GtkAppChooserDialog). The
 * window offers it as "Other Application…" under Open With, and in place of an error when a
 * file has no default application. It lists the recommended applications and, a button
 * away, every installed one.
 *
 * For files of one type, a tick box (off at first) makes the choice that type's default.
 * Either way the choice joins the type's recommended applications, so Open With lists it
 * next time (as GNOME and Xfce do). Asynchronous (no nested main loop); main thread only. */
#pragma once

/* What the chosen application gets used on. */
typedef void (*GbkAppChosen) (GAppInfo *app, void *data);

/* Ask which application opens `what` (for the heading: "notes.txt", "3 files"), files of
 * type `mime` (nullptr: of several types; no tick box then). The dialog is modal on
 * `parent` and goes when `parent` does. `done` runs with the choice, only if one is made;
 * `destroy` (may be nullptr) frees `data` in every case. */
void gbk_appchoose (GtkWindow *parent, const char *what, const char *mime, GbkAppChosen done,
                    void *data, GDestroyNotify destroy);

/* Tests: the application a choice made in the dialog stands for (instead of the selected
 * one, which a test can't click); nullptr to stop. Referenced. */
void gbk_appchoose_set_pick_for_test (GAppInfo *app);
