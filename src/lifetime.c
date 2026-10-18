#include "lifetime.h"

static int live_count;

int
gbk_lifetime_live (void)
{
  return g_atomic_int_get (&live_count);
}

/* ---- Stall watch ---- */

static gint64 stall_limit_us = -1;    /* -1: GBK_STALL_MS not read yet; 0: off. Main thread. */

static gint64
stall_limit (void)
{
  if (stall_limit_us < 0)
    {
      const char *v = g_getenv ("GBK_STALL_MS");
      guint64 ms = 0;
      if (v != nullptr && !g_ascii_string_to_unsigned (v, 10, 1, 600000, &ms, nullptr))
        gbk_fatal ("GBK_STALL_MS must be a whole number of milliseconds, 1 to 600000");
      stall_limit_us = (gint64) ms * 1000;
    }
  return stall_limit_us;
}

/* `start` from g_get_monotonic_time () before the callback, or 0 when the watch is off. */
static gint64
stall_start (void)
{
  return stall_limit () > 0 ? g_get_monotonic_time () : 0;
}

static void
stall_check (gint64 start, const char *kind, const char *name, const char *detail)
{
  if (start == 0)
    return;
  gint64 took = g_get_monotonic_time () - start;
  if (took > stall_limit_us)
    gbk_fatal ("main loop blocked for %" G_GINT64_FORMAT " ms (GBK_STALL_MS=%" G_GINT64_FORMAT
               ") by %s %s%s",
               took / 1000, stall_limit_us / 1000, kind, name, detail);
}

/* ---- Signals ----
 * A GClosure watched by the owner (g_object_watch_closure) is invalidated when the owner is
 * disposed; GSignal removes a handler whose closure is invalidated. The closure is also
 * dropped when the instance is finalized. Either way it is finalized exactly once, which is
 * where the live count goes down. The watch also holds a ref on the owner while the
 * callback runs, so the owner cannot be freed mid-callback. */

typedef struct
{
  const char *name;       /* the callback's name: a string literal from the macro */
  const char *signal;     /* interned */
  const char *type;       /* the instance's type name: static */
  int depth;              /* a handler can re-enter itself by emitting its own signal */
} Handler;

static void
closure_finalized (void *data, GClosure *closure G_GNUC_UNUSED)
{
  g_free (data);
  g_atomic_int_dec_and_test (&live_count);
}

/* The closure's marshal guards are taken (g_object_watch_closure uses the one pair GLib
 * allows), so the watch times a handler from a meta-marshal around GLib's generic one:
 * slower, so it is installed only while the watch is on. */
static void
timed_marshal (GClosure *closure, GValue *return_value, guint n_param_values,
               const GValue *param_values, void *invocation_hint, void *marshal_data)
{
  Handler *h = marshal_data;
  bool outer = h->depth++ == 0;
  gint64 start = outer ? stall_start () : 0;
  g_cclosure_marshal_generic (closure, return_value, n_param_values, param_values, invocation_hint,
                              nullptr);
  h->depth--;
  if (start != 0)
    {
      g_autofree char *detail = g_strdup_printf (" for \"%s\" on %s", h->signal, h->type);
      stall_check (start, "handler", h->name, detail);
    }
}

gulong
gbk_connect_named (GObject *owner, gpointer instance, const char *signal, GCallback callback,
                   void *data, bool after, const char *name)
{
  g_assert (G_IS_OBJECT (owner));
  g_assert (G_IS_OBJECT (instance));
  g_assert (signal != nullptr);
  g_assert (callback != nullptr);

  GClosure *closure = g_cclosure_new (callback, data, nullptr);
  Handler *h = g_new0 (Handler, 1);
  h->name = name;
  h->signal = g_intern_string (signal);
  h->type = G_OBJECT_TYPE_NAME (instance);
  g_closure_add_finalize_notifier (closure, h, closure_finalized);
  if (stall_limit () > 0)
    g_closure_set_meta_marshal (closure, h, timed_marshal);
  g_atomic_int_inc (&live_count);
  g_object_watch_closure (owner, closure);

  gulong id = g_signal_connect_closure (instance, signal, closure, after);
  if (id == 0)
    gbk_fatal ("gbk_connect: no signal \"%s\" on %s", signal, G_OBJECT_TYPE_NAME (instance));
  return id;
}

/* ---- Sources ----
 * The record is freed only by the source's destroy notify, which GLib defers until any
 * in-progress dispatch has returned, so `r` is valid for the whole of source_dispatch. */

typedef struct
{
  GObject *owner;      /* nullptr once the owner has been disposed */
  GbkSourceFunc fn;    /* exactly one of fn / fd_fn is set */
  GbkFdFunc fd_fn;
  void *data;
  GSource *source;     /* borrowed; valid until source_destroyed */
  const char *name;    /* the callback's name: a string literal from the macro */
} SourceRec;

