/* Owner-scoped signal connections and main-loop sources.
 *
 * Every callback registered here has an owner GObject. When the owner is disposed
 * (gtk_widget_destroy, or its last unref), every callback it owns is disconnected or
 * removed before the owner's memory can go away, so a callback can never run with a
 * dangling owner/data pointer. Signal connections also end when the emitting instance
 * is finalized.
 *
 * The raw GLib/GTK equivalents are poisoned (prelude.h); this is the only way in.
 * Main thread only. */
#pragma once

/* Return true to keep the source, false to remove it. */
typedef bool (*GbkSourceFunc) (void *data);
typedef bool (*GbkFdFunc) (int fd, GIOCondition condition, void *data);

/* Each registration is a macro passing the callback's name as `name`, so the stall watch
 * (below) can say which callback blocked the main loop. Call the macros, not the *_named
 * functions. */

/* Connect `callback` to `signal` on `instance`. `data` is passed as the handler's user_data.
 * Returns the handler id (never 0; an invalid signal name is a fatal programming error). */
#define gbk_connect(owner, instance, signal, callback, data) \
  gbk_connect_named (owner, instance, signal, callback, data, false, #callback)
#define gbk_connect_after(owner, instance, signal, callback, data) \
  gbk_connect_named (owner, instance, signal, callback, data, true, #callback)
gulong gbk_connect_named (GObject *owner, gpointer instance, const char *signal, GCallback callback,
                          void *data, bool after, const char *name);

/* Run `fn(data)` when idle / every `interval_ms`, until it returns false or `owner` is
 * disposed. Returns a source id usable with gbk_source_cancel(). */
#define gbk_idle_add(owner, fn, data) gbk_idle_add_named (owner, fn, data, #fn)
#define gbk_timeout_add(owner, interval_ms, fn, data) \
  gbk_timeout_add_named (owner, interval_ms, fn, data, #fn)
guint gbk_idle_add_named (GObject *owner, GbkSourceFunc fn, void *data, const char *name);
guint gbk_timeout_add_named (GObject *owner, guint interval_ms, GbkSourceFunc fn, void *data,
                             const char *name);

/* Call `fn` whenever `fd` meets `condition`, until it returns false or `owner` is disposed.
 * Does not take ownership of `fd`, which must stay open while the watch exists. The watch
 * is removed when the owner's dispose chains up to GObject's, so a subclass dispose that
 * closes the fd *before* chaining up must gbk_source_cancel() the watch first. Closing it
 * in finalize is always safe. */
#define gbk_fd_watch(owner, fd, condition, fn, data) \
  gbk_fd_watch_named (owner, fd, condition, fn, data, #fn)
guint gbk_fd_watch_named (GObject *owner, int fd, GIOCondition condition, GbkFdFunc fn, void *data,
                          const char *name);

/* Request clipboard contents for `target`. `fn` runs only if `owner` is still alive when
 * the (asynchronous) answer arrives. `destroy` (may be nullptr) is always called on `data`
 * afterwards, whether or not `fn` ran. `sd` may hold no data (empty / unsupported). */
typedef void (*GbkClipboardFunc) (GtkSelectionData *sd, void *data);
#define gbk_clipboard_request(owner, clipboard, target, fn, data, destroy) \
  gbk_clipboard_request_named (owner, clipboard, target, fn, data, destroy, #fn)
void gbk_clipboard_request_named (GObject *owner, GtkClipboard *clipboard, GdkAtom target,
                                  GbkClipboardFunc fn, void *data, GDestroyNotify destroy,
                                  const char *name);

/* Follow a D-Bus signal: `fn` gets each emission's parameters while `owner` lives. Getting
 * the bus is asynchronous; with no bus (or none of that type) nothing ever arrives. The
 * subscription ends when the owner is disposed. `sender`, `path` may be nullptr (any). */
typedef void (*GbkDBusSignalFunc) (GVariant *params, void *data);
#define gbk_dbus_signal(owner, bus, sender, iface, member, path, fn, data) \
  gbk_dbus_signal_named (owner, bus, sender, iface, member, path, fn, data, #fn)
void gbk_dbus_signal_named (GObject *owner, GBusType bus, const char *sender, const char *iface,
                            const char *member, const char *path, GbkDBusSignalFunc fn, void *data,
                            const char *name);

/* The stall watch. While one of the callbacks registered here runs, the main loop can't
 * repaint or take input: a frozen window. With GBK_STALL_MS set (the tests set it), a
 * callback that runs longer than that many milliseconds aborts, naming itself. Every
 * callback of ours is registered here (the raw APIs are poisoned elsewhere), so this times
 * all of our code on the main loop; GTK's own handlers are not timed. Unset, it costs a
 * branch per callback. */

/* Cancel a source early and zero `*id`. Safe if the source already finished or `*id` is 0. */
void gbk_source_cancel (guint *id);

/* Number of live registrations (connections + sources). Tests assert this returns to 0. */
int gbk_lifetime_live (void);
