/* A real OpenSSH: sshd on 127.0.0.1 as this (non-root) user, with its own keys and config,
 * and nss_wrapper giving the container's nameless uid a passwd entry (ssh and sshd both
 * look the user up). Proves the real ssh takes every option conn.c forces, that sending
 * quoin-serve over works through a real login shell, and that a wrong host key and a
 * refused key end the connection asking for a person. */
#include "conn.h"
#include "dirmodel.h"

static char *dir;
static char *home;
static GbkTypeTable *types;
static GPid sshd_pid;
static int port;

static char *
in_dir (const char *name)
{
  return g_build_filename (dir, name, nullptr);
}

static void
write_file (const char *name, const char *content, int mode)
{
  g_autofree char *p = in_dir (name);
  g_assert_true (g_file_set_contents (p, content, -1, nullptr));
  g_assert_cmpint (chmod (p, (mode_t) mode), ==, 0);
}

static void
run (const char *const *argv)
{
  g_autoptr (GStrvBuilder) b = g_strv_builder_new ();
  for (size_t i = 0; argv[i] != nullptr; i++)
    g_strv_builder_add (b, argv[i]);
  g_auto (GStrv) v = g_strv_builder_end (b);
  int status = 0;
  g_autoptr (GError) error = nullptr;
  g_assert_true (g_spawn_sync (nullptr, v, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr,
                               nullptr, &status, &error));
  g_assert_no_error (error);
  g_assert_true (g_spawn_check_wait_status (status, nullptr));
}

static char *
read_file (const char *name)
{
  g_autofree char *p = in_dir (name);
  char *text = nullptr;
  g_assert_true (g_file_get_contents (p, &text, nullptr, nullptr));
  g_strstrip (text);
  return text;
}

/* A port nothing listens on (bound and released: a small race, harmless here). */
static int
free_port (void)
{
  int fd = socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint (fd, >=, 0);
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl (INADDR_LOOPBACK) };
  g_assert_cmpint (bind (fd, (struct sockaddr *) (void *) &a, sizeof a), ==, 0);
  socklen_t len = sizeof a;
  g_assert_cmpint (getsockname (fd, (struct sockaddr *) (void *) &a, &len), ==, 0);
  close (fd);
  return ntohs (a.sin_port);
}

