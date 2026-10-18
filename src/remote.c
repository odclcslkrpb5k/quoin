#include "remote.h"
#include "lifetime.h"

#define IDLE_CHECK_MS ((int) 60 * 1000)
#define NM_STATE_CONNECTED_GLOBAL 70u

static bool test_session_bus;
static GbkRemote *default_remote;

void
gbk_remote_set_default (GbkRemote *r)
{
  if (r != nullptr)
    g_object_ref (r);
  g_clear_object (&default_remote);
  default_remote = r;
}

GbkRemote *
gbk_remote_default (void)
{
  return default_remote;
}
static int test_check_ms, test_idle_ms;

void
gbk_remote_use_session_bus_for_test (bool session)
{
  test_session_bus = session;
}

void
gbk_remote_set_idle_for_test (int check_ms, int idle_ms)
{
  test_check_ms = check_ms;
  test_idle_ms = idle_ms;
}

struct _GbkRemote
{
  GObject parent_instance;
  GbkTypeTable *types;
  GHashTable *conns;       /* host -> GbkConn* (owned) */
  uint32_t nm_state;
  int idle_ms;
};

G_DEFINE_FINAL_TYPE (GbkRemote, gbk_remote, G_TYPE_OBJECT)

enum
{
  SIGNAL_NETWORK_UP,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

GbkConn *
gbk_remote_conn (GbkRemote *r, const char *host, GError **error)
{
  GbkConn *c = g_hash_table_lookup (r->conns, host);
  if (c != nullptr && gbk_conn_state (c) != GBK_CONN_DEAD)
    return g_object_ref (c);
  c = gbk_conn_new (host, r->types, error);
  if (c == nullptr)
    return nullptr;
  g_hash_table_replace (r->conns, g_strdup (host), c);
  return g_object_ref (c);
}

GPtrArray *
gbk_remote_hosts (GbkRemote *r)
{
  GPtrArray *hosts = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GList) keys = g_hash_table_get_keys (r->conns);
  keys = g_list_sort (keys, (GCompareFunc) g_strcmp0);
  for (GList *l = keys; l != nullptr; l = l->next)
    g_ptr_array_add (hosts, g_strdup (l->data));
  return hosts;
}

GPtrArray *
gbk_ssh_config_hosts (const char *text, size_t len)
{
  GPtrArray *hosts = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *copy = g_strndup (text, len);
  g_auto (GStrv) lines = g_strsplit (copy, "\n", -1);
  for (size_t i = 0; lines[i] != nullptr; i++)
    {
      /* "Host a b c" or "Host=a b", keyword in any case; the rest of the file is ssh's. */
      const char *l = lines[i];
      while (*l == ' ' || *l == '\t')
        l++;
      if (g_ascii_strncasecmp (l, "host", 4) != 0 || (l[4] != ' ' && l[4] != '\t' && l[4] != '='))
        continue;
      /* "Host", then blanks, at most one '=' and blanks, then the names. */
      l += 4;
      l += strspn (l, " \t");
      if (*l == '=')
        l++;
      g_auto (GStrv) words = g_strsplit_set (l, " \t\r", -1);
      for (size_t w = 0; words[w] != nullptr; w++)
        {
          const char *h = words[w];
          if (h[0] == '#')
            break;                       /* a comment to the end of the line */
          bool pattern = strpbrk (h, "*?!") != nullptr;
          bool seen = false;
          for (guint k = 0; k < hosts->len && !seen; k++)
            seen = g_str_equal (g_ptr_array_index (hosts, k), h);
          if (h[0] != '\0' && !pattern && !seen && gbk_host_valid (h))
            g_ptr_array_add (hosts, g_strdup (h));
        }
    }
  return hosts;
}

GbkConn *
gbk_remote_peek (GbkRemote *r, const char *host)
{
  return g_hash_table_lookup (r->conns, host);
}

void
gbk_remote_close_all (GbkRemote *r)
{
  GHashTableIter it;
  void *c;
  g_hash_table_iter_init (&it, r->conns);
  while (g_hash_table_iter_next (&it, nullptr, &c))
    gbk_conn_close (c);
}

static void
on_prepare_for_sleep (GVariant *params, void *data)
{
  gboolean going = false;
  if (g_variant_is_of_type (params, G_VARIANT_TYPE ("(b)")))
    g_variant_get (params, "(b)", &going);
  if (going)
    gbk_remote_close_all (data);
}

static void
on_nm_state (GVariant *params, void *data)
{
  GbkRemote *r = data;
  if (!g_variant_is_of_type (params, G_VARIANT_TYPE ("(u)")))
    return;
  uint32_t state;
  g_variant_get (params, "(u)", &state);
  bool was_up = r->nm_state >= NM_STATE_CONNECTED_GLOBAL;
  r->nm_state = state;
  if (state >= NM_STATE_CONNECTED_GLOBAL && !was_up)
    g_signal_emit (r, signals[SIGNAL_NETWORK_UP], 0);
}

/* Close connections nothing has used for a while; forget dead ones. */
static bool
check_idle (void *data)
{
  GbkRemote *r = data;
  int64_t now = g_get_monotonic_time ();
  GHashTableIter it;
  void *c;
  g_hash_table_iter_init (&it, r->conns);
  while (g_hash_table_iter_next (&it, nullptr, &c))
    {
      int64_t since = gbk_conn_unused_since (c);
      if (gbk_conn_state (c) == GBK_CONN_DEAD && since != 0)
        g_hash_table_iter_remove (&it);
      else if (since != 0 && now - since >= (int64_t) r->idle_ms * 1000)
        gbk_conn_close (c);
    }
  return true;
}

GbkRemote *
gbk_remote_new (GbkTypeTable *types)
{
  GbkRemote *r = g_object_new (GBK_TYPE_REMOTE, nullptr);
  r->types = gbk_type_table_ref (types);
  r->idle_ms = test_idle_ms > 0 ? test_idle_ms : GBK_REMOTE_IDLE_MS;
  GBusType bus = test_session_bus ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
  gbk_dbus_signal (G_OBJECT (r), bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
                   "PrepareForSleep", "/org/freedesktop/login1", on_prepare_for_sleep, r);
  gbk_dbus_signal (G_OBJECT (r), bus, "org.freedesktop.NetworkManager",
                   "org.freedesktop.NetworkManager", "StateChanged",
                   "/org/freedesktop/NetworkManager", on_nm_state, r);
  gbk_timeout_add (G_OBJECT (r), (guint) (test_check_ms > 0 ? test_check_ms : IDLE_CHECK_MS),
                   check_idle, r);
  return r;
}

static void
gbk_remote_dispose (GObject *o)
{
  GbkRemote *r = GBK_REMOTE (o);
  if (r->conns != nullptr)
    gbk_remote_close_all (r);
  g_clear_pointer (&r->conns, g_hash_table_unref);
  G_OBJECT_CLASS (gbk_remote_parent_class)->dispose (o);
}

static void
gbk_remote_finalize (GObject *o)
{
  gbk_type_table_unref (GBK_REMOTE (o)->types);
  G_OBJECT_CLASS (gbk_remote_parent_class)->finalize (o);
}

static void
gbk_remote_class_init (GbkRemoteClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = gbk_remote_dispose;
  G_OBJECT_CLASS (klass)->finalize = gbk_remote_finalize;
  signals[SIGNAL_NETWORK_UP]
      = g_signal_new ("network-up", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                      nullptr, nullptr, G_TYPE_NONE, 0);
}

static void
gbk_remote_init (GbkRemote *r)
{
  r->conns = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}
