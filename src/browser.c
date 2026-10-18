#include "browser.h"
#include "lifetime.h"
#include "open.h"
#include "remote.h"

#define HISTORY_MAX 100
#define PENDING_SELECT_MS 2000

struct _GbkBrowser
{
  GtkBox parent_instance;

  GbkContext *ctx;
  GtkBox *toolbar;
  GbkPathbar *pathbar;
  GbkView *view;
  GtkInfoBar *bar;            /* a folder on another machine that can't be reached */
  GtkLabel *bar_label;
  GtkWidget *login_button;

  /* An address waiting for its machine's home folder (host:, host:~/x, host:rel). */
  bool has_pending;
  GbkAddress pending;
  GbkConn *pending_conn;

  GbkLoc *loc;
  GbkDirModel *model;
  gulong model_state_id;
  gulong model_changed_id;

  GPtrArray *back;            /* GbkLoc*, most recent last */
  GPtrArray *forward;

  bool show_hidden;
  GbkSortMode sort;
  char *focus_on_load;        /* raw name to select once it appears (after going up) */
  GPtrArray *pending_select;  /* names to select as they appear (after paste/rename) */
  guint pending_timeout;
  char *message;
};

G_DEFINE_FINAL_TYPE (GbkBrowser, gbk_browser, GTK_TYPE_BOX)