static bool
listening (void)
{
  int fd = socket (AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint (fd, >=, 0);
  struct sockaddr_in a = { .sin_family = AF_INET,
                           .sin_port = htons ((uint16_t) port),
                           .sin_addr.s_addr = htonl (INADDR_LOOPBACK) };
  bool ok = connect (fd, (struct sockaddr *) (void *) &a, sizeof a) == 0;
  close (fd);
  return ok;
}

static char *
nss_env (void)
{
  g_autofree char *passwd = in_dir ("passwd");
  g_autofree char *group = in_dir ("group");
  return g_strdup_printf ("LD_PRELOAD=libnss_wrapper.so NSS_WRAPPER_PASSWD=%s "
                          "NSS_WRAPPER_GROUP=%s",
                          passwd, group);
}

static void
start_sshd (void)
{
  dir = g_dir_make_tmp ("gbk-sshd-XXXXXX", nullptr);
  home = in_dir ("home");
  g_mkdir_with_parents (home, 0700);
  g_autofree char *passwd
      = g_strdup_printf ("tester:x:%u:%u:tester:%s:/bin/sh\n", getuid (), getgid (), home);
  g_autofree char *group = g_strdup_printf ("tester:x:%u:\n", getgid ());
  write_file ("passwd", passwd, 0644);
  write_file ("group", group, 0644);
  for (const char *const *k
       = (const char *const[]) { "host_key", "client_key", "other_key", nullptr };
       *k != nullptr; k++)
    {
      g_autofree char *p = in_dir (*k);
      run ((const char *const[]) { "ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", p,
                                   nullptr });
    }
  g_autofree char *client_pub = read_file ("client_key.pub");
  g_autofree char *auth = g_strconcat (client_pub, "\n", nullptr);
  write_file ("authorized_keys", auth, 0600);

  port = free_port ();
  g_autofree char *host_key = in_dir ("host_key");
  g_autofree char *auth_path = in_dir ("authorized_keys");
  g_autofree char *sshd_config = g_strdup_printf ("ListenAddress 127.0.0.1:%d\n"
                                                  "HostKey %s\n"
                                                  "AuthorizedKeysFile %s\n"
                                                  "PidFile none\n"
                                                  "UsePAM no\n"
                                                  "StrictModes no\n"
                                                  "PasswordAuthentication no\n"
                                                  "KbdInteractiveAuthentication no\n",
                                                  port, host_key, auth_path);
  write_file ("sshd_config", sshd_config, 0600);
  g_autofree char *host_pub = read_file ("host_key.pub");
  g_autofree char *known = g_strdup_printf ("[127.0.0.1]:%d %s\n", port, host_pub);
  write_file ("known_hosts", known, 0600);
  g_autofree char *other_pub = read_file ("other_key.pub");
  g_autofree char *wrong = g_strdup_printf ("[127.0.0.1]:%d %s\n", port, other_pub);
  write_file ("wrong_known_hosts", wrong, 0600);

  /* Two aliases: "ws" as it should be, "wrong-key" expecting another host key. */
  g_autofree char *client_key = in_dir ("client_key");
  g_autofree char *known_path = in_dir ("known_hosts");
  g_autofree char *wrong_path = in_dir ("wrong_known_hosts");
  g_autofree char *other_key = in_dir ("other_key");
  g_autofree char *ssh_config = g_strdup_printf (
      "Host ws wrong-key refused\n  HostName 127.0.0.1\n  Port %d\n  User tester\n"
      "  GlobalKnownHostsFile /dev/null\n  IdentitiesOnly yes\n"
      "Host ws\n  IdentityFile %s\n  UserKnownHostsFile %s\n"
      "Host wrong-key\n  IdentityFile %s\n  UserKnownHostsFile %s\n"
      "Host refused\n  IdentityFile %s\n  UserKnownHostsFile %s\n",
      port, client_key, known_path, client_key, wrong_path, other_key, known_path);
  write_file ("ssh_config", ssh_config, 0600);

  g_autofree char *nss = nss_env ();
  g_autofree char *config_path = in_dir ("sshd_config");
  g_autofree char *log = in_dir ("sshd.log");
  g_autofree char *cmd
      = g_strdup_printf ("exec env %s /usr/sbin/sshd -D -e -f '%s' 2>'%s'", nss, config_path, log);
  char *argv[] = { (char[]) { "/bin/sh" }, (char[]) { "-c" }, cmd, nullptr };
  g_autoptr (GError) error = nullptr;
  g_assert_true (g_spawn_async (nullptr, argv, nullptr, G_SPAWN_DO_NOT_REAP_CHILD, nullptr, nullptr,
                                &sshd_pid, &error));
  g_assert_no_error (error);
  for (int i = 0; i < 500 && !listening (); i++)
    g_usleep (G_USEC_PER_SEC / 100);
  g_assert_true (listening ());

  /* Quoin runs the real ssh, with our client config and the passwd entry. */
  g_autofree char *ssh_config_path = in_dir ("ssh_config");
  g_autofree char *quoin_ssh = g_strdup_printf ("env %s ssh -F '%s'", nss, ssh_config_path);
  g_setenv ("QUOIN_SSH", quoin_ssh, true);
}

static bool
settled (void *c)
{
  return gbk_conn_state (c) != GBK_CONN_CONNECTING;
}

static bool
dead (void *c)
{
  return gbk_conn_state (c) == GBK_CONN_DEAD;
}

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

typedef struct
{
  GbkDirModel *m;
} Watching;

static void
on_scan (GbkScanMsg *msg, void *data)
{
  if (msg != nullptr)
    gbk_dir_model_apply_for_test (((Watching *) data)->m, msg);
}

static bool
listed (void *data)
{
  return gbk_dir_model_state (((Watching *) data)->m) == GBK_DIR_READY;
}

static GbkConn *
connect_to (const char *host)
{
  g_autoptr (GError) error = nullptr;
  GbkConn *c = gbk_conn_new (host, types, &error);
  g_assert_no_error (error);
  g_assert_true (wait_until (settled, c, 30000));
  return c;
}

static void
test_real_ssh (void)
{
  g_autofree char *marker = g_build_filename (home, "on-the-other-side", nullptr);
  g_assert_true (g_file_set_contents (marker, "", -1, nullptr));

  GbkConn *c = connect_to ("ws");
  g_assert_cmpstr (gbk_conn_error (c), ==, nullptr);
  g_assert_cmpint (gbk_conn_state (c), ==, GBK_CONN_READY);
  g_assert_cmpstr (gbk_conn_hello (c)->home, ==, home);   /* sshd's HOME, from passwd */
  g_autofree char *bin
      = g_build_filename (home, ".cache", "quoin", "serve", GBK_VERSION, "quoin-serve", nullptr);
  g_assert_true (g_file_test (bin, G_FILE_TEST_IS_EXECUTABLE));

  g_autoptr (GObject) owner = g_object_new (G_TYPE_OBJECT, nullptr);
  g_autoptr (GbkDirModel) m = gbk_dir_model_new_detached (types);
  Watching w = { m };
  GbkConnWatch *watch = gbk_conn_watch (c, home, owner, on_scan, &w);
  g_assert_true (wait_until (listed, &w, 20000));
  g_assert_cmpuint (gbk_dir_model_find (m, "on-the-other-side"), !=, GBK_HANDLE_NONE);
  gbk_conn_unwatch (watch);
  gbk_conn_close (c);
  g_assert_true (wait_until (dead, c, 10000));
  g_object_unref (c);
}

static void
test_real_refusals (void)
{
  GbkConn *wrong = connect_to ("wrong-key");
  g_assert_cmpint (gbk_conn_state (wrong), ==, GBK_CONN_DEAD);
  g_test_message ("wrong host key: %s", gbk_conn_error (wrong));
  g_assert_true (gbk_conn_needs_login (wrong));
  g_object_unref (wrong);

  GbkConn *refused = connect_to ("refused");
  g_assert_cmpint (gbk_conn_state (refused), ==, GBK_CONN_DEAD);
  g_test_message ("refused key: %s", gbk_conn_error (refused));
  g_assert_nonnull (strstr (gbk_conn_error (refused), "Permission denied"));
  g_assert_true (gbk_conn_needs_login (refused));
  g_object_unref (refused);
}

static bool
no_conns (void *data G_GNUC_UNUSED)
{
  return gbk_conn_live () == 0;
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  start_sshd ();
  types = gbk_type_builder_finish (gbk_type_builder_new ());
  g_test_add_func ("/sshd/real-ssh", test_real_ssh);
  g_test_add_func ("/sshd/real-refusals", test_real_refusals);
  int status = g_test_run ();
  g_assert_true (wait_until (no_conns, nullptr, 10000));
  kill (sshd_pid, SIGTERM);
  waitpid (sshd_pid, nullptr, 0);
  g_spawn_close_pid (sshd_pid);
  gbk_type_table_unref (types);
  return status;
}
