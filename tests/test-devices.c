/* Devices: the ☰ menu's Devices list and its open / mount / unmount / eject, against a fake
 * volume monitor whose "mounts" are ordinary folders (under Xvfb). */
#include "lifetime.h"
#include "scanner.h"
#include "window.h"

/* Windows and tabs on local folders, by path. */
static const char *
wpath (GbkWindow *w)
{
  const char *p;
  g_assert_true (gbk_loc_as_local (gbk_window_loc (w), &p));
  return p;
}

[[maybe_unused]] static const char *
tab_path (GbkWindow *w, int i)
{
  const char *p;
  g_assert_true (gbk_loc_as_local (gbk_window_tab_loc (w, i), &p));
  return p;
}

static GbkWindow *
window_at (const GtkApplication *app, GbkContext *ctx, const char *path)
{
  g_autoptr (GbkLoc) loc = gbk_loc_new_local (path);
  return gbk_window_new (app, ctx, loc);
}

static void
open_at (GbkWindow *w, const char *path)
{
  g_autoptr (GbkLoc) loc = gbk_loc_new_local (path);
  gbk_window_open (w, loc);
}

[[maybe_unused]] static void
new_tab_at (GbkWindow *w, const char *path)
{
  g_autoptr (GbkLoc) loc = gbk_loc_new_local (path);
  gbk_window_new_tab (w, loc);
}

static GtkApplication *app;
static GbkContext *ctx;

typedef bool (*Pred) (void *data);

static bool
wait_until (Pred pred, void *data)
{
  int64_t deadline = g_get_monotonic_time () + INT64_C (10) * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < deadline)
    {
      while (gtk_events_pending ())
        gtk_main_iteration ();
      if (pred (data))
        return true;
      g_usleep (1000);
    }
  return pred (data);
}

static void
drain (void)
{
  for (int i = 0; i < 50; i++)
    {
      while (gtk_events_pending ())
        gtk_main_iteration ();
      g_usleep (1000);
    }
}

/* ---- fakes ---- */

typedef struct _FakeMonitor FakeMonitor;
static FakeMonitor *monitor;      /* the one in use */
static GbkWindow *watched;        /* its tabs are recorded when an unmount starts */

G_DECLARE_FINAL_TYPE (FakeMount, fake_mount, FAKE, MOUNT, GObject)
G_DECLARE_FINAL_TYPE (FakeVolume, fake_volume, FAKE, VOLUME, GObject)
G_DECLARE_FINAL_TYPE (FakeMonitor, fake_monitor, FAKE, MONITOR, GVolumeMonitor)

struct _FakeMount
{
  GObject parent_instance;
  char *name;
  char *root;
  FakeVolume *volume;        /* borrowed; nullable */
  bool can_unmount;
  bool can_eject;
  int fail;                  /* GIOErrorEnum to fail with; 0 (G_IO_ERROR_FAILED): succeed */
  int calls;                 /* unmounts + ejects started */
  bool busy_at_call;         /* one of our readers still had a folder under root open */
  char *tab_at_call;         /* the watched window's first tab, when the last call came */
};

struct _FakeVolume
{
  GObject parent_instance;
  char *name;
  char *mount_root;          /* where mounting puts it */
  FakeMount *mount;          /* owned; nullable */
  bool can_mount;
  int fail;
  int calls;
  GTask *held;               /* hold_mounts: the mount waits for release_mount() */
};

struct _FakeMonitor
{
  GVolumeMonitor parent_instance;
  GList *volumes;            /* FakeVolume*, owned */
  GList *mounts;             /* FakeMount*, owned */
};

static bool hold_mounts;

static GList *
copy_list (GList *l)
{
  return g_list_copy_deep (l, (GCopyFunc) (void (*) (void)) g_object_ref, nullptr);
}

static void
monitor_add_mount (FakeMount *m)
{
  monitor->mounts = g_list_append (monitor->mounts, g_object_ref (m));
  g_signal_emit_by_name (monitor, "mount-added", m);
}

