/* Desktop notifications go to the freedesktop Notifications service with the arguments it
 * takes; a later one with the same key replaces the first (its id), another key doesn't.
 * This test plays the service on a private session bus. */
#include "notify.h"

typedef struct
{
  int calls;
  uint32_t last_replaces;
  char *last_summary;
  char *last_body;
  int32_t last_expire;
  uint8_t last_urgency;
} Seen;

static Seen seen;

static void
on_call (GDBusConnection *bus G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
         const char *path G_GNUC_UNUSED, const char *iface G_GNUC_UNUSED, const char *method,
         GVariant *params, GDBusMethodInvocation *inv, void *data G_GNUC_UNUSED)
{
  g_assert_cmpstr (method, ==, "Notify");
  const char *app, *icon, *summary, *body;
  uint32_t replaces;
  int32_t expire;
  g_autoptr (GVariant) actions = nullptr;
  g_autoptr (GVariant) hints = nullptr;
  g_variant_get (params, "(&su&s&s&s@as@a{sv}i)", &app, &replaces, &icon, &summary, &body, &actions,
                 &hints, &expire);
  g_assert_cmpstr (app, ==, "Quoin");
  seen.calls++;
  seen.last_replaces = replaces;
  g_free (seen.last_summary);
  g_free (seen.last_body);
  seen.last_summary = g_strdup (summary);
  seen.last_body = g_strdup (body);
  seen.last_expire = expire;
  g_assert_true (g_variant_lookup (hints, "urgency", "y", &seen.last_urgency));
  uint32_t id = replaces != 0 ? replaces : (uint32_t) (100 + seen.calls);
  g_dbus_method_invocation_return_value (inv, g_variant_new ("(u)", id));
}

static const char xml[]
    = "<node><interface name='org.freedesktop.Notifications'><method name='Notify'>"
      "<arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='in'/>"
      "<arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='as' direction='in'/>"
      "<arg type='a{sv}' direction='in'/><arg type='i' direction='in'/>"
      "<arg type='u' direction='out'/></method></interface></node>";

static bool
calls_at_least (int n)
{
  int64_t deadline = g_get_monotonic_time () + (int64_t) 10 * G_USEC_PER_SEC;
  while (seen.calls < n && g_get_monotonic_time () < deadline)
    g_main_context_iteration (nullptr, true);
  return seen.calls >= n;
}

/* Let the reply (the notification's id) come back. */
static void
spin (void)
{
  int64_t until = g_get_monotonic_time () + G_USEC_PER_SEC / 2;
  while (g_get_monotonic_time () < until)
    {
      g_main_context_iteration (nullptr, false);
      g_usleep (1000);
    }
}

static void
test_notify (void)
{
  g_autoptr (GError) error = nullptr;
  g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, nullptr, &error);
  g_assert_no_error (error);
  g_autoptr (GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (xml, &error);
  g_assert_no_error (error);
  const GDBusInterfaceVTable vtable = { .method_call = on_call };
  guint reg
      = g_dbus_connection_register_object (bus, "/org/freedesktop/Notifications",
                                           node->interfaces[0], &vtable, nullptr, nullptr, &error);
  g_assert_no_error (error);
  guint name = g_bus_own_name_on_connection (bus, "org.freedesktop.Notifications",
                                             G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr,
                                             nullptr);

  gbk_notify ("a", GBK_NOTIFY_LOW, "Saved x.txt to ws", "<b>&</b>");
  g_assert_true (calls_at_least (1));
  g_assert_cmpuint (seen.last_replaces, ==, 0);
  g_assert_cmpstr (seen.last_summary, ==, "Saved x.txt to ws");
  g_assert_cmpstr (seen.last_body, ==, "&lt;b&gt;&amp;&lt;/b&gt;");   /* text, not markup */
  g_assert_cmpint (seen.last_expire, ==, GBK_NOTIFY_LOW_MS);
  g_assert_cmpuint (seen.last_urgency, ==, 0);
  spin ();                                /* the reply (its id) comes back */

  gbk_notify ("a", GBK_NOTIFY_NORMAL, "Couldn't save x.txt", "why");
  g_assert_true (calls_at_least (2));
  g_assert_cmpuint (seen.last_replaces, ==, 101);   /* the same one, replaced */
  g_assert_cmpint (seen.last_expire, ==, 0);        /* stays until dismissed */
  g_assert_cmpuint (seen.last_urgency, ==, 1);
  spin ();

  gbk_notify ("b", GBK_NOTIFY_LOW, "Saved y.txt to ws", "");
  g_assert_true (calls_at_least (3));
  g_assert_cmpuint (seen.last_replaces, ==, 0);     /* another file: its own */
  spin ();

  g_bus_unown_name (name);
  g_dbus_connection_unregister_object (bus, reg);
  g_free (seen.last_summary);
  g_free (seen.last_body);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_autoptr (GTestDBus) dbus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (dbus);
  g_test_add_func ("/notify/to-the-service", test_notify);
  int status = g_test_run ();
  gbk_notify_shutdown ();
  g_test_dbus_down (dbus);
  return status;
}