enum
{
  SIGNAL_PATH_CHANGED,
  SIGNAL_STATUS_CHANGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void
status_changed (GbkBrowser *b)
{
  g_signal_emit (b, signals[SIGNAL_STATUS_CHANGED], 0);
}

/* ---- status ---- */

static char *
format_size (uint64_t bytes)
{
  return g_format_size_full (bytes, G_FORMAT_SIZE_IEC_UNITS);
}

char *
gbk_browser_status (GbkBrowser *b)
{
  GString *s = g_string_new ("");
  GbkDirModel *m = b->model;
  const char *search = gbk_view_type_ahead_text (b->view);

  if (b->message != nullptr)
    g_string_append (s, b->message);
  else if (search[0] != '\0')
    g_string_append_printf (s, "Find: %s", search);
  else if (b->has_pending)
    g_string_append_printf (s, "Connecting to %s…", b->pending.host);
  else if (m == nullptr)
    ;
  else if (gbk_dir_model_state (m) == GBK_DIR_ERROR)
    g_string_append_printf (s, "Can't open: %s", g_strerror (gbk_dir_model_error (m)));
  else if (gbk_dir_model_state (m) == GBK_DIR_DISCONNECTED)
    g_string_append_printf (s, "Disconnected from %s. Reconnecting…", gbk_loc_host (b->loc));
  else if (gbk_dir_model_state (m) == GBK_DIR_LOADING && gbk_dir_model_connecting (m))
    g_string_append_printf (s, "Connecting to %s…", gbk_loc_host (b->loc));
  else
    {
      size_t n_sel;
      g_autofree GbkHandle *sel = gbk_view_selected (b->view, &n_sel);
      if (n_sel == 1)
        {
          const GbkEntry *e = gbk_dir_model_get (m, sel[0]);
          g_string_append (s, gbk_entry_display (e));
          if (!gbk_entry_is_dir (e) && !(e->flags & GBK_ENTRY_STAT_FAILED))
            {
              g_autofree char *size = format_size (e->size);
              g_string_append_printf (s, "  %s", size);
            }
        }
      else if (n_sel > 1)
        {
          uint64_t total = 0;
          for (size_t i = 0; i < n_sel; i++)
            {
              const GbkEntry *e = gbk_dir_model_get (m, sel[i]);
              if (!gbk_entry_is_dir (e))
                total += e->size;
            }
          g_autofree char *size = format_size (total);
          g_string_append_printf (s, "%zu selected  %s", n_sel, size);
        }
      else
        {
          size_t n = gbk_dir_model_n_visible (m), dirs = 0;
          for (size_t i = 0; i < n; i++)
            if (gbk_entry_is_dir (gbk_dir_model_get (m, gbk_dir_model_visible_handle (m, i))))
              dirs++;
          size_t files = n - dirs;
          if (dirs > 0)
            g_string_append_printf (s, "%zu %s", dirs, dirs == 1 ? "folder" : "folders");
          if (files > 0)
            g_string_append_printf (s, "%s%zu %s", dirs > 0 ? " · " : "", files,
                                    files == 1 ? "file" : "files");
          if (n == 0)
            g_string_append (s, gbk_dir_model_state (m) == GBK_DIR_LOADING ? "Loading…" : "Empty");
        }
      uint64_t free_bytes;
      if (gbk_dir_model_free_space (m, &free_bytes))
        {
          g_autofree char *size = format_size (free_bytes);
          g_string_append_printf (s, "   %s free", size);
        }
    }
  return g_string_free (s, false);
}

void
gbk_browser_set_message (GbkBrowser *b, const char *message)
{
  g_free (b->message);
  b->message = g_strdup (message);
  status_changed (b);
}

/* ---- pending selection ---- */

/* Select whichever pending names exist (they arrive via live updates, possibly in several
 * batches). Stops once all are there, on navigation, or after a short while. */
static void
apply_pending_select (GbkBrowser *b)
{
  if (b->pending_select == nullptr || b->model == nullptr)
    return;
  g_autofree GbkHandle *handles = g_new (GbkHandle, MAX (b->pending_select->len, 1));
  size_t n = 0;
  for (guint i = 0; i < b->pending_select->len; i++)
    {
      GbkHandle h = gbk_dir_model_find (b->model, g_ptr_array_index (b->pending_select, i));
      if (h != GBK_HANDLE_NONE)
        handles[n++] = h;
    }
  if (n > 0)
    gbk_view_set_selection (b->view, handles, n);
  if (n == b->pending_select->len)
    {
      g_clear_pointer (&b->pending_select, g_ptr_array_unref);
      gbk_source_cancel (&b->pending_timeout);
    }
}

static bool
pending_expired (void *data)
{
  GbkBrowser *b = data;
  b->pending_timeout = 0;
  g_clear_pointer (&b->pending_select, g_ptr_array_unref);
  return false;
}

void
gbk_browser_select_names (GbkBrowser *b, GPtrArray *names)
{
  g_clear_pointer (&b->pending_select, g_ptr_array_unref);
  gbk_source_cancel (&b->pending_timeout);
  if (names == nullptr || names->len == 0)
    return;
  b->pending_select = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < names->len; i++)
    g_ptr_array_add (b->pending_select, g_strdup (g_ptr_array_index (names, i)));
  b->pending_timeout = gbk_timeout_add (G_OBJECT (b), PENDING_SELECT_MS, pending_expired, b);
  apply_pending_select (b);
}

/* ---- model & navigation ---- */

static void open_loc (GbkBrowser *b, GbkLoc *loc, const char *focus_name);

static void
on_model_changed (GbkDirModel *m, GbkBrowser *b)
{
  if (m != b->model)
    return;
  if (b->focus_on_load != nullptr)
    {
      GbkHandle h = gbk_dir_model_find (m, b->focus_on_load);
      if (h != GBK_HANDLE_NONE)
        {
          gbk_view_select_only (b->view, h);
          g_clear_pointer (&b->focus_on_load, g_free);
        }
    }
  apply_pending_select (b);
  status_changed (b);
}

/* The info bar: shown while the folder's machine can't be reached. */
static void
update_bar (GbkBrowser *b)
{
  bool down = b->model != nullptr && gbk_dir_model_state (b->model) == GBK_DIR_DISCONNECTED;
  if (down)
    {
      g_autofree char *text = g_strdup_printf ("Can't reach %s: %s", gbk_loc_host (b->loc),
                                               gbk_dir_model_conn_error (b->model));
      gtk_label_set_text (b->bar_label, text);
      gtk_widget_set_visible (b->login_button, gbk_dir_model_needs_login (b->model));
    }
  gtk_widget_set_visible (GTK_WIDGET (b->bar), down);
}