static void
owner_disposed (void *p, GObject *where_the_object_was G_GNUC_UNUSED)
{
  SourceRec *r = p;
  r->owner = nullptr;            /* the weak ref is already gone; don't unref it later */
  g_source_destroy (r->source);
}

static gboolean
dispatch_common (SourceRec *r, int fd, GIOCondition condition)
{
  g_assert (r->owner != nullptr);  /* a destroyed source is never dispatched */

  /* Hold the owner across the callback: if the callback drops the last other ref, the
   * owner is disposed at our unref below, after the callback has returned. */
  GObject *owner = g_object_ref (r->owner);
  gint64 start = stall_start ();
  bool keep = r->fn != nullptr ? r->fn (r->data) : r->fd_fn (fd, condition, r->data);
  stall_check (start, "source", r->name, "");
  g_object_unref (owner);

  return (keep && r->owner != nullptr) ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean
source_dispatch (void *p)
{
  return dispatch_common (p, -1, 0);
}

static gboolean
fd_source_dispatch (int fd, GIOCondition condition, void *p)
{
  /* G_IO_NVAL: the fd was closed while still watched. Its number may already belong to
   * something else, so carrying on would read the wrong file. Always a lifetime bug. */
  if (condition & G_IO_NVAL)
    gbk_fatal ("fd watch on closed fd %d", fd);
  return dispatch_common (p, fd, condition);
}

static void
source_destroyed (void *p)
{
  SourceRec *r = p;
  if (r->owner != nullptr)
    g_object_weak_unref (r->owner, owner_disposed, r);
  g_free (r);
  g_atomic_int_dec_and_test (&live_count);
}

static guint
add_source (GObject *owner, GSource *source, GbkSourceFunc fn, GbkFdFunc fd_fn, void *data,
            const char *name)
{
  g_assert (G_IS_OBJECT (owner));
  g_assert ((fn != nullptr) != (fd_fn != nullptr));

  SourceRec *r = g_new0 (SourceRec, 1);
  r->owner = owner;
  r->fn = fn;
  r->fd_fn = fd_fn;
  r->data = data;
  r->source = source;
  r->name = name;
  g_atomic_int_inc (&live_count);

  g_object_weak_ref (owner, owner_disposed, r);
  /* A unix fd source calls its callback as GUnixFDSourceFunc; G_SOURCE_FUNC is GLib's
   * sanctioned cast for registering it. */
  g_source_set_callback (source,
                         fd_fn != nullptr ? G_SOURCE_FUNC (fd_source_dispatch) : source_dispatch, r,
                         source_destroyed);
  guint id = g_source_attach (source, nullptr);
  g_source_unref (source);         /* the main context holds it now */
  return id;
}

guint
gbk_idle_add_named (GObject *owner, GbkSourceFunc fn, void *data, const char *name)
{
  return add_source (owner, g_idle_source_new (), fn, nullptr, data, name);
}

guint
gbk_timeout_add_named (GObject *owner, guint interval_ms, GbkSourceFunc fn, void *data,
                       const char *name)
{
  return add_source (owner, g_timeout_source_new (interval_ms), fn, nullptr, data, name);
}

guint
gbk_fd_watch_named (GObject *owner, int fd, GIOCondition condition, GbkFdFunc fn, void *data,
                    const char *name)
{
  g_assert (fd >= 0);
  g_assert (fn != nullptr);
  return add_source (owner, g_unix_fd_source_new (fd, condition), nullptr, fn, data, name);
}

/* ---- Clipboard requests ----
 * GTK always calls the callback eventually (with empty data on failure), but possibly after
 * the requester is gone. A weak ref decides whether `fn` may run. */

typedef struct
{
  GWeakRef owner;
  GbkClipboardFunc fn;
  void *data;
  GDestroyNotify destroy;
  const char *name;
} ClipReq;

static void
clipboard_received (GtkClipboard *clipboard G_GNUC_UNUSED, GtkSelectionData *sd, void *p)
{
  ClipReq *r = p;
  GObject *owner = g_weak_ref_get (&r->owner);
  if (owner != nullptr)
    {
      gint64 start = stall_start ();
      r->fn (sd, r->data);
      stall_check (start, "clipboard callback", r->name, "");
      g_object_unref (owner);
    }
  if (r->destroy != nullptr)
    r->destroy (r->data);
  g_weak_ref_clear (&r->owner);
  g_free (r);
  g_atomic_int_dec_and_test (&live_count);
}

void
gbk_clipboard_request_named (GObject *owner, GtkClipboard *clipboard, GdkAtom target,
                             GbkClipboardFunc fn, void *data, GDestroyNotify destroy,
                             const char *name)
{
  g_assert (G_IS_OBJECT (owner) && fn != nullptr);
  ClipReq *r = g_new0 (ClipReq, 1);
  g_weak_ref_init (&r->owner, owner);
  r->fn = fn;
  r->data = data;
  r->destroy = destroy;
  r->name = name;
  g_atomic_int_inc (&live_count);
  gtk_clipboard_request_contents (clipboard, target, clipboard_received, r);
}

/* ---- D-Bus signals ----
 * The live count covers a subscription while its owner lives (that's what a lifetime bug
 * would leak): it goes down when the owner is disposed. The record itself may live a
 * little longer, on GLib's terms: GDBus may still deliver an emission queued before the
 * unsubscribe and frees the record later, from the main loop; `owner` (cleared at
 * dispose) says whether `fn` may run. The bus arrives asynchronously, through a separate
 * request that the record can let go of if its owner goes first. */

typedef struct DBusGet DBusGet;

typedef struct
{
  GObject *owner;          /* weak: nullptr once disposed */
  DBusGet *get;            /* while the bus is being fetched */
  GDBusConnection *bus;
  guint sub;
  char *sender, *iface, *member, *path;
  GbkDBusSignalFunc fn;
  void *data;
  const char *name;
} DBusRec;

struct DBusGet
{
  DBusRec *rec;            /* nullptr: the owner went, nobody wants the bus any more */
  GCancellable *cancel;
};

static void
dbus_rec_free (void *p)
{
  DBusRec *r = p;
  g_clear_object (&r->bus);
  g_free (r->sender);
  g_free (r->iface);
  g_free (r->member);
  g_free (r->path);
  g_free (r);
}

static void
dbus_emitted (GDBusConnection *bus G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
              const char *path G_GNUC_UNUSED, const char *iface G_GNUC_UNUSED,
              const char *member G_GNUC_UNUSED, GVariant *params, void *p)
{
  const DBusRec *r = p;
  if (r->owner == nullptr)
    return;
  g_object_ref (r->owner);
  gint64 start = stall_start ();
  r->fn (params, r->data);
  stall_check (start, "D-Bus signal", r->name, "");
  g_object_unref (r->owner);
}

static void
dbus_owner_disposed (void *p, GObject *gone G_GNUC_UNUSED)
{
  DBusRec *r = p;
  r->owner = nullptr;
  g_atomic_int_dec_and_test (&live_count);
  if (r->get != nullptr)
    {
      r->get->rec = nullptr;             /* the request completes on its own, and finds no one */
      g_cancellable_cancel (r->get->cancel);
      dbus_rec_free (r);
    }
  else if (r->sub != 0)
    g_dbus_connection_signal_unsubscribe (r->bus, r->sub);   /* GDBus frees it */
  else
    dbus_rec_free (r);                   /* there was no bus: nothing else holds it */
}

static void
dbus_got_bus (GObject *source G_GNUC_UNUSED, GAsyncResult *res, void *p)
{
  DBusGet *get = p;
  GDBusConnection *bus = g_bus_get_finish (res, nullptr);
  DBusRec *r = get->rec;
  g_object_unref (get->cancel);
  g_free (get);
  if (r == nullptr)
    {
      g_clear_object (&bus);
      return;
    }
  r->get = nullptr;
  if (bus == nullptr)
    return;                              /* no such bus here: nothing will arrive */
  r->bus = bus;
  r->sub = g_dbus_connection_signal_subscribe (r->bus, r->sender, r->iface, r->member, r->path,
                                               nullptr, G_DBUS_SIGNAL_FLAGS_NONE, dbus_emitted, r,
                                               dbus_rec_free);
}

void
gbk_dbus_signal_named (GObject *owner, GBusType bus, const char *sender, const char *iface,
                       const char *member, const char *path, GbkDBusSignalFunc fn, void *data,
                       const char *name)
{
  g_assert (G_IS_OBJECT (owner) && fn != nullptr);
  DBusRec *r = g_new0 (DBusRec, 1);
  r->owner = owner;
  r->sender = g_strdup (sender);
  r->iface = g_strdup (iface);
  r->member = g_strdup (member);
  r->path = g_strdup (path);
  r->fn = fn;
  r->data = data;
  r->name = name;
  r->get = g_new0 (DBusGet, 1);
  r->get->rec = r;
  r->get->cancel = g_cancellable_new ();
  g_atomic_int_inc (&live_count);
  g_object_weak_ref (owner, dbus_owner_disposed, r);
  g_bus_get (bus, r->get->cancel, dbus_got_bus, r->get);
}

void
gbk_source_cancel (guint *id)
{
  g_assert (id != nullptr);
  if (*id == 0)
    return;
  /* Look it up rather than calling g_source_remove() blindly: a source that already
   * returned false is gone, and removing a stale id is a GLib critical. */
  GSource *source = g_main_context_find_source_by_id (nullptr, *id);
  if (source != nullptr)
    g_source_destroy (source);
  *id = 0;
}
