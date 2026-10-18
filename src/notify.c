#include "notify.h"

#define APP_ICON "io.github.odclcslkrpb5k.Quoin"
#define CALL_TIMEOUT_MS 5000

/* Process-wide, and never freed: the replies that arrive later (the bus, each notification's
 * id) touch only this, so none of them can find something gone. */
static struct
{
  bool asked;              /* the bus has been asked for */
  bool none;               /* there is no session bus */
  GDBusConnection *bus;
  GPtrArray *waiting;      /* Note*: sent once the bus arrives */
  GHashTable *ids;         /* key -> the server's id (GUINT_TO_POINTER) */
  GbkNotifyFn test;
} N;

typedef struct
{
  char *key, *summary, *body;
  GbkNotifyKind kind;
} Note;

static void
note_free (Note *n)
{
  g_free (n->key);
  g_free (n->summary);
  g_free (n->body);
  g_free (n);
}

void
gbk_notify_shutdown (void)
{
  N.none = true;
  g_clear_object (&N.bus);
  if (N.waiting != nullptr)
    g_ptr_array_set_size (N.waiting, 0);
}

void
gbk_notify_set_for_test (GbkNotifyFn fn)
{
  N.test = fn;
}

static void
on_shown (GObject *bus, GAsyncResult *res, void *data)
{
  char *key = data;
  g_autoptr (GVariant) reply
      = g_dbus_connection_call_finish (G_DBUS_CONNECTION (bus), res, nullptr);
  uint32_t id = 0;
  if (reply != nullptr)
    g_variant_get (reply, "(u)", &id);
  if (id != 0)
    g_hash_table_replace (N.ids, key, GUINT_TO_POINTER (id));   /* takes the key */
  else
    g_free (key);              /* no server, or it said no: nothing to replace next time */
}

static void
show (const Note *n)
{
  uint32_t replaces = GPOINTER_TO_UINT (g_hash_table_lookup (N.ids, n->key));
  GVariantBuilder hints;
  g_variant_builder_init (&hints, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&hints, "{sv}", "urgency",
                         g_variant_new_byte (n->kind == GBK_NOTIFY_LOW ? 0 : 1));
  g_variant_builder_add (&hints, "{sv}", "desktop-entry", g_variant_new_string ("quoin"));
  g_autofree char *body = g_markup_escape_text (n->body, -1);
  const char *const no_actions[] = { nullptr };
  int expire = n->kind == GBK_NOTIFY_LOW ? GBK_NOTIFY_LOW_MS : 0;   /* 0: until dismissed */
  g_dbus_connection_call (N.bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                          "org.freedesktop.Notifications", "Notify",
                          g_variant_new ("(susss^asa{sv}i)", "Quoin", replaces, APP_ICON,
                                         n->summary, body, no_actions, &hints, expire),
                          G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, nullptr,
                          on_shown, g_strdup (n->key));
}

static void
on_bus (GObject *source G_GNUC_UNUSED, GAsyncResult *res, void *data G_GNUC_UNUSED)
{
  GDBusConnection *bus = g_bus_get_finish (res, nullptr);
  if (N.none)
    {
      g_clear_object (&bus);   /* shut down meanwhile */
      return;
    }
  N.bus = bus;
  N.none = N.bus == nullptr;
  for (guint i = 0; N.bus != nullptr && i < N.waiting->len; i++)
    show (g_ptr_array_index (N.waiting, i));
  g_ptr_array_set_size (N.waiting, 0);
}

void
gbk_notify (const char *key, GbkNotifyKind kind, const char *summary, const char *body)
{
  if (N.test != nullptr)
    {
      N.test (key, kind, summary, body);
      return;
    }
  if (N.none)
    return;
  if (N.ids == nullptr)
    {
      N.ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nullptr);
      N.waiting = g_ptr_array_new_with_free_func ((GDestroyNotify) note_free);
    }
  Note *n = g_new0 (Note, 1);
  n->key = g_strdup (key);
  n->summary = g_strdup (summary);
  n->body = g_strdup (body);
  n->kind = kind;
  if (N.bus != nullptr)
    {
      show (n);
      note_free (n);
      return;
    }
  /* The first one asks for the bus (asynchronously: never a wait on the main loop). */
  g_ptr_array_add (N.waiting, n);
  if (!N.asked)
    {
      N.asked = true;
      g_bus_get (G_BUS_TYPE_SESSION, nullptr, on_bus, nullptr);
    }
}