enum
{
  RESPONSE_RETRY = 1,
  RESPONSE_LOGIN = 2,
};

static void
on_bar_response (GtkInfoBar *bar G_GNUC_UNUSED, int response, GbkBrowser *b)
{
  if (b->model == nullptr || b->loc == nullptr || gbk_loc_is_local (b->loc))
    return;
  if (response == RESPONSE_LOGIN)
    {
      g_autoptr (GError) error = nullptr;
      if (!gbk_open_ssh_login (b->ctx->config, gbk_loc_host (b->loc), &error))
        gbk_browser_set_message (b, error->message);
      return;       /* the retry comes when the window has the focus back (see window.c) */
    }
  if (response == RESPONSE_RETRY)
    gbk_dir_model_reconnect (b->model);
}

void
gbk_browser_reconnect (GbkBrowser *b)
{
  if (b->model != nullptr)
    gbk_dir_model_reconnect (b->model);
}

static void
on_model_state (GbkDirModel *m, GbkBrowser *b)
{
  if (m != b->model)
    return;
  update_bar (b);
  GbkDirState state = gbk_dir_model_state (m);
  if (state == GBK_DIR_READY)
    g_clear_pointer (&b->focus_on_load, g_free);   /* it never appeared: forget it */
  if (state == GBK_DIR_READY && !gbk_loc_is_local (b->loc))
    gbk_pathbar_refresh (b->pathbar);   /* its home folder is known now */

  /* The directory vanished (deleted, moved, unmounted) or never existed: walk up to the
   * nearest ancestor. Each level is another async attempt, so nothing blocks. */
  bool missing = state == GBK_DIR_GONE
                 || (state == GBK_DIR_ERROR
                     && (gbk_dir_model_error (m) == ENOENT || gbk_dir_model_error (m) == ENOTDIR));
  if (missing && !gbk_loc_is_root (b->loc))
    {
      g_autoptr (GbkLoc) parent = gbk_loc_parent (b->loc);
      g_autofree char *child = gbk_loc_name (b->loc);
      open_loc (b, parent, child);
      return;
    }
  status_changed (b);
}

static void
set_model (GbkBrowser *b, GbkDirModel *m)
{
  if (b->model != nullptr)
    {
      g_signal_handler_disconnect (b->model, b->model_state_id);
      g_signal_handler_disconnect (b->model, b->model_changed_id);
      g_clear_object (&b->model);
    }
  b->model = m;     /* takes the reference */
  if (m != nullptr)
    {
      gbk_dir_model_set_show_hidden (m, b->show_hidden);
      gbk_dir_model_set_sort (m, b->sort);
      b->model_state_id
          = gbk_connect (G_OBJECT (b), m, "state-changed", G_CALLBACK (on_model_state), b);
      b->model_changed_id
          = gbk_connect (G_OBJECT (b), m, "changed", G_CALLBACK (on_model_changed), b);
    }
  gbk_view_set_model (b->view, m);
  update_bar (b);
}

/* Replace the current directory without touching history. */
static void
open_loc (GbkBrowser *b, GbkLoc *loc, const char *focus_name)
{
  gbk_loc_ref (loc);              /* first: `loc` may be the one being replaced */
  g_clear_pointer (&b->loc, gbk_loc_unref);
  b->loc = loc;
  g_free (b->focus_on_load);
  b->focus_on_load = g_strdup (focus_name);
  gbk_browser_select_names (b, nullptr);
  g_clear_pointer (&b->message, g_free);
  set_model (b, gbk_dir_model_new (b->loc, b->ctx->types));
  gbk_pathbar_set_loc (b->pathbar, b->loc);
  g_signal_emit (b, signals[SIGNAL_PATH_CHANGED], 0);
  status_changed (b);
}

static void
push_history (GPtrArray *stack, GbkLoc *loc)
{
  if (stack->len >= HISTORY_MAX)
    g_ptr_array_remove_index (stack, 0);
  g_ptr_array_add (stack, gbk_loc_ref (loc));
}