static void
monitor_remove_mount (FakeMount *m)
{
  GList *link = g_list_find (monitor->mounts, m);
  if (link == nullptr)
    gbk_fatal ("mount not in the monitor");
  monitor->mounts = g_list_delete_link (monitor->mounts, link);
  if (m->volume != nullptr)
    g_clear_object (&m->volume->mount);
  g_signal_emit_by_name (monitor, "mount-removed", m);
  g_object_unref (m);
}

/* FakeMount */

static GFile *
fm_get_root (GMount *mount)
{
  return g_file_new_for_path (FAKE_MOUNT (mount)->root);
}

static char *
fm_get_name (GMount *mount)
{
  return g_strdup (FAKE_MOUNT (mount)->name);
}

static GVolume *
fm_get_volume (GMount *mount)
{
  FakeVolume *v = FAKE_MOUNT (mount)->volume;
  return v != nullptr ? G_VOLUME (g_object_ref (v)) : nullptr;
}

static GDrive *
fm_get_drive (GMount *mount G_GNUC_UNUSED)
{
  return nullptr;
}

static gboolean
fm_can_unmount (GMount *mount)
{
  return FAKE_MOUNT (mount)->can_unmount;
}

static gboolean
fm_can_eject (GMount *mount)
{
  return FAKE_MOUNT (mount)->can_eject;
}

/* Like the kernel: a mount that one of our readers still holds is busy. */
static void
fm_unmount (GMount *mount, GMountUnmountFlags flags G_GNUC_UNUSED,
            GMountOperation *op G_GNUC_UNUSED, GCancellable *cancellable, GAsyncReadyCallback cb,
            void *data)
{
  FakeMount *m = FAKE_MOUNT (mount);
  m->calls++;
  m->busy_at_call = gbk_scanner_busy_under (m->root);
  g_free (m->tab_at_call);
  m->tab_at_call = watched != nullptr ? g_strdup (tab_path (watched, 0)) : nullptr;
  g_autoptr (GTask) t = g_task_new (mount, cancellable, cb, data);
  if (m->busy_at_call)
    g_task_return_new_error (t, G_IO_ERROR, G_IO_ERROR_BUSY, "target is busy");
  else if (m->fail != 0)
    g_task_return_new_error (t, G_IO_ERROR, m->fail, "fake failure");
  else
    {
      monitor_remove_mount (m);
      g_task_return_boolean (t, true);
    }
}

[[nodiscard]] static gboolean
fm_finish (GMount *mount G_GNUC_UNUSED, GAsyncResult *res, GError **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}

static void
fake_mount_iface_init (GMountIface *iface)
{
  iface->get_root = fm_get_root;
  iface->get_name = fm_get_name;
  iface->get_volume = fm_get_volume;
  iface->get_drive = fm_get_drive;
  iface->can_unmount = fm_can_unmount;
  iface->can_eject = fm_can_eject;
  iface->unmount_with_operation = fm_unmount;
  iface->unmount_with_operation_finish = fm_finish;
  iface->eject_with_operation = fm_unmount;
  iface->eject_with_operation_finish = fm_finish;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (FakeMount, fake_mount, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT, fake_mount_iface_init))

static void
fake_mount_finalize (GObject *o)
{
  FakeMount *m = FAKE_MOUNT (o);
  g_free (m->name);
  g_free (m->root);
  g_free (m->tab_at_call);
  G_OBJECT_CLASS (fake_mount_parent_class)->finalize (o);
}

static void
fake_mount_class_init (FakeMountClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fake_mount_finalize;
}

static void
fake_mount_init (FakeMount *m G_GNUC_UNUSED)
{
}

static FakeMount *
fake_mount_new (const char *name, const char *root, bool can_unmount, bool can_eject)
{
  FakeMount *m = g_object_new (fake_mount_get_type (), nullptr);
  m->name = g_strdup (name);
  m->root = g_strdup (root);
  m->can_unmount = can_unmount;
  m->can_eject = can_eject;
  return m;
}

/* FakeVolume */

static char *
fv_get_name (GVolume *volume)
{
  return g_strdup (FAKE_VOLUME (volume)->name);
}

static GMount *
fv_get_mount (GVolume *volume)
{
  FakeMount *m = FAKE_VOLUME (volume)->mount;
  return m != nullptr ? G_MOUNT (g_object_ref (m)) : nullptr;
}

