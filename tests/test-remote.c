/* The connection registry and its monitors, on a private session bus where this test plays
 * logind and NetworkManager. Connections go through tests/fake-ssh, as in test-conn. */
#include "lifetime.h"
#include "remote.h"

static GbkTypeTable *types;
static char *home;

static bool
wait_until (bool (*cond) (void *), void *data, int ms)
{
  int64_t deadline = g_get_monotonic_time () + (int64_t) ms * 1000;
  while (!cond (data))
    {
      if (g_get_monotonic_time () > deadline)
        return false;
      g_main_context_iteration (nullptr, false);
      g_usleep (1000);
    }
  return true;
}

static bool
ready (void *c)
{
  return gbk_conn_state (c) == GBK_CONN_READY;
}

static bool
dead (void *c)
{
  return gbk_conn_state (c) == GBK_CONN_DEAD;
}

static bool
no_conns (void *data G_GNUC_UNUSED)
{
  return gbk_conn_live () == 0;
}

/* Play a system service: own its name on the test bus. */
static GDBusConnection *
service (const char *name)
{
  g_autoptr (GError) error = nullptr;
  GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, nullptr, &error);
  g_assert_no_error (error);
  g_autoptr (GVariant) r = g_dbus_connection_call_sync (
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
      g_variant_new ("(su)", name, 4u /* DO_NOT_QUEUE */), G_VARIANT_TYPE ("(u)"),
      G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  g_assert_no_error (error);
  guint32 result;
  g_variant_get (r, "(u)", &result);
  g_assert_cmpuint (result, ==, 1);      /* primary owner */
  return bus;
}

typedef struct
{
  GDBusConnection *bus;
  const char *path, *iface, *member;
  const char *format;
  uint32_t u;
  bool b;
  bool (*done) (void *);
  void *data;
} Emit;

/* Emit until it has its effect: the subscriber gets its bus asynchronously, so the first
 * emissions may go unheard. */
static bool
emit_until (Emit *e, int ms)
{
  int64_t deadline = g_get_monotonic_time () + (int64_t) ms * 1000;
  while (!e->done (e->data))
    {
      if (g_get_monotonic_time () > deadline)
        return false;
      GVariant *p = g_str_equal (e->format, "(b)") ? g_variant_new ("(b)", e->b)
                                                   : g_variant_new ("(u)", e->u);
      g_autoptr (GError) error = nullptr;
      g_assert_true (
          g_dbus_connection_emit_signal (e->bus, nullptr, e->path, e->iface, e->member, p, &error));
      g_assert_no_error (error);
      for (int i = 0; i < 50 && !e->done (e->data); i++)
        {
          g_main_context_iteration (nullptr, false);
          g_usleep (1000);
        }
    }
  return true;
}