void
gbk_browser_open (GbkBrowser *b, GbkLoc *loc)
{
  if (b->loc != nullptr)
    {
      if (gbk_loc_equal (loc, b->loc))
        return;
      push_history (b->back, b->loc);
      g_ptr_array_set_size (b->forward, 0);
    }
  open_loc (b, loc, nullptr);
}

void
gbk_browser_back (GbkBrowser *b)
{
  if (b->back->len == 0)
    return;
  g_autoptr (GbkLoc) target = g_ptr_array_steal_index (b->back, b->back->len - 1);
  g_autofree char *came_from = gbk_loc_name (b->loc);
  push_history (b->forward, b->loc);
  /* If we're going back to the parent we came from, select where we were. */
  open_loc (b, target, gbk_loc_is_child_of (b->loc, target) ? came_from : nullptr);
}

void
gbk_browser_forward (GbkBrowser *b)
{
  if (b->forward->len == 0)
    return;
  g_autoptr (GbkLoc) target = g_ptr_array_steal_index (b->forward, b->forward->len - 1);
  push_history (b->back, b->loc);
  open_loc (b, target, nullptr);
}

void
gbk_browser_up (GbkBrowser *b)
{
  if (gbk_loc_is_root (b->loc))
    return;
  g_autoptr (GbkLoc) parent = gbk_loc_parent (b->loc);
  g_autofree char *child = gbk_loc_name (b->loc);
  push_history (b->back, b->loc);
  g_ptr_array_set_size (b->forward, 0);
  open_loc (b, parent, child);
}

/* ---- addresses ---- */

static void
clear_pending (GbkBrowser *b)
{
  if (b->has_pending)
    gbk_address_clear (&b->pending);
  b->has_pending = false;
  g_clear_object (&b->pending_conn);   /* its handler checks this, so it does nothing now */
}

/* `a`, now that `home` (its machine's home folder, or nullptr) is known. */
static void
open_resolved (GbkBrowser *b, const GbkAddress *a, const char *host, const char *home)
{
  g_autoptr (GbkLoc) loc = nullptr;
  switch (a->base)
    {
    case GBK_ADDRESS_ABSOLUTE:
      loc = host != nullptr ? gbk_loc_new_remote (host, a->path) : gbk_loc_new_local (a->path);
      break;
    case GBK_ADDRESS_CURRENT:
      loc = b->loc != nullptr ? gbk_loc_new_like (b->loc, a->path) : gbk_loc_new_local (a->path);
      break;
    case GBK_ADDRESS_HOME:
      {
        g_autofree char *path = g_build_filename (home, a->path, nullptr);
        loc = host != nullptr ? gbk_loc_new_remote (host, path) : gbk_loc_new_local (path);
        break;
      }
    default: gbk_fatal ("bad address base %d", (int) a->base);
    }
  if (loc == nullptr)
    {
      gbk_browser_set_message (b, "That isn't a location");
      return;
    }
  gbk_browser_open (b, loc);
}

static void
on_pending_conn (GbkConn *c, GbkBrowser *b)
{
  if (c != b->pending_conn)
    return;                              /* an earlier address, since replaced */
  if (gbk_conn_state (c) == GBK_CONN_READY)
    {
      GbkAddress a = b->pending;         /* moved out */
      b->has_pending = false;
      g_autofree char *home = g_strdup (gbk_conn_hello (c)->home);
      g_clear_object (&b->pending_conn);
      open_resolved (b, &a, a.host, home);
      gbk_address_clear (&a);
    }
  else if (gbk_conn_state (c) == GBK_CONN_DEAD)
    {
      g_autofree char *msg
          = g_strdup_printf ("Couldn't connect to %s: %s", b->pending.host, gbk_conn_error (c));
      clear_pending (b);
      if (b->loc == nullptr)             /* a new tab with nowhere to be: home, here */
        {
          g_autoptr (GbkLoc) home = gbk_loc_new_local (g_get_home_dir ());
          gbk_browser_open (b, home);
        }
      gbk_browser_set_message (b, msg);
    }
}