static GDrive *
fv_get_drive (GVolume *volume G_GNUC_UNUSED)
{
  return nullptr;
}

static gboolean
fv_can_mount (GVolume *volume)
{
  return FAKE_VOLUME (volume)->can_mount;
}

static gboolean
fv_can_eject (GVolume *volume G_GNUC_UNUSED)
{
  return false;
}

static void
complete_mount (FakeVolume *v, GTask *t)
{
  if (v->fail != 0)
    {
      g_task_return_new_error (t, G_IO_ERROR, v->fail, "fake mount failure");
      return;
    }
  v->mount = fake_mount_new (v->name, v->mount_root, true, false);
  v->mount->volume = v;
  monitor_add_mount (v->mount);
  g_task_return_boolean (t, true);
}

static void
fv_mount (GVolume *volume, GMountMountFlags flags G_GNUC_UNUSED, GMountOperation *op G_GNUC_UNUSED,
          GCancellable *cancellable, GAsyncReadyCallback cb, void *data)
{
  FakeVolume *v = FAKE_VOLUME (volume);
  v->calls++;
  g_autoptr (GTask) t = g_task_new (volume, cancellable, cb, data);
  if (hold_mounts)
    v->held = g_steal_pointer (&t);
  else
    complete_mount (v, t);
}

static void
release_mount (FakeVolume *v)
{
  g_autoptr (GTask) t = g_steal_pointer (&v->held);
  if (t == nullptr)
    gbk_fatal ("no mount held");
  complete_mount (v, t);
}

[[nodiscard]] static gboolean
fv_mount_finish (GVolume *volume G_GNUC_UNUSED, GAsyncResult *res, GError **error)
{
  return g_task_propagate_boolean (G_TASK (res), error);
}

static void
fake_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = fv_get_name;
  iface->get_mount = fv_get_mount;
  iface->get_drive = fv_get_drive;
  iface->can_mount = fv_can_mount;
  iface->can_eject = fv_can_eject;
  iface->mount_fn = fv_mount;
  iface->mount_finish = fv_mount_finish;
}

G_DEFINE_FINAL_TYPE_WITH_CODE (FakeVolume, fake_volume, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME, fake_volume_iface_init))

static void
fake_volume_finalize (GObject *o)
{
  FakeVolume *v = FAKE_VOLUME (o);
  g_free (v->name);
  g_free (v->mount_root);
  g_clear_object (&v->mount);
  G_OBJECT_CLASS (fake_volume_parent_class)->finalize (o);
}

static void
fake_volume_class_init (FakeVolumeClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fake_volume_finalize;
}

static void
fake_volume_init (FakeVolume *v G_GNUC_UNUSED)
{
}

/* An unmounted volume; mounting it "mounts" `root` (or fails with `fail`). */
static FakeVolume *
fake_volume_add (const char *name, const char *root, bool can_mount, int fail)
{
  FakeVolume *v = g_object_new (fake_volume_get_type (), nullptr);
  v->name = g_strdup (name);
  v->mount_root = g_strdup (root);
  v->can_mount = can_mount;
  v->fail = fail;
  monitor->volumes = g_list_append (monitor->volumes, v);
  g_signal_emit_by_name (monitor, "volume-added", v);
  return v;
}

/* A volume that's mounted already. */
static FakeMount *
fake_mounted_volume_add (const char *name, const char *root, bool can_eject)
{
  FakeVolume *v = fake_volume_add (name, root, true, 0);
  v->mount = fake_mount_new (name, root, true, can_eject);
  v->mount->volume = v;
  monitor_add_mount (v->mount);
  return v->mount;
}

/* FakeMonitor */

static GList *
fmon_drives (GVolumeMonitor *m G_GNUC_UNUSED)
{
  return nullptr;
}

static GList *
fmon_volumes (GVolumeMonitor *m)
{
  return copy_list (FAKE_MONITOR (m)->volumes);
}

static GList *
fmon_mounts (GVolumeMonitor *m)
{
  return copy_list (FAKE_MONITOR (m)->mounts);
}

