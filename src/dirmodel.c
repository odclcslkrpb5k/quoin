#include "dirmodel.h"
#include "lifetime.h"
#include "natsort.h"
#include "remote.h"

/* While a directory is still loading, re-sort for display at most this often (the first
 * chunk is always shown at once). Each re-sort is O(n log n) over everything so far. */
#define LOADING_REFRESH_US (INT64_C (100) * 1000)

/* Messages handled per main-loop wakeup before yielding (keeps the UI responsive while a
 * huge directory streams in). */
#define DRAIN_BUDGET 16
#define RETRY_FIRST_MS 1000          /* a lost connection: try again after this, */
#define RETRY_MAX_MS 30000           /* doubling up to this */

#define NO_POS UINT32_MAX

typedef struct
{
  GbkEntry *e;          /* nullptr: free slot */
  uint32_t gen;         /* bumped when the slot is freed; never 0 */
  uint32_t seen;        /* snapshot epoch that last delivered this entry */
  uint32_t touched;     /* batch stamp: changed in the current batch */
  uint32_t pos;         /* index in `order`, or NO_POS */
} Slot;

struct _GbkDirModel
{
  GObject parent_instance;

  GbkLoc *loc;           /* nullptr when detached */
  GbkTypeTable *types;
  GbkScanner *scanner;   /* nullptr once finished, failed, or detached */
  guint watch_id;

  /* A folder on another machine: its listing comes through a connection instead. */
  GbkRemote *remote;     /* referenced; nullptr for a local folder */
  GbkConn *conn;         /* while watching */
  GbkConnWatch *cwatch;
  guint retry_id;
  int retry_ms;
  char *conn_error;      /* DISCONNECTED: why */
  bool needs_login;

  GbkDirState state;
  int error;
  bool live;
  bool has_free;
  uint64_t free_bytes;

  GbkSortMode sort;
  bool show_hidden;

  GArray *slots;         /* Slot */
  GArray *free_slots;    /* uint32_t */
  GHashTable *index;     /* raw name (borrowed from entry) -> slot + 1 */
  size_t n_entries;

  GArray *order;         /* uint32_t slot ids, visible, sorted */

  uint32_t epoch;        /* current snapshot */
  uint32_t batch;        /* current change batch stamp */
  GArray *touched;       /* uint32_t slot ids changed in this batch */
  bool order_stale;      /* re-sort deferred while loading: `order` lacks recent entries */
  bool painted;          /* "changed" has been emitted at least once */
  int64_t last_order_us; /* when `order` was last brought up to date */
};

G_DEFINE_FINAL_TYPE (GbkDirModel, gbk_dir_model, G_TYPE_OBJECT)