void
gbk_browser_open_address (GbkBrowser *b, const GbkAddress *a)
{
  clear_pending (b);
  const char *host = a->host != nullptr  ? a->host
                     : b->loc != nullptr ? gbk_loc_host (b->loc)
                                         : nullptr;
  if (a->base != GBK_ADDRESS_HOME || host == nullptr)
    {
      open_resolved (b, a, host, g_get_home_dir ());
      return;
    }
  /* Another machine's home: known once it has answered. */
  GbkRemote *r = gbk_remote_default ();
  g_autoptr (GError) error = nullptr;
  g_autoptr (GbkConn) c = r != nullptr ? gbk_remote_conn (r, host, &error) : nullptr;
  if (c == nullptr)
    {
      gbk_browser_set_message (b, error != nullptr ? error->message
                                                   : "Other machines can't be reached from here");
      return;
    }
  if (gbk_conn_state (c) == GBK_CONN_READY)
    {
      open_resolved (b, a, host, gbk_conn_hello (c)->home);
      return;
    }
  b->has_pending = true;
  b->pending = (GbkAddress) { g_strdup (host), a->base, g_strdup (a->path) };
  b->pending_conn = g_object_ref (c);
  gbk_connect (G_OBJECT (b), c, "state-changed", G_CALLBACK (on_pending_conn), b);
  status_changed (b);
}

void
gbk_browser_home (GbkBrowser *b)
{
  char here[] = "";
  GbkAddress home = { nullptr, GBK_ADDRESS_HOME, here };
  gbk_browser_open_address (b, &home);
}

GbkLoc *
gbk_browser_loc (const GbkBrowser *b)
{
  return b->loc;
}

GbkView *
gbk_browser_view (const GbkBrowser *b)
{
  return b->view;
}

GbkPathbar *
gbk_browser_pathbar (const GbkBrowser *b)
{
  return b->pathbar;
}

GtkBox *
gbk_browser_toolbar (const GbkBrowser *b)
{
  return b->toolbar;
}

GbkDirModel *
gbk_browser_model (const GbkBrowser *b)
{
  return b->model;
}

void
gbk_browser_set_show_hidden (GbkBrowser *b, bool show)
{
  b->show_hidden = show;
  if (b->model != nullptr)
    gbk_dir_model_set_show_hidden (b->model, show);
}

void
gbk_browser_set_sort (GbkBrowser *b, GbkSortMode sort)
{
  b->sort = sort;
  if (b->model != nullptr)
    gbk_dir_model_set_sort (b->model, sort);
}

/* ---- GObject ---- */

static void
on_view_status (GbkView *v G_GNUC_UNUSED, GbkBrowser *b)
{
  g_clear_pointer (&b->message, g_free);   /* a new selection replaces any message */
  status_changed (b);
}

static void
on_pathbar_navigate (GbkPathbar *bar G_GNUC_UNUSED, GbkLoc *loc, GbkBrowser *b)
{
  gbk_browser_open (b, loc);
  gtk_widget_grab_focus (GTK_WIDGET (b->view));
}

static void
on_pathbar_address (GbkPathbar *bar G_GNUC_UNUSED, const GbkAddress *a, GbkBrowser *b)
{
  gbk_browser_open_address (b, a);
  gtk_widget_grab_focus (GTK_WIDGET (b->view));
}

static void
gbk_browser_dispose (GObject *o)
{
  GbkBrowser *b = GBK_BROWSER (o);
  clear_pending (b);
  if (b->model != nullptr)
    set_model (b, nullptr);
  G_OBJECT_CLASS (gbk_browser_parent_class)->dispose (o);
}

static void
gbk_browser_finalize (GObject *o)
{
  GbkBrowser *b = GBK_BROWSER (o);
  g_ptr_array_unref (b->back);
  g_ptr_array_unref (b->forward);
  g_clear_pointer (&b->loc, gbk_loc_unref);
  g_free (b->focus_on_load);
  g_free (b->message);
  g_clear_pointer (&b->pending_select, g_ptr_array_unref);
  gbk_context_unref (b->ctx);
  G_OBJECT_CLASS (gbk_browser_parent_class)->finalize (o);
}