G_DEFINE_FINAL_TYPE (FakeMonitor, fake_monitor, G_TYPE_VOLUME_MONITOR)

static void
fake_monitor_finalize (GObject *o)
{
  FakeMonitor *m = FAKE_MONITOR (o);
  g_list_free_full (m->volumes, g_object_unref);
  g_list_free_full (m->mounts, g_object_unref);
  G_OBJECT_CLASS (fake_monitor_parent_class)->finalize (o);
}

static void
fake_monitor_class_init (FakeMonitorClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = fake_monitor_finalize;
  G_VOLUME_MONITOR_CLASS (klass)->get_connected_drives = fmon_drives;
  G_VOLUME_MONITOR_CLASS (klass)->get_volumes = fmon_volumes;
  G_VOLUME_MONITOR_CLASS (klass)->get_mounts = fmon_mounts;
}

static void
fake_monitor_init (FakeMonitor *m G_GNUC_UNUSED)
{
}

/* A fresh, empty fake monitor for the windows made from now on. */
static void
use_fake_monitor (void)
{
  g_clear_object (&monitor);
  monitor = g_object_new (fake_monitor_get_type (), nullptr);
  gbk_devices_use_monitor_for_test (G_VOLUME_MONITOR (monitor));
}

static void
end_fakes (void)
{
  gbk_devices_use_monitor_for_test (nullptr);
  g_clear_object (&monitor);
  watched = nullptr;
}

/* ---- menu reading ---- */

static GMenuModel *
devices_menu (const GbkWindow *w)
{
  return gbk_devices_menu (gbk_window_devices (w));
}

static char *
item_string (GMenuModel *m, int i, const char *attr)
{
  char *s = nullptr;
  if (!g_menu_model_get_item_attribute (m, i, attr, "s", &s))
    return nullptr;
  return s;
}

/* "Label: Action, Action" for every device, joined by " | ". */
static char *
describe (const GbkWindow *w)
{
  GMenuModel *m = devices_menu (w);
  g_autoptr (GString) out = g_string_new (nullptr);
  for (int i = 0; i < g_menu_model_get_n_items (m); i++)
    {
      g_autofree char *label = item_string (m, i, G_MENU_ATTRIBUTE_LABEL);
      g_string_append_printf (out, "%s%s", i > 0 ? " | " : "", label != nullptr ? label : "?");
      g_autoptr (GMenuModel) sub = g_menu_model_get_item_link (m, i, G_MENU_LINK_SUBMENU);
      for (int j = 0; sub != nullptr && j < g_menu_model_get_n_items (sub); j++)
        {
          g_autofree char *action_label = item_string (sub, j, G_MENU_ATTRIBUTE_LABEL);
          g_string_append_printf (out, "%s%s", j > 0 ? ", " : ": ",
                                  action_label != nullptr ? action_label : "?");
        }
    }
  return g_string_free (g_steal_pointer (&out), false);
}

/* The id the menu gives device `label` (as shown) for `action` ("win.device-open", ...). */
static char *
device_id (const GbkWindow *w, const char *label, const char *action)
{
  GMenuModel *m = devices_menu (w);
  char *id = nullptr;
  for (int i = 0; id == nullptr && i < g_menu_model_get_n_items (m); i++)
    {
      g_autofree char *l = item_string (m, i, G_MENU_ATTRIBUTE_LABEL);
      if (l == nullptr || !g_str_equal (l, label))
        continue;
      g_autoptr (GMenuModel) sub = g_menu_model_get_item_link (m, i, G_MENU_LINK_SUBMENU);
      for (int j = 0; id == nullptr && sub != nullptr && j < g_menu_model_get_n_items (sub); j++)
        {
          g_autofree char *a = item_string (sub, j, G_MENU_ATTRIBUTE_ACTION);
          if (a == nullptr || !g_str_equal (a, action))
            continue;
          g_autoptr (GVariant) target = g_menu_model_get_item_attribute_value (
              sub, j, G_MENU_ATTRIBUTE_TARGET, G_VARIANT_TYPE_STRING);
          if (target != nullptr)
            id = g_variant_dup_string (target, nullptr);
        }
    }
  if (id == nullptr)
    gbk_fatal ("no %s for %s in: %s", action, label, describe (w));
  return id;
}