enum
{
  SIGNAL_CHANGED,
  SIGNAL_STATE_CHANGED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* ---- handles ---- */

static inline GbkHandle
make_handle (uint32_t slot, uint32_t gen)
{
  return ((GbkHandle) gen << 32) | slot;
}

static Slot *
slot_at (GbkDirModel *m, uint32_t id)
{
  g_assert (id < m->slots->len);
  return &g_array_index (m->slots, Slot, id);
}

/* Slot for a handle, or nullptr if stale/invalid. */
static Slot *
resolve (GbkDirModel *m, GbkHandle h)
{
  uint32_t id = (uint32_t) (h & 0xffffffffu);
  uint32_t gen = (uint32_t) (h >> 32);
  if (gen == 0 || id >= m->slots->len)
    return nullptr;
  Slot *s = slot_at (m, id);
  return (s->e != nullptr && s->gen == gen) ? s : nullptr;
}

/* ---- ordering ---- */

static bool
is_visible (const GbkDirModel *m, const GbkEntry *e)
{
  return e != nullptr && (m->show_hidden || !(e->flags & GBK_ENTRY_HIDDEN));
}

static int
compare_entries (const GbkEntry *a, const GbkEntry *b, GbkSortMode mode)
{
  bool da = gbk_entry_is_dir (a), db = gbk_entry_is_dir (b);
  if (da != db)
    return da ? -1 : 1;
  if (mode == GBK_SORT_MTIME && a->mtime_ns != b->mtime_ns)
    return a->mtime_ns > b->mtime_ns ? -1 : 1;
  if (mode == GBK_SORT_SIZE && !da && a->size != b->size)
    return a->size > b->size ? -1 : 1;
  int c = gbk_natural_compare (gbk_entry_key (a), a->key_len, gbk_entry_key (b), b->key_len);
  if (c != 0)
    return c;
  /* Raw names are unique in a directory, so this makes the order total. */
  c = memcmp (gbk_entry_name (a), gbk_entry_name (b), MIN (a->name_len, b->name_len) + 1);
  return (c > 0) - (c < 0);
}

static int
compare_slots (const void *pa, const void *pb, void *data)
{
  GbkDirModel *m = data;
  uint32_t a = *(const uint32_t *) pa, b = *(const uint32_t *) pb;
  return compare_entries (slot_at (m, a)->e, slot_at (m, b)->e, m->sort);
}

static void
update_positions (GbkDirModel *m, size_t from)
{
  for (size_t i = from; i < m->order->len; i++)
    slot_at (m, g_array_index (m->order, uint32_t, i))->pos = (uint32_t) i;
}

typedef struct
{
  const GbkEntry *e;
  uint32_t id;
} SortItem;

static int
compare_items (const void *pa, const void *pb, void *data)
{
  const SortItem *a = pa, *b = pb;
  return compare_entries (a->e, b->e, *(const GbkSortMode *) data);
}

static void
build_full_order (GbkDirModel *m, GArray *out)
{
  /* Sort (entry, id) pairs so the comparator touches entries directly, not via slots. */
  g_autoptr (GArray) items = g_array_sized_new (false, false, sizeof (SortItem), m->slots->len);
  for (uint32_t i = 0; i < m->slots->len; i++)
    {
      const GbkEntry *e = slot_at (m, i)->e;
      if (is_visible (m, e))
        {
          SortItem it = { e, i };
          g_array_append_val (items, it);
        }
    }
  g_sort_array (items->data, items->len, sizeof (SortItem), compare_items, &m->sort);
  g_array_set_size (out, items->len);
  for (guint i = 0; i < items->len; i++)
    g_array_index (out, uint32_t, i) = g_array_index (items, SortItem, i).id;
}

static void
rebuild_order (GbkDirModel *m)
{
  for (uint32_t i = 0; i < m->slots->len; i++)
    slot_at (m, i)->pos = NO_POS;
  build_full_order (m, m->order);
  update_positions (m, 0);
}

/* Binary search for where slot `id` belongs in the current order. */
static size_t
insertion_point (GbkDirModel *m, uint32_t id)
{
  size_t lo = 0, hi = m->order->len;
  while (lo < hi)
    {
      size_t mid = lo + (hi - lo) / 2;
      uint32_t at = g_array_index (m->order, uint32_t, mid);
      if (compare_slots (&at, &id, m) < 0)
        lo = mid + 1;
      else
        hi = mid;
    }
  return lo;
}

/* Re-sort only the slots touched in this batch: pull them out, binary-insert them back. */
static void
incremental_order (GbkDirModel *m)
{
  size_t w = 0;
  size_t first_change = m->order->len;
  for (size_t r = 0; r < m->order->len; r++)
    {
      uint32_t id = g_array_index (m->order, uint32_t, r);
      if (slot_at (m, id)->touched == m->batch)
        {
          slot_at (m, id)->pos = NO_POS;
          first_change = MIN (first_change, r);
          continue;
        }
      g_array_index (m->order, uint32_t, w++) = id;
    }
  g_array_set_size (m->order, (guint) w);

  for (size_t i = 0; i < m->touched->len; i++)
    {
      uint32_t id = g_array_index (m->touched, uint32_t, i);
      if (!is_visible (m, slot_at (m, id)->e))
        continue;
      size_t at = insertion_point (m, id);
      g_array_insert_val (m->order, (guint) at, id);
      first_change = MIN (first_change, at);
    }
  update_positions (m, first_change);
}

/* ---- applying changes ---- */

static void
touch (GbkDirModel *m, uint32_t id)
{
  Slot *s = slot_at (m, id);
  if (s->touched == m->batch)
    return;
  s->touched = m->batch;
  g_array_append_val (m->touched, id);
}

static void
remove_slot (GbkDirModel *m, uint32_t id)
{
  Slot *s = slot_at (m, id);
  g_assert (s->e != nullptr);
  touch (m, id);
  g_hash_table_remove (m->index, gbk_entry_name (s->e));
  g_clear_pointer (&s->e, gbk_entry_free);
  if (++s->gen == 0)
    s->gen = 1;
  g_array_append_val (m->free_slots, id);
  m->n_entries--;
}

static void
upsert (GbkDirModel *m, GbkEntry *e)
{
  const void *found = g_hash_table_lookup (m->index, gbk_entry_name (e));
  uint32_t id;
  if (found != nullptr)
    {
      id = GPOINTER_TO_UINT (found) - 1;
      Slot *s = slot_at (m, id);
      /* The index key points into the old entry: drop it before freeing that entry. */
      g_hash_table_remove (m->index, gbk_entry_name (s->e));
      gbk_entry_free (s->e);
      s->e = e;
    }
  else
    {
      if (m->free_slots->len > 0)
        {
          id = g_array_index (m->free_slots, uint32_t, m->free_slots->len - 1);
          g_array_set_size (m->free_slots, m->free_slots->len - 1);
        }
      else
        {
          if (m->slots->len >= UINT32_MAX - 1)
            gbk_fatal ("directory has too many entries");
          id = m->slots->len;
          Slot fresh = { .gen = 1, .pos = NO_POS };
          g_array_append_val (m->slots, fresh);
        }
      slot_at (m, id)->e = e;
      m->n_entries++;
    }
  g_hash_table_insert (m->index, e->data /* the raw name */, GUINT_TO_POINTER (id + 1));
  slot_at (m, id)->seen = m->epoch;
  touch (m, id);
}

/* Takes ownership of every entry, in order (a later duplicate name wins). */
static void
upsert_all (GbkDirModel *m, GPtrArray *entries)
{
  gsize n = 0;
  void **taken = g_ptr_array_steal (entries, &n);
  for (gsize i = 0; i < n; i++)
    upsert (m, taken[i]);
  g_free (taken);
}

static void
remove_name (GbkDirModel *m, const char *name)
{
  const void *found = g_hash_table_lookup (m->index, name);
  if (found != nullptr)
    remove_slot (m, GPOINTER_TO_UINT (found) - 1);
}

static void
set_state (GbkDirModel *m, GbkDirState state)
{
  if (m->state == state)
    return;
  m->state = state;
  g_signal_emit (m, signals[SIGNAL_STATE_CHANGED], 0);
}

static void
stop_scanner (GbkDirModel *m)
{
  /* Cancel the watch before the scanner can close the fd it watches. */
  gbk_source_cancel (&m->watch_id);
  g_clear_pointer (&m->scanner, gbk_scanner_stop);
  g_clear_pointer (&m->cwatch, gbk_conn_unwatch);
  g_clear_object (&m->conn);
}

/* Applies one message. Returns true if entries changed. */
static bool
apply (GbkDirModel *m, GbkScanMsg *msg)
{
  bool free_changed = false;
  if (msg->has_free && (!m->has_free || m->free_bytes != msg->free_bytes))
    {
      m->has_free = true;
      m->free_bytes = msg->free_bytes;
      free_changed = true;
    }

  switch (msg->type)
    {
    case GBK_SCAN_CHUNK:
      if (msg->first)
        {
          m->epoch++;
          m->live = msg->live;
        }
      upsert_all (m, msg->upserts);
      if (msg->last)
        {
          /* Sweep: anything this snapshot didn't deliver no longer exists. */
          for (uint32_t id = 0; id < m->slots->len; id++)
            if (slot_at (m, id)->e != nullptr && slot_at (m, id)->seen != m->epoch)
              remove_slot (m, id);
        }
      return true;

    case GBK_SCAN_DELTA:
      for (guint i = 0; i < msg->removes->len; i++)
        remove_name (m, g_ptr_array_index (msg->removes, i));
      upsert_all (m, msg->upserts);
      return true;

    case GBK_SCAN_ERROR:
      m->error = msg->error;
      m->live = false;
      return free_changed;

    case GBK_SCAN_GONE: m->live = false; return free_changed;

    default: gbk_fatal ("bad scan message type %d", (int) msg->type);
    }
}

static GbkDirState
state_after (const GbkScanMsg *msg, GbkDirState current)
{
  switch (msg->type)
    {
    case GBK_SCAN_CHUNK: return msg->last ? GBK_DIR_READY : current;
    case GBK_SCAN_DELTA: return current;
    case GBK_SCAN_ERROR: return GBK_DIR_ERROR;
    case GBK_SCAN_GONE: return GBK_DIR_GONE;
    default: gbk_fatal ("bad scan message type %d", (int) msg->type);
    }
}

static void
begin_batch (GbkDirModel *m)
{
  if (++m->batch == 0)
    {
      /* Stamp wrapped: clear stale stamps so none collides with the new one. */
      for (uint32_t i = 0; i < m->slots->len; i++)
        slot_at (m, i)->touched = 0;
      m->batch = 1;
    }
  g_array_set_size (m->touched, 0);
}

static void
end_batch (GbkDirModel *m, bool changed, bool loading)
{
  if (!changed)
    return;
  int64_t now = g_get_monotonic_time ();
  /* Big directory still streaming in: show the first chunk at once, then refresh at a
   * bounded rate. Deferring is safe only while loading: chunks only add or replace entries,
   * so the stale order still references live slots. The last chunk ends LOADING, so the
   * final order is always computed. */
  if (loading && m->painted && now - m->last_order_us < LOADING_REFRESH_US)
    {
      m->order_stale = true;
      return;
    }
  /* Incremental costs O(n) plus a memmove per touched entry; a full sort is O(n log n).
   * Past a few percent of the directory, sorting from scratch is cheaper. */
  if (m->order_stale || m->touched->len > 64 + m->order->len / 16)
    rebuild_order (m);
  else
    incremental_order (m);
  m->order_stale = false;
  m->painted = true;
  m->last_order_us = now;
  g_signal_emit (m, signals[SIGNAL_CHANGED], 0);
}

/* Applies a message. The resulting state is returned via `next`, not set: callers announce
 * it only after end_batch, so a "state-changed" listener always sees an up-to-date order
 * (e.g. READY means every entry is already visible). */
static void
handle_message (GbkDirModel *m, GbkScanMsg *msg, bool *changed, GbkDirState *next)
{
  *changed |= apply (m, msg);
  *next = state_after (msg, *next);
  bool final = msg->type == GBK_SCAN_ERROR || msg->type == GBK_SCAN_GONE;
  gbk_scan_msg_free (msg);
  if (final)
    stop_scanner (m);
}

/* Order first, then announce the state: a READY listener must see every entry. */
static void
finish_batch (GbkDirModel *m, bool changed, GbkDirState next)
{
  end_batch (m, changed, next == GBK_DIR_LOADING);
  set_state (m, next);
}

static bool
on_scanner_wake (int fd G_GNUC_UNUSED, GIOCondition condition G_GNUC_UNUSED, void *data)
{
  GbkDirModel *m = data;
  gbk_scanner_ack (m->scanner);

  begin_batch (m);
  bool changed = false;
  GbkDirState next = m->state;
  for (int n = 0; n < DRAIN_BUDGET && m->scanner != nullptr; n++)
    {
      GbkScanMsg *msg = gbk_scanner_pop (m->scanner);
      if (msg == nullptr)
        break;
      handle_message (m, msg, &changed, &next);
      if (n == DRAIN_BUDGET - 1 && m->scanner != nullptr)
        gbk_scanner_rearm (m->scanner);     /* more may be waiting: come back next iteration */
    }
  bool alive = m->scanner != nullptr;
  finish_batch (m, changed, next);
  return alive;
}

/* ---- GObject ---- */

static void
gbk_dir_model_dispose (GObject *object)
{
  GbkDirModel *m = GBK_DIR_MODEL (object);
  stop_scanner (m);
  gbk_source_cancel (&m->retry_id);
  g_clear_object (&m->remote);
  G_OBJECT_CLASS (gbk_dir_model_parent_class)->dispose (object);
}

static void
gbk_dir_model_finalize (GObject *object)
{
  GbkDirModel *m = GBK_DIR_MODEL (object);
  for (uint32_t i = 0; i < m->slots->len; i++)
    g_clear_pointer (&slot_at (m, i)->e, gbk_entry_free);
  g_hash_table_unref (m->index);
  g_array_unref (m->slots);
  g_array_unref (m->free_slots);
  g_array_unref (m->order);
  g_array_unref (m->touched);
  gbk_type_table_unref (m->types);
  g_clear_pointer (&m->loc, gbk_loc_unref);
  g_free (m->conn_error);
  G_OBJECT_CLASS (gbk_dir_model_parent_class)->finalize (object);
}

static void
gbk_dir_model_class_init (GbkDirModelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = gbk_dir_model_dispose;
  object_class->finalize = gbk_dir_model_finalize;

  signals[SIGNAL_CHANGED] = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                          0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  signals[SIGNAL_STATE_CHANGED]
      = g_signal_new ("state-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                      nullptr, nullptr, G_TYPE_NONE, 0);
}

static void
gbk_dir_model_init (GbkDirModel *m)
{
  m->retry_ms = RETRY_FIRST_MS;
  m->slots = g_array_new (false, false, sizeof (Slot));
  m->free_slots = g_array_new (false, false, sizeof (uint32_t));
  m->order = g_array_new (false, false, sizeof (uint32_t));
  m->touched = g_array_new (false, false, sizeof (uint32_t));
  m->index = g_hash_table_new (g_str_hash, g_str_equal);
  m->state = GBK_DIR_LOADING;
  m->sort = GBK_SORT_NAME;
}

/* ---- a folder on another machine ---- */

static void start_remote (GbkDirModel *m);

static bool
retry_now (void *data)
{
  GbkDirModel *m = data;
  m->retry_id = 0;
  start_remote (m);
  return false;
}

/* The connection died under the watch: keep the listing, say so, try again later. */
static void
lost (GbkDirModel *m)
{
  g_free (m->conn_error);
  m->conn_error = g_strdup (m->conn != nullptr && gbk_conn_error (m->conn) != nullptr
                                ? gbk_conn_error (m->conn)
                                : "The connection ended");
  m->needs_login = m->conn != nullptr && gbk_conn_needs_login (m->conn);
  stop_scanner (m);
  m->live = false;
  gbk_source_cancel (&m->retry_id);
  m->retry_id = gbk_timeout_add (G_OBJECT (m), (guint) m->retry_ms, retry_now, m);
  m->retry_ms = MIN (m->retry_ms * 2, RETRY_MAX_MS);
  set_state (m, GBK_DIR_DISCONNECTED);
}

static void
on_remote_scan (GbkScanMsg *msg, void *data)
{
  GbkDirModel *m = data;
  if (msg == nullptr)
    {
      lost (m);
      return;
    }
  if (msg->type == GBK_SCAN_CHUNK && msg->last)
    m->retry_ms = RETRY_FIRST_MS;      /* a whole listing: the connection works again */
  begin_batch (m);
  bool changed = false;
  GbkDirState next = m->state;
  handle_message (m, msg, &changed, &next);
  finish_batch (m, changed, next);
}

static void
start_remote (GbkDirModel *m)
{
  if (m->cwatch != nullptr)
    return;
  g_autoptr (GError) error = nullptr;
  m->conn = gbk_remote_conn (m->remote, gbk_loc_host (m->loc), &error);
  if (m->conn == nullptr)
    {
      m->state = GBK_DIR_ERROR;        /* a host gbk_host_valid() refused: can't happen */
      m->error = EINVAL;
      return;
    }
  m->cwatch = gbk_conn_watch (m->conn, gbk_loc_path (m->loc), G_OBJECT (m), on_remote_scan, m);
}

static void
on_network_up (GbkRemote *r G_GNUC_UNUSED, GbkDirModel *m)
{
  gbk_dir_model_reconnect (m);
}

void
gbk_dir_model_reconnect (GbkDirModel *m)
{
  if (m->state != GBK_DIR_DISCONNECTED)
    return;
  gbk_source_cancel (&m->retry_id);
  m->retry_ms = RETRY_FIRST_MS;
  start_remote (m);
}

const char *
gbk_dir_model_conn_error (const GbkDirModel *m)
{
  return m->state == GBK_DIR_DISCONNECTED ? m->conn_error : nullptr;
}

bool
gbk_dir_model_needs_login (const GbkDirModel *m)
{
  return m->state == GBK_DIR_DISCONNECTED && m->needs_login;
}

bool
gbk_dir_model_connecting (const GbkDirModel *m)
{
  return m->conn != nullptr && gbk_conn_state (m->conn) == GBK_CONN_CONNECTING;
}

GbkDirModel *
gbk_dir_model_new_detached (GbkTypeTable *types)
{
  GbkDirModel *m = g_object_new (GBK_TYPE_DIR_MODEL, nullptr);
  m->types = gbk_type_table_ref (types);
  return m;
}

GbkDirModel *
gbk_dir_model_new (GbkLoc *loc, GbkTypeTable *types)
{
  GbkDirModel *m = gbk_dir_model_new_detached (types);
  m->loc = gbk_loc_ref (loc);
  const char *path;
  if (!gbk_loc_as_local (loc, &path))
    {
      GbkRemote *r = gbk_remote_default ();
      if (r == nullptr)
        {
          m->state = GBK_DIR_ERROR;    /* no way to reach other machines in this process */
          m->error = EREMOTE;
          return m;
        }
      m->remote = g_object_ref (r);
      gbk_connect (G_OBJECT (m), r, "network-up", G_CALLBACK (on_network_up), m);
      start_remote (m);
      return m;
    }
  m->scanner = gbk_scanner_start (path, types);
  m->watch_id
      = gbk_fd_watch (G_OBJECT (m), gbk_scanner_wake_fd (m->scanner), G_IO_IN, on_scanner_wake, m);
  return m;
}

void
gbk_dir_model_apply_for_test (GbkDirModel *m, GbkScanMsg *msg)
{
  g_assert (m->scanner == nullptr);
  begin_batch (m);
  bool changed = false;
  GbkDirState next = m->state;
  handle_message (m, msg, &changed, &next);
  finish_batch (m, changed, next);
}

void
gbk_dir_model_check_order (GbkDirModel *m)
{
  if (m->order_stale)
    return;               /* deliberately behind while loading; see end_batch */
  g_autoptr (GArray) expected = g_array_new (false, false, sizeof (uint32_t));
  build_full_order (m, expected);
  /* An empty GArray's data may be nullptr, and memcmp(nullptr, ..., 0) is UB. */
  if (expected->len != m->order->len
      || (expected->len > 0
          && memcmp (expected->data, m->order->data, expected->len * sizeof (uint32_t)) != 0))
    gbk_fatal ("incremental order diverged from full sort (%u vs %u entries)", m->order->len,
               expected->len);
  for (uint32_t i = 0; i < m->order->len; i++)
    if (slot_at (m, g_array_index (m->order, uint32_t, i))->pos != i)
      gbk_fatal ("position map out of date at %u", i);
  size_t live = 0;
  for (uint32_t i = 0; i < m->slots->len; i++)
    {
      const Slot *s = slot_at (m, i);
      if (s->e != nullptr)
        live++;
      if (s->e == nullptr && s->pos != NO_POS)
        gbk_fatal ("free slot %u has a position", i);
    }
  if (live != m->n_entries || g_hash_table_size (m->index) != m->n_entries)
    gbk_fatal ("entry count mismatch");
}

/* ---- accessors ---- */

const GbkLoc *
gbk_dir_model_loc (const GbkDirModel *m)
{
  return m->loc;
}

GbkDirState
gbk_dir_model_state (const GbkDirModel *m)
{
  return m->state;
}

int
gbk_dir_model_error (const GbkDirModel *m)
{
  return m->error;
}

bool
gbk_dir_model_live (const GbkDirModel *m)
{
  return m->live;
}

bool
gbk_dir_model_free_space (const GbkDirModel *m, uint64_t *bytes)
{
  *bytes = m->free_bytes;
  return m->has_free;
}

void
gbk_dir_model_set_sort (GbkDirModel *m, GbkSortMode mode)
{
  if (m->sort == mode)
    return;
  m->sort = mode;
  rebuild_order (m);
  g_signal_emit (m, signals[SIGNAL_CHANGED], 0);
}

GbkSortMode
gbk_dir_model_sort (const GbkDirModel *m)
{
  return m->sort;
}

void
gbk_dir_model_set_show_hidden (GbkDirModel *m, bool show)
{
  if (m->show_hidden == show)
    return;
  m->show_hidden = show;
  rebuild_order (m);
  g_signal_emit (m, signals[SIGNAL_CHANGED], 0);
}

bool
gbk_dir_model_show_hidden (const GbkDirModel *m)
{
  return m->show_hidden;
}

size_t
gbk_dir_model_n_entries (const GbkDirModel *m)
{
  return m->n_entries;
}

size_t
gbk_dir_model_n_visible (GbkDirModel *m)
{
  return m->order->len;
}

GbkHandle
gbk_dir_model_visible_handle (GbkDirModel *m, size_t i)
{
  if (i >= m->order->len)
    gbk_fatal ("visible index %zu out of range (%u)", i, m->order->len);
  uint32_t id = g_array_index (m->order, uint32_t, i);
  return make_handle (id, slot_at (m, id)->gen);
}

size_t
gbk_dir_model_visible_index (GbkDirModel *m, GbkHandle h)
{
  Slot *s = resolve (m, h);
  return (s == nullptr || s->pos == NO_POS) ? SIZE_MAX : s->pos;
}

const GbkEntry *
gbk_dir_model_get (GbkDirModel *m, GbkHandle h)
{
  Slot *s = resolve (m, h);
  return s != nullptr ? s->e : nullptr;
}

GbkHandle
gbk_dir_model_find (GbkDirModel *m, const char *raw_name)
{
  const void *found = g_hash_table_lookup (m->index, raw_name);
  if (found == nullptr)
    return GBK_HANDLE_NONE;
  uint32_t id = GPOINTER_TO_UINT (found) - 1;
  return make_handle (id, slot_at (m, id)->gen);
}