static void
gbk_browser_class_init (GbkBrowserClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = gbk_browser_dispose;
  G_OBJECT_CLASS (klass)->finalize = gbk_browser_finalize;
  signals[SIGNAL_PATH_CHANGED]
      = g_signal_new ("path-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                      nullptr, nullptr, G_TYPE_NONE, 0);
  signals[SIGNAL_STATUS_CHANGED]
      = g_signal_new ("status-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                      nullptr, nullptr, G_TYPE_NONE, 0);
}

static void
gbk_browser_init (GbkBrowser *b)
{
  b->back = gbk_loc_array_new ();
  b->forward = gbk_loc_array_new ();
}

GbkBrowser *
gbk_browser_new (GbkContext *ctx, bool show_hidden, GbkSortMode sort)
{
  GbkBrowser *b = g_object_new (GBK_TYPE_BROWSER, "orientation", GTK_ORIENTATION_VERTICAL, nullptr);
  b->ctx = gbk_context_ref (ctx);
  b->show_hidden = show_hidden;
  b->sort = sort;
  b->toolbar = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4));
  b->pathbar = GBK_PATHBAR (gbk_pathbar_new ());
  gtk_box_pack_start (b->toolbar, GTK_WIDGET (b->pathbar), true, true, 0);
  /* Flush with the tab strip and window edges; a gap below separates it from the files. */
  gtk_widget_set_margin_bottom (GTK_WIDGET (b->toolbar), 4);
  gtk_box_pack_start (GTK_BOX (b), GTK_WIDGET (b->toolbar), false, false, 0);
  gbk_connect (G_OBJECT (b), b->pathbar, "navigate", G_CALLBACK (on_pathbar_navigate), b);
  gbk_connect (G_OBJECT (b), b->pathbar, "navigate-address", G_CALLBACK (on_pathbar_address), b);

  /* Shown while a folder's machine can't be reached. */
  b->bar = GTK_INFO_BAR (gtk_info_bar_new ());
  gtk_info_bar_set_message_type (b->bar, GTK_MESSAGE_WARNING);
  b->bar_label = GTK_LABEL (gtk_label_new (""));
  gtk_label_set_xalign (b->bar_label, 0);
  gtk_label_set_line_wrap (b->bar_label, true);
  gtk_container_add (GTK_CONTAINER (gtk_info_bar_get_content_area (b->bar)),
                     GTK_WIDGET (b->bar_label));
  b->login_button = gtk_info_bar_add_button (b->bar, "_Log in in a terminal", RESPONSE_LOGIN);
  gtk_info_bar_add_button (b->bar, "_Retry", RESPONSE_RETRY);
  gtk_widget_set_no_show_all (GTK_WIDGET (b->bar), true);
  gtk_widget_show_all (gtk_info_bar_get_content_area (b->bar));
  gtk_box_pack_start (GTK_BOX (b), GTK_WIDGET (b->bar), false, false, 0);
  gbk_connect (G_OBJECT (b), b->bar, "response", G_CALLBACK (on_bar_response), b);

  GtkWidget *scroller = gtk_scrolled_window_new (nullptr, nullptr);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_AUTOMATIC,
                                  GTK_POLICY_NEVER);
  /* An overlay scrollbar would sit on the bottom row of items (the view fills its height
   * with rows); a real one takes its own space. */
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (scroller), false);
  b->view = GBK_VIEW (gbk_view_new (ctx->types));
  gtk_container_add (GTK_CONTAINER (scroller), GTK_WIDGET (b->view));
  gtk_box_pack_start (GTK_BOX (b), scroller, true, true, 0);
  gbk_connect (G_OBJECT (b), b->view, "selection-changed", G_CALLBACK (on_view_status), b);
  gbk_connect (G_OBJECT (b), b->view, "search-changed", G_CALLBACK (on_view_status), b);
  return b;
}