static void
device_action (GbkWindow *w, const char *label, const char *action)
{
  g_autofree char *id = device_id (w, label, action);
  g_action_group_activate_action (G_ACTION_GROUP (w), action + strlen ("win."),
                                  g_variant_new_string (id));
}

typedef struct
{
  GbkWindow *w;
  const char *text;
} DescCtx;

static bool
described_as (void *data)
{
  const DescCtx *c = data;
  g_autofree char *d = describe (c->w);
  return g_str_equal (d, c->text);
}

static void
expect_menu (GbkWindow *w, const char *text)
{
  DescCtx c = { w, text };
  if (!wait_until (described_as, &c))
    {
      g_autofree char *d = describe (w);
      gbk_fatal ("menu is \"%s\", expected \"%s\"", d, text);
    }
}

/* ---- window helpers ---- */

typedef struct
{
  GbkWindow *w;
  int tab;
  const char *path;
} TabCtx;

static bool
tab_at (void *data)
{
  TabCtx *c = data;
  return gbk_window_n_tabs (c->w) > c->tab && g_str_equal (tab_path (c->w, c->tab), c->path);
}

static void
wait_tab (GbkWindow *w, int tab, const char *path)
{
  TabCtx c = { w, tab, path };
  if (!wait_until (tab_at, &c))
    gbk_fatal ("tab %d is at %s, expected %s", tab, tab_path (w, tab), path);
}

static GbkWindow *
new_window (const char *path)
{
  GbkWindow *w = window_at (app, ctx, path);
  gtk_widget_show_all (GTK_WIDGET (w));
  gbk_devices_start (gbk_window_devices (w));   /* don't wait for the delay */
  return w;
}

static GtkMessageDialog *
visible_dialog (void)
{
  g_autoptr (GList) tops = gtk_window_list_toplevels ();
  for (GList *l = tops; l != nullptr; l = l->next)
    if (GTK_IS_MESSAGE_DIALOG (l->data) && gtk_widget_get_visible (l->data))
      return l->data;
  return nullptr;
}

static bool
dialog_shown (void *data G_GNUC_UNUSED)
{
  return visible_dialog () != nullptr;
}

/* An error dialog titled `title` appeared; close it. */
static void
expect_error (const char *title)
{
  if (!wait_until (dialog_shown, nullptr))
    gbk_fatal ("no error dialog");
  GtkMessageDialog *d = visible_dialog ();
  g_autofree char *text = nullptr;
  g_object_get (d, "text", &text, nullptr);
  g_assert_cmpstr (text, ==, title);
  gtk_dialog_response (GTK_DIALOG (d), GTK_RESPONSE_CLOSE);
  drain ();
  g_assert_null (visible_dialog ());
}

typedef struct
{
  const int *counter;
  int at_least;
} CountCtx;

static bool
counted (void *data)
{
  CountCtx *c = data;
  return *c->counter >= c->at_least;
}

static void
wait_count (const int *counter, int n)
{
  CountCtx c = { counter, n };
  if (!wait_until (counted, &c))
    gbk_fatal ("count is %d, expected %d", *counter, n);
}

static char *
make_dir (const char *parent, const char *name)
{
  char *p = g_build_filename (parent, name, nullptr);
  if (g_mkdir_with_parents (p, 0755) != 0)
    gbk_fatal ("mkdir %s", p);
  return p;
}