static void
test_one_per_host (void)
{
  g_autoptr (GbkRemote) r = gbk_remote_new (types);
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkConn) a = gbk_remote_conn (r, "ws", &error);
  g_assert_no_error (error);
  g_autoptr (GbkConn) b = gbk_remote_conn (r, "ws", &error);
  g_assert_true (a == b);
  g_autoptr (GbkConn) other = gbk_remote_conn (r, "ws2", &error);
  g_assert_true (other != a);
  g_assert_null (gbk_remote_conn (r, "-oProxyCommand=x", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_true (wait_until (ready, a, 20000));

  /* A dead one is replaced. */
  gbk_conn_close (a);
  g_assert_true (wait_until (dead, a, 10000));
  g_autoptr (GError) e2 = nullptr;
  g_autoptr (GbkConn) fresh = gbk_remote_conn (r, "ws", &e2);
  g_assert_true (fresh != a);
  g_assert_true (wait_until (ready, fresh, 20000));
}

static void
test_sleep_closes (void)
{
  g_autoptr (GDBusConnection) logind = service ("org.freedesktop.login1");
  g_autoptr (GbkRemote) r = gbk_remote_new (types);
  g_autoptr (GbkConn) c = gbk_remote_conn (r, "ws", nullptr);
  g_assert_true (wait_until (ready, c, 20000));

  /* Waking up (false) changes nothing; going to sleep (true) closes it. */
  GVariant *awake = g_variant_ref_sink (g_variant_new ("(b)", false));
  g_autoptr (GError) error = nullptr;
  g_assert_true (g_dbus_connection_emit_signal (logind, nullptr, "/org/freedesktop/login1",
                                                "org.freedesktop.login1.Manager", "PrepareForSleep",
                                                awake, &error));
  g_variant_unref (awake);
  for (int i = 0; i < 100; i++)
    g_main_context_iteration (nullptr, false);
  g_assert_cmpint (gbk_conn_state (c), ==, GBK_CONN_READY);

  Emit e = { .bus = logind,
             .path = "/org/freedesktop/login1",
             .iface = "org.freedesktop.login1.Manager",
             .member = "PrepareForSleep",
             .format = "(b)",
             .b = true,
             .done = dead,
             .data = c };
  g_assert_true (emit_until (&e, 10000));
}

static int network_ups;

static void
on_network_up (GbkRemote *r G_GNUC_UNUSED, void *data G_GNUC_UNUSED)
{
  network_ups++;
}

static bool
saw_up (void *want)
{
  return network_ups >= GPOINTER_TO_INT (want);
}

static void
test_network_up (void)
{
  g_autoptr (GDBusConnection) nm = service ("org.freedesktop.NetworkManager");
  g_autoptr (GbkRemote) r = gbk_remote_new (types);
  network_ups = 0;
  gbk_connect (G_OBJECT (r), r, "network-up", G_CALLBACK (on_network_up), nullptr);
  Emit up = { .bus = nm,
              .path = "/org/freedesktop/NetworkManager",
              .iface = "org.freedesktop.NetworkManager",
              .member = "StateChanged",
              .format = "(u)",
              .u = 70,
              .done = saw_up,
              .data = GINT_TO_POINTER (1) };
  g_assert_true (emit_until (&up, 10000));
  /* Still up: no second announcement. Down (20) and up again: one more. */
  GVariant *again = g_variant_ref_sink (g_variant_new ("(u)", 70u));
  g_assert_true (g_dbus_connection_emit_signal (nm, nullptr, "/org/freedesktop/NetworkManager",
                                                "org.freedesktop.NetworkManager", "StateChanged",
                                                again, nullptr));
  g_variant_unref (again);
  for (int i = 0; i < 200; i++)
    {
      g_main_context_iteration (nullptr, false);
      g_usleep (500);
    }
  g_assert_cmpint (network_ups, ==, 1);
  GVariant *d = g_variant_ref_sink (g_variant_new ("(u)", 20u));
  g_assert_true (g_dbus_connection_emit_signal (nm, nullptr, "/org/freedesktop/NetworkManager",
                                                "org.freedesktop.NetworkManager", "StateChanged", d,
                                                nullptr));
  g_variant_unref (d);
  up.data = GINT_TO_POINTER (2);
  g_assert_true (emit_until (&up, 10000));
}

static void
on_scan (GbkScanMsg *msg, void *data G_GNUC_UNUSED)
{
  gbk_scan_msg_free (msg);
}

static void
test_ssh_config (void)
{
  const char text[] = "# my machines\n"
                      "Host ws1 ws2\n"
                      "  HostName 10.0.0.2\n"
                      "host=laptop\n"
                      "  Host *.lan !bad ws1 gw # a comment\n"
                      "Match host x\n"
                      "Host -oProxyCommand=evil\n"
                      "HostName notahostline\n";
  g_autoptr (GPtrArray) hosts = gbk_ssh_config_hosts (text, strlen (text));
  g_assert_cmpuint (hosts->len, ==, 4);
  const char *want[] = { "ws1", "ws2", "laptop", "gw" };
  for (guint i = 0; i < hosts->len; i++)
    g_assert_cmpstr (g_ptr_array_index (hosts, i), ==, want[i]);
}

static void
test_idle_close (void)
{
  gbk_remote_set_idle_for_test (50, 300);
  g_autoptr (GbkRemote) r = gbk_remote_new (types);
  gbk_remote_set_idle_for_test (0, 0);
  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);
  g_autoptr (GbkConn) busy = gbk_remote_conn (r, "ws", nullptr);
  g_autoptr (GbkConn) idle = gbk_remote_conn (r, "ws2", nullptr);
  g_assert_true (wait_until (ready, busy, 20000));
  g_assert_true (wait_until (ready, idle, 20000));
  GbkConnWatch *w = gbk_conn_watch (busy, home, owner, on_scan, nullptr);
  g_assert_true (wait_until (dead, idle, 5000));
  g_assert_cmpint (gbk_conn_state (busy), ==, GBK_CONN_READY);
  gbk_conn_unwatch (w);
  g_assert_true (wait_until (dead, busy, 5000));
}

int
main (int argc, char **argv)
{
  home = g_dir_make_tmp ("gbk-remote-XXXXXX", nullptr);
  g_setenv ("HOME", home, true);
  g_autofree char *fake
      = g_build_filename (g_getenv ("GBK_SOURCE_ROOT"), "tests", "fake-ssh", nullptr);
  g_setenv ("QUOIN_SSH", fake, true);
  g_test_init (&argc, &argv, nullptr);
  /* A private session bus, before any thread starts (it sets the environment). */
  g_autoptr (GTestDBus) bus = g_test_dbus_new (G_TEST_DBUS_NONE);
  g_test_dbus_up (bus);
  gbk_remote_use_session_bus_for_test (true);
  types = gbk_type_builder_finish (gbk_type_builder_new ());
  g_test_add_func ("/remote/one-per-host", test_one_per_host);
  g_test_add_func ("/remote/sleep-closes", test_sleep_closes);
  g_test_add_func ("/remote/network-up", test_network_up);
  g_test_add_func ("/remote/idle-close", test_idle_close);
  g_test_add_func ("/remote/ssh-config", test_ssh_config);
  int status = g_test_run ();
  g_assert_true (wait_until (no_conns, nullptr, 10000));
  gbk_type_table_unref (types);
  g_test_dbus_down (bus);
  if (gbk_lifetime_live () != 0)
    gbk_fatal ("registrations leaked");
  return status;
}