static void
rm_rf (const char *path)
{
  g_autoptr (GDir) d = g_dir_open (path, 0, nullptr);
  const char *name;
  while (d != nullptr && (name = g_dir_read_name (d)) != nullptr)
    {
      g_autofree char *child = g_build_filename (path, name, nullptr);
      if (g_file_test (child, G_FILE_TEST_IS_DIR) && !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
        rm_rf (child);
      else
        unlink (child);
    }
  rmdir (path);
}

/* ---- tests ---- */

/* The real monitor (in the container: the system mount table) builds a menu. */
static void
test_system_monitor (void)
{
  g_autoptr (GbkDevices) d = gbk_devices_new ();
  g_assert_false (gbk_devices_started (d));
  gbk_devices_start (d);
  g_assert_true (gbk_devices_started (d));
  g_assert_cmpint (g_menu_model_get_n_items (gbk_devices_menu (d)), >=, 1);
}

static void
test_menu (void)
{
  use_fake_monitor ();
  g_autofree char *media = g_dir_make_tmp ("gbk-dev-XXXXXX", nullptr);
  g_autofree char *a = make_dir (media, "Stick_1");
  g_autofree char *c = make_dir (media, "Share");

  GbkWindow *w = window_at (app, ctx, media);
  gtk_widget_show_all (GTK_WIDGET (w));
  /* Before the delay: only the file system, which needs no monitor. It opens /. */
  g_assert_false (gbk_devices_started (gbk_window_devices (w)));
  expect_menu (w, "File System: Open");
  device_action (w, "File System", "win.device-open");
  g_assert_cmpstr (wpath (w), ==, "/");
  open_at (w, media);

  fake_mounted_volume_add ("Stick_1", a, true);
  fake_volume_add ("Backup", "/nonexistent", true, 0);
  fake_volume_add ("Unusable", "/nonexistent", false, 0);    /* can't mount or eject: left out */
  FakeMount *share = fake_mount_new ("Share", c, true, false);  /* a mount without a volume */
  monitor_add_mount (share);
  g_object_unref (share);
  /* It starts on its own, a little after the window opens. */
  expect_menu (w, "File System: Open | Stick__1: Open, Unmount, Eject | Backup (not mounted): Open "
                  "| Share: Open, Unmount");
  g_assert_true (gbk_devices_started (gbk_window_devices (w)));

  /* It follows changes. */
  monitor_remove_mount (share);
  expect_menu (w,
               "File System: Open | Stick__1: Open, Unmount, Eject | Backup (not mounted): Open");

  /* Ids are never reused: a stale one, or one that isn't a device, does nothing. */
  g_autofree char *old = device_id (w, "Backup (not mounted)", "win.device-open");
  fake_volume_add ("Later", "/nonexistent", true, 0);
  expect_menu (w, "File System: Open | Stick__1: Open, Unmount, Eject | Backup (not mounted): Open "
                  "| Later (not mounted): Open");
  const char *stale[] = { old, "", "x", "18446744073709551615" };
  for (size_t i = 0; i < G_N_ELEMENTS (stale); i++)
    g_action_group_activate_action (G_ACTION_GROUP (w), "device-open",
                                    g_variant_new_string (stale[i]));
  drain ();
  g_assert_cmpstr (wpath (w), ==, media);
  g_assert_null (visible_dialog ());

  gtk_widget_destroy (GTK_WIDGET (w));
  end_fakes ();
  rm_rf (media);
}

static void
test_open (void)
{
  use_fake_monitor ();
  g_autofree char *media = g_dir_make_tmp ("gbk-dev-XXXXXX", nullptr);
  g_autofree char *a = make_dir (media, "A");
  g_autofree char *b = make_dir (media, "B");
  fake_mounted_volume_add ("A", a, false);
  const FakeVolume *vb = fake_volume_add ("B", b, true, 0);
  const FakeVolume *vc = fake_volume_add ("C", "/nonexistent", true, G_IO_ERROR_PERMISSION_DENIED);
  FakeVolume *vd = fake_volume_add ("D", "/nonexistent", true, G_IO_ERROR_FAILED_HANDLED);

  GbkWindow *w = new_window (media);
  expect_menu (w, "File System: Open | A: Open, Unmount | B (not mounted): Open | C (not mounted): "
                  "Open | D (not mounted): Open");

  /* Mounted: opens in the tab. */
  device_action (w, "A", "win.device-open");
  wait_tab (w, 0, a);

  /* Not mounted: mounts, then opens; the menu now shows it mounted. */
  device_action (w, "B (not mounted)", "win.device-open");
  wait_tab (w, 0, b);
  g_assert_cmpint (vb->calls, ==, 1);
  expect_menu (w, "File System: Open | A: Open, Unmount | B: Open, Unmount | C (not mounted): Open "
                  "| D (not mounted): Open");

  /* Failing: says so, and stays put. */
  device_action (w, "C (not mounted)", "win.device-open");
  expect_error ("Couldn't open the device");
  g_assert_cmpint (vc->calls, ==, 1);
  g_assert_cmpstr (wpath (w), ==, b);

  /* Already reported by the mount operation (or a cancelled password prompt): no second
   * dialog. */
  device_action (w, "D (not mounted)", "win.device-open");
  wait_count (&vd->calls, 1);
  drain ();
  g_assert_null (visible_dialog ());
  g_assert_cmpstr (wpath (w), ==, b);

  gtk_widget_destroy (GTK_WIDGET (w));
  end_fakes ();
  rm_rf (media);
}

static void
check_removal (const char *action, const char *menu_after)
{
  use_fake_monitor ();
  g_autofree char *media = g_dir_make_tmp ("gbk-dev-XXXXXX", nullptr);
  g_autofree char *a = make_dir (media, "A");
  g_autofree char *deep = make_dir (a, "sub/deeper");
  g_autofree char *elsewhere = make_dir (media, "elsewhere");
  g_autofree char *a2 = make_dir (media, "A2");    /* shares a prefix with A, isn't in it */
  FakeMount *ma = fake_mounted_volume_add ("A", a, true);
  g_object_ref (ma);

  GbkWindow *w = new_window (deep);
  watched = w;
  new_tab_at (w, elsewhere);
  new_tab_at (w, a2);
  new_tab_at (w, a);
  expect_menu (w, "File System: Open | A: Open, Unmount, Eject");

  /* Tabs on the device leave it first, and we wait for their readers to let go (the fake,
   * like the kernel, refuses while one still holds a folder there). Other tabs stay.
   * Readers are slow to let go here, as on a slow device: without the wait, it's busy. */
  gbk_scanner_set_release_delay_for_test (200);
  device_action (w, "A", action);
  wait_count (&ma->calls, 1);
  gbk_scanner_set_release_delay_for_test (0);
  g_assert_false (ma->busy_at_call);
  g_assert_cmpstr (ma->tab_at_call, ==, media);
  expect_menu (w, menu_after);
  g_assert_cmpint (gbk_window_n_tabs (w), ==, 4);
  wait_tab (w, 0, media);
  wait_tab (w, 1, elsewhere);
  wait_tab (w, 2, a2);
  wait_tab (w, 3, media);
  g_assert_null (visible_dialog ());

  gtk_widget_destroy (GTK_WIDGET (w));
  g_object_unref (ma);
  end_fakes ();
  rm_rf (media);
}

static void
test_unmount (void)
{
  check_removal ("win.device-unmount", "File System: Open | A (not mounted): Open");
}

static void
test_eject (void)
{
  check_removal ("win.device-eject", "File System: Open | A (not mounted): Open");
}

/* Another program is using it: the system's refusal is shown. */
static void
test_busy (void)
{
  use_fake_monitor ();
  g_autofree char *media = g_dir_make_tmp ("gbk-dev-XXXXXX", nullptr);
  g_autofree char *a = make_dir (media, "A");
  FakeMount *ma = fake_mounted_volume_add ("A", a, true);
  ma->fail = G_IO_ERROR_BUSY;

  GbkWindow *w = new_window (media);
  expect_menu (w, "File System: Open | A: Open, Unmount, Eject");
  device_action (w, "A", "win.device-unmount");
  expect_error ("Couldn't remove the device");
  device_action (w, "A", "win.device-eject");
  expect_error ("Couldn't remove the device");
  g_assert_cmpint (ma->calls, ==, 2);
  expect_menu (w, "File System: Open | A: Open, Unmount, Eject");

  gtk_widget_destroy (GTK_WIDGET (w));
  end_fakes ();
  rm_rf (media);
}

/* Another program unmounts through gvfs: tabs there move out before it happens. */
static void
test_unmounted_elsewhere (void)
{
  use_fake_monitor ();
  g_autofree char *media = g_dir_make_tmp ("gbk-dev-XXXXXX", nullptr);
  g_autofree char *a = make_dir (media, "A");
  g_autofree char *sub = make_dir (a, "sub");
  FakeMount *ma = fake_mounted_volume_add ("A", a, false);

  GbkWindow *w = new_window (sub);
  new_tab_at (w, media);
  expect_menu (w, "File System: Open | A: Open, Unmount");
  g_signal_emit_by_name (monitor, "mount-pre-unmount", ma);
  wait_tab (w, 0, media);
  wait_tab (w, 1, media);

  gtk_widget_destroy (GTK_WIDGET (w));
  end_fakes ();
  rm_rf (media);
}

/* The window closes while a mount is still in progress: it finishes quietly. */
static void
set_true (void *flag)
{
  *(bool *) flag = true;
}

static void
test_window_closes_mid_mount (void)
{
  use_fake_monitor ();
  g_autofree char *media = g_dir_make_tmp ("gbk-dev-XXXXXX", nullptr);
  g_autofree char *b = make_dir (media, "B");
  FakeVolume *vb = fake_volume_add ("B", b, true, 0);

  GbkWindow *w = new_window (media);
  expect_menu (w, "File System: Open | B (not mounted): Open");
  hold_mounts = true;
  device_action (w, "B (not mounted)", "win.device-open");
  wait_count (&vb->calls, 1);
  GtkWidget *alive = GTK_WIDGET (w);
  g_object_add_weak_pointer (G_OBJECT (w), (void **) &alive);
  /* Object data is released at finalize: proof the window is freed, not only destroyed
   * (GtkMountOperation used to keep it, see device_action). */
  bool finalized = false;
  g_object_set_data_full (G_OBJECT (w), "test-finalized", &finalized, set_true);
  gtk_widget_destroy (GTK_WIDGET (w));
  drain ();
  g_assert_null (alive);
  release_mount (vb);
  hold_mounts = false;
  drain ();
  g_assert_null (visible_dialog ());
  g_assert_true (finalized);
  end_fakes ();
  rm_rf (media);
}

static void
test_path_is_under (void)
{
  g_assert_true (gbk_path_is_under ("/media/a", "/media/a"));
  g_assert_true (gbk_path_is_under ("/media/a/b", "/media/a"));
  g_assert_true (gbk_path_is_under ("/media/a/b", "/media/a/"));
  g_assert_false (gbk_path_is_under ("/media/ab", "/media/a"));
  g_assert_false (gbk_path_is_under ("/media", "/media/a"));
  g_assert_true (gbk_path_is_under ("/anything", "/"));
  g_assert_true (gbk_path_is_under ("/", "/"));
}

static bool
no_workers (void *data G_GNUC_UNUSED)
{
  return gbk_scanner_live_workers () == 0;
}

int
main (int argc, char **argv)
{
  g_autofree char *home = g_dir_make_tmp ("gbk-dev-home-XXXXXX", nullptr);
  g_setenv ("HOME", home, true);
  gtk_test_init (&argc, &argv, nullptr);
  ctx = gbk_context_new ("/nonexistent/gbk-test.conf");
  app = gtk_application_new ("net.gbk.QuoinDevicesTest", G_APPLICATION_NON_UNIQUE);
  g_autoptr (GError) error = nullptr;
  if (!g_application_register (G_APPLICATION (app), nullptr, &error))
    gbk_fatal ("register: %s", error->message);

  g_test_add_func ("/devices/path-is-under", test_path_is_under);
  g_test_add_func ("/devices/system-monitor", test_system_monitor);
  g_test_add_func ("/devices/menu", test_menu);
  g_test_add_func ("/devices/open", test_open);
  g_test_add_func ("/devices/unmount", test_unmount);
  g_test_add_func ("/devices/eject", test_eject);
  g_test_add_func ("/devices/busy", test_busy);
  g_test_add_func ("/devices/unmounted-elsewhere", test_unmounted_elsewhere);
  g_test_add_func ("/devices/window-closes-mid-mount", test_window_closes_mid_mount);
  int status = g_test_run ();

  drain ();
  g_object_unref (app);
  gbk_context_unref (ctx);
  wait_until (no_workers, nullptr);
  if (gbk_lifetime_live () != 0)
    gbk_fatal ("%d registrations leaked", gbk_lifetime_live ());
  rmdir (home);
  return status;
}
