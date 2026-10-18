#include "view.h"
#include "cliptext.h"
#include "dnd.h"
#include "lifetime.h"
#include "natsort.h"

#define ICON_SIZE 16
#define PAD_X 4
#define PAD_Y 1
#define ICON_GAP 4
#define EMBLEM_SIZE 10
#define MIN_COL_W 48
#define TEXT_SLACK 3            /* px added to estimated widths (kerning can widen a bit) */
#define SEARCH_TIMEOUT_MS 1000
#define WHEEL_STEP_PX 64
#define DND_EDGE_PX 24          /* hovering a drag this close to a side scrolls that way */
#define DND_SCROLL_MS 40
#define DND_SCROLL_PX 24

static_assert (sizeof (void *) >= sizeof (GbkHandle), "handles are stored as pointers");

typedef struct
{
  uint32_t gen;                 /* 0: not measured */
  uint16_t width;
} WidthCache;

struct _GbkView
{
  GtkWidget parent_instance;

  GbkTypeTable *types;
  GbkDirModel *model;
  gulong model_changed_id;

  GtkAdjustment *hadj, *vadj;
  gulong hadj_changed_id;
  /* Scroll position as content, not pixels: the item at the top of the leftmost visible
   * column and how far into that column, or "at the far end". Every layout restores it, so
   * a passing layout (GTK allocates once without the scrollbar, then with it) or a resize
   * can't move what's on screen. Updated only by real scrolling, never by our own
   * reconfiguring (`adjusting`). */
  size_t anchor_index;
  int anchor_offset;
  bool anchor_end;
  bool adjusting;
  guint hscroll_policy, vscroll_policy;

  /* Metrics (style_updated). */
  PangoLayout *layout;
  int ascii_adv[128];           /* Pango units; 0 for control chars */
  int text_h, row_h;

  /* Layout (relayout). */
  size_t rows;
  GArray *col_x;                /* int: start x of each column, plus the total at the end */
  GArray *widths;               /* WidthCache, indexed by model slot: valid for `model` only
                   * (slots and generations restart in every model) */

  /* Selection. */
  GHashTable *selected;         /* GbkHandle set */
  GbkHandle cursor, anchor;
  size_t cursor_hint;           /* last index of the cursor, to recover if it vanishes */

  /* Type-ahead. */
  GString *search;
  guint search_timeout;

  /* Pressing on an item: a drag may start; pressing on a selected one selects only it on
   * release unless it became a drag (so several selected items can be dragged). */
  bool press_armed;
  bool press_pending;
  double press_x, press_y;
  GbkHandle press_item;
  GPtrArray *drag_locs;         /* what's being dragged out (GbkLoc*) */

  /* Being dropped onto: the highlighted folder item, or the whole view (its folder). */
  bool drop_hover;
  GbkHandle drop_folder;        /* GBK_HANDLE_NONE: the view's own folder */
  bool drop_requested;          /* we asked for the data of a drop */
  int scroll_dir;
  guint autoscroll;

  /* Icons, per type id, at the current scale. */
  GPtrArray *icons;             /* cairo_surface_t* or nullptr */
  GArray *icons_tried;          /* bool */
  GHashTable *marked; /* names (char*) open for editing elsewhere (edits.h); may be nullptr */
  cairo_surface_t *emblem; /* drawn on a marked item's icon */
  bool emblem_tried;
  int icon_scale;
};

enum
{
  PROP_0,
  PROP_HADJUSTMENT,
  PROP_VADJUSTMENT,
  PROP_HSCROLL_POLICY,
  PROP_VSCROLL_POLICY,
};

enum
{
  SIGNAL_ACTIVATE,
  SIGNAL_SELECTION_CHANGED,
  SIGNAL_CONTEXT_MENU,
  SIGNAL_SEARCH_CHANGED,
  SIGNAL_DROP,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE_WITH_CODE (GbkView, gbk_view, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, nullptr))

/* ---- small helpers ---- */

static size_t
n_items (GbkView *v)
{
  return v->model != nullptr ? gbk_dir_model_n_visible (v->model) : 0;
}

static GbkHandle
handle_at_index (GbkView *v, size_t i)
{
  return gbk_dir_model_visible_handle (v->model, i);
}

static size_t
index_of (GbkView *v, GbkHandle h)
{
  return v->model != nullptr ? gbk_dir_model_visible_index (v->model, h) : SIZE_MAX;
}

static size_t
n_columns (GbkView *v)
{
  return v->col_x->len > 0 ? v->col_x->len - 1 : 0;
}

static int
col_start (GbkView *v, size_t c)
{
  return g_array_index (v->col_x, int, c);
}

static int
content_width (GbkView *v)
{
  return v->col_x->len > 0 ? col_start (v, v->col_x->len - 1) : 0;
}

static int
scroll_x (GbkView *v)
{
  return v->hadj != nullptr ? (int) gtk_adjustment_get_value (v->hadj) : 0;
}

/* ---- metrics & widths ---- */

static void
update_metrics (GbkView *v)
{
  g_clear_object (&v->layout);
  v->layout = gtk_widget_create_pango_layout (GTK_WIDGET (v), nullptr);
  /* Filenames may contain newlines: never let them break the line. */
  pango_layout_set_single_paragraph_mode (v->layout, true);

  for (int c = 0; c < 128; c++)
    {
      v->ascii_adv[c] = 0;
      if (c < 32 || c == 127)
        continue;
      char s[2] = { (char) c, '\0' };
      pango_layout_set_text (v->layout, s, 1);
      PangoRectangle logical;
      pango_layout_get_extents (v->layout, nullptr, &logical);
      v->ascii_adv[c] = logical.width;
    }
  pango_layout_set_text (v->layout, "Xg", -1);
  int w, h;
  pango_layout_get_pixel_size (v->layout, &w, &h);
  v->text_h = h;
  v->row_h = MAX (ICON_SIZE, h) + 2 * PAD_Y;

  /* Fonts changed: every cached width is wrong. */
  g_array_set_size (v->widths, 0);
}

static int
measure_text (GbkView *v, const GbkEntry *e)
{
  const char *s = gbk_entry_display (e);
  int64_t units = 0;
  for (const unsigned char *p = (const unsigned char *) s; *p != '\0'; p++)
    {
      if (*p >= 128 || v->ascii_adv[*p] == 0)
        {
          /* Non-ASCII or control characters: let Pango shape it for real. */
          int w, h;
          pango_layout_set_text (v->layout, s, -1);
          pango_layout_get_pixel_size (v->layout, &w, &h);
          return w + TEXT_SLACK;
        }
      units += v->ascii_adv[*p];
    }
  return (int) MIN (PANGO_PIXELS_CEIL (units), G_MAXINT / 2) + TEXT_SLACK;
}

static int
text_width (GbkView *v, GbkHandle h, const GbkEntry *e)
{
  uint32_t slot = (uint32_t) (h & 0xffffffffu);
  uint32_t gen = (uint32_t) (h >> 32);
  if (slot >= v->widths->len)
    g_array_set_size (v->widths, slot + 1); /* new elements zeroed (clear = true) */
  WidthCache *wc = &g_array_index (v->widths, WidthCache, slot);
  if (wc->gen != gen)
    {
      wc->gen = gen;
      wc->width = (uint16_t) MIN (measure_text (v, e), UINT16_MAX);
    }
  return wc->width;
}

static int
max_column_width (GbkView *v)
{
  int alloc_w = gtk_widget_get_allocated_width (GTK_WIDGET (v));
  return MAX (240, alloc_w * 2 / 3);
}

/* ---- layout ---- */

static size_t column_at (GbkView *v, int cx);

static void
update_adjustment (GbkView *v)
{
  if (v->hadj == nullptr)
    return;
  double page = gtk_widget_get_allocated_width (GTK_WIDGET (v));
  double upper = MAX (content_width (v), page);
  double value = 0;
  size_t c = v->rows > 0 ? v->anchor_index / v->rows : 0;
  if (v->anchor_end)
    value = upper - page;
  else if (c < n_columns (v))
    value = col_start (v, c) + MIN (v->anchor_offset, col_start (v, c + 1) - col_start (v, c));
  value = CLAMP (value, 0, upper - page);
  v->adjusting = true;
  gtk_adjustment_configure (v->hadj, value, 0, upper, v->row_h * 2, page * 0.9, page);
  v->adjusting = false;
}

/* After the user (or scroll_to_index) moved the scroll position: remember it as content. */
static void
record_anchor (GbkView *v)
{
  double value = gtk_adjustment_get_value (v->hadj);
  double upper = gtk_adjustment_get_upper (v->hadj), page = gtk_adjustment_get_page_size (v->hadj);
  v->anchor_end = upper > page && value >= upper - page - 0.5;
  if (n_columns (v) == 0)
    {
      v->anchor_index = 0;
      v->anchor_offset = 0;
      return;
    }
  size_t c = column_at (v, (int) value);
  v->anchor_index = c * v->rows;
  v->anchor_offset = (int) value - col_start (v, c);
}

static void
reset_anchor (GbkView *v)
{
  v->anchor_index = 0;
  v->anchor_offset = 0;
  v->anchor_end = false;
}

static void
relayout (GbkView *v)
{
  /* Layout can be requested (set_model) before the first style_updated. */
  if (v->layout == nullptr)
    update_metrics (v);
  int height = gtk_widget_get_allocated_height (GTK_WIDGET (v));
  v->rows = (size_t) MAX (1, height / MAX (1, v->row_h));
  size_t n = n_items (v);
  size_t ncols = (n + v->rows - 1) / v->rows;
  int cap = max_column_width (v);

  g_array_set_size (v->col_x, (guint) ncols + 1);
  int x = 0;
  for (size_t c = 0; c < ncols; c++)
    {
      g_array_index (v->col_x, int, c) = x;
      int w = MIN_COL_W;
      size_t end = MIN (n, (c + 1) * v->rows);
      for (size_t i = c * v->rows; i < end; i++)
        {
          GbkHandle h = handle_at_index (v, i);
          const GbkEntry *e = gbk_dir_model_get (v->model, h);
          w = MAX (w, PAD_X + ICON_SIZE + ICON_GAP + text_width (v, h, e) + PAD_X);
        }
      int add = MIN (w, cap);
      x = x > G_MAXINT - add ? G_MAXINT : x + add; /* absurdly wide: saturate */
    }
  g_array_index (v->col_x, int, ncols) = x;
  update_adjustment (v);
}

/* Column containing content x coordinate `cx` (clamped). */
static size_t
column_at (GbkView *v, int cx)
{
  size_t lo = 0, hi = n_columns (v);
  while (lo + 1 < hi)
    {
      size_t mid = lo + (hi - lo) / 2;
      if (col_start (v, mid) <= cx)
        lo = mid;
      else
        hi = mid;
    }
  return lo;
}

static void
scroll_to_index (GbkView *v, size_t i)
{
  if (v->hadj == nullptr || v->rows == 0 || i >= n_items (v))
    return;
  size_t c = i / v->rows;
  if (c >= n_columns (v))
    return;
  double x0 = col_start (v, c), x1 = col_start (v, c + 1);
  double value = gtk_adjustment_get_value (v->hadj);
  double page = gtk_adjustment_get_page_size (v->hadj);
  if (x0 < value)
    gtk_adjustment_set_value (v->hadj, x0);
  else if (x1 > value + page)
    gtk_adjustment_set_value (v->hadj, MIN (x0, x1 - page));
}

/* ---- icons ---- */

static void
clear_icons (GbkView *v)
{
  g_clear_pointer (&v->emblem, cairo_surface_destroy);
  v->emblem_tried = false;
  for (guint i = 0; i < v->icons->len; i++)
    g_clear_pointer (&g_ptr_array_index (v->icons, i), cairo_surface_destroy);
  g_ptr_array_set_size (v->icons, 0);
  g_array_set_size (v->icons_tried, 0);
}

static cairo_surface_t *
icon_for (GbkView *v, GbkTypeId type)
{
  int scale = gtk_widget_get_scale_factor (GTK_WIDGET (v));
  if (scale != v->icon_scale)
    {
      clear_icons (v);
      v->icon_scale = scale;
    }
  if (type >= v->icons->len)
    {
      g_ptr_array_set_size (v->icons, type + 1);
      g_array_set_size (v->icons_tried, type + 1);
    }
  if (g_array_index (v->icons_tried, bool, type))
    return g_ptr_array_index (v->icons, type);
  g_array_index (v->icons_tried, bool, type) = true;

  const char *names[] = {
    gbk_type_table_icon (v->types, type),
    gbk_type_table_generic_icon (v->types, type),
    "text-x-generic",
    nullptr,
  };
  GtkIconTheme *theme = gtk_icon_theme_get_for_screen (gtk_widget_get_screen (GTK_WIDGET (v)));
  g_autoptr (GtkIconInfo) info = gtk_icon_theme_choose_icon_for_scale (
      theme, names, ICON_SIZE, scale, GTK_ICON_LOOKUP_FORCE_SIZE);
  if (info == nullptr)
    return nullptr;
  cairo_surface_t *surface
      = gtk_icon_info_load_surface (info, gtk_widget_get_window (GTK_WIDGET (v)), nullptr);
  g_ptr_array_index (v->icons, type) = surface;
  return surface;
}

/* The "being edited" emblem, drawn over the bottom right of an item's icon. */
static cairo_surface_t *
emblem (GbkView *v)
{
  if (v->emblem_tried)
    return v->emblem;
  v->emblem_tried = true;
  const char *names[]
      = { "document-edit-symbolic", "document-edit", "accessories-text-editor", nullptr };
  GtkIconTheme *theme = gtk_icon_theme_get_for_screen (gtk_widget_get_screen (GTK_WIDGET (v)));
  g_autoptr (GtkIconInfo) info = gtk_icon_theme_choose_icon_for_scale (
      theme, names, EMBLEM_SIZE, gtk_widget_get_scale_factor (GTK_WIDGET (v)),
      GTK_ICON_LOOKUP_FORCE_SIZE);
  if (info != nullptr)
    v->emblem = gtk_icon_info_load_surface (info, gtk_widget_get_window (GTK_WIDGET (v)), nullptr);
  return v->emblem;
}

void
gbk_view_set_marked (GbkView *v, GHashTable *names)
{
  g_clear_pointer (&v->marked, g_hash_table_unref);
  v->marked = names != nullptr ? g_hash_table_ref (names) : nullptr;
  gtk_widget_queue_draw (GTK_WIDGET (v));
}

bool
gbk_view_is_marked (const GbkView *v, GbkHandle h)
{
  const GbkEntry *e = v->model != nullptr ? gbk_dir_model_get (v->model, h) : nullptr;
  return e != nullptr && v->marked != nullptr
         && g_hash_table_contains (v->marked, gbk_entry_name (e));
}

static void
on_icon_theme_changed (GtkIconTheme *theme G_GNUC_UNUSED, GbkView *v)
{
  clear_icons (v);
  gtk_widget_queue_draw (GTK_WIDGET (v));
}

/* ---- selection ---- */

static void *
key_of (GbkHandle h)
{
  return (void *) (uintptr_t) h;
}

static void
emit_selection_changed (GbkView *v)
{
  g_signal_emit (v, signals[SIGNAL_SELECTION_CHANGED], 0);
  gtk_widget_queue_draw (GTK_WIDGET (v));
}

bool
gbk_view_is_selected (GbkView *v, GbkHandle h)
{
  return h != GBK_HANDLE_NONE && g_hash_table_contains (v->selected, key_of (h));
}

static void
set_cursor (GbkView *v, GbkHandle h)
{
  v->cursor = h;
  size_t i = index_of (v, h);
  if (i != SIZE_MAX)
    v->cursor_hint = i;
}

static void
select_range (GbkView *v, size_t a, size_t b)
{
  g_hash_table_remove_all (v->selected);
  for (size_t i = MIN (a, b); i <= MAX (a, b); i++)
    g_hash_table_add (v->selected, key_of (handle_at_index (v, i)));
}

void
gbk_view_select_only (GbkView *v, GbkHandle h)
{
  size_t i = index_of (v, h);
  if (i == SIZE_MAX)
    return;
  g_hash_table_remove_all (v->selected);
  g_hash_table_add (v->selected, key_of (h));
  set_cursor (v, h);
  v->anchor = h;
  scroll_to_index (v, i);
  emit_selection_changed (v);
}

void
gbk_view_set_selection (GbkView *v, const GbkHandle *handles, size_t n)
{
  g_hash_table_remove_all (v->selected);
  size_t first = SIZE_MAX;
  for (size_t i = 0; i < n; i++)
    {
      size_t at = index_of (v, handles[i]);
      if (at == SIZE_MAX)
        continue;
      g_hash_table_add (v->selected, key_of (handles[i]));
      first = MIN (first, at);
    }
  if (first != SIZE_MAX)
    {
      GbkHandle h = handle_at_index (v, first);
      set_cursor (v, h);
      v->anchor = h;
      scroll_to_index (v, first);
    }
  emit_selection_changed (v);
}

void
gbk_view_select_all (GbkView *v)
{
  size_t n = n_items (v);
  for (size_t i = 0; i < n; i++)
    g_hash_table_add (v->selected, key_of (handle_at_index (v, i)));
  emit_selection_changed (v);
}

void
gbk_view_unselect_all (GbkView *v)
{
  if (g_hash_table_size (v->selected) == 0)
    return;
  g_hash_table_remove_all (v->selected);
  emit_selection_changed (v);
}

size_t
gbk_view_n_selected (GbkView *v)
{
  return g_hash_table_size (v->selected);
}

GbkHandle *
gbk_view_selected (GbkView *v, size_t *n)
{
  size_t count = g_hash_table_size (v->selected);
  GbkHandle *out = g_new (GbkHandle, MAX (count, 1));
  size_t k = 0;
  size_t total = n_items (v);
  /* Walk the visible order so the result is sorted; stop once all are found. */
  for (size_t i = 0; i < total && k < count; i++)
    {
      GbkHandle h = handle_at_index (v, i);
      if (g_hash_table_contains (v->selected, key_of (h)))
        out[k++] = h;
    }
  *n = k;
  return out;
}

GbkHandle
gbk_view_cursor (const GbkView *v)
{
  return v->cursor;
}

/* Drop selected handles that no longer resolve or are no longer visible; recover the
 * cursor near where it was. Returns true if the selection changed. */
static bool
prune_selection (GbkView *v)
{
  bool changed = false;
  GHashTableIter it;
  void *key;
  g_hash_table_iter_init (&it, v->selected);
  while (g_hash_table_iter_next (&it, &key, nullptr))
    if (index_of (v, (GbkHandle) (uintptr_t) key) == SIZE_MAX)
      {
        g_hash_table_iter_remove (&it);
        changed = true;
      }

  size_t n = n_items (v);
  if (v->cursor != GBK_HANDLE_NONE && index_of (v, v->cursor) == SIZE_MAX)
    v->cursor = n > 0 ? handle_at_index (v, MIN (v->cursor_hint, n - 1)) : GBK_HANDLE_NONE;
  else if (v->cursor != GBK_HANDLE_NONE)
    v->cursor_hint = index_of (v, v->cursor);
  if (v->anchor != GBK_HANDLE_NONE && index_of (v, v->anchor) == SIZE_MAX)
    v->anchor = v->cursor;
  return changed;
}

/* ---- cursor movement ---- */

static size_t
columns_per_page (GbkView *v)
{
  size_t ncols = n_columns (v);
  if (ncols == 0)
    return 1;
  int avg = MAX (1, content_width (v) / (int) ncols);
  return (size_t) MAX (1, gtk_widget_get_allocated_width (GTK_WIDGET (v)) / avg);
}

static size_t
step_index (size_t cur, int64_t delta, size_t n)
{
  int64_t t = (int64_t) cur + delta;
  return (size_t) CLAMP (t, 0, (int64_t) n - 1);
}

void
gbk_view_move_cursor (GbkView *v, GbkMoveStep step, int count, GbkSelectMode mode)
{
  size_t n = n_items (v);
  if (n == 0 || count == 0)
    return;
  size_t cur = index_of (v, v->cursor);
  size_t target;
  if (cur == SIZE_MAX)
    target = (step == GBK_MOVE_ENDS && count > 0) ? n - 1 : 0; /* no cursor yet */
  else
    switch (step)
      {
      case GBK_MOVE_ITEM: target = step_index (cur, count, n); break;
      case GBK_MOVE_COLUMN:
        target = step_index (cur, (int64_t) count * (int64_t) v->rows, n);
        break;
      case GBK_MOVE_PAGE:
        target = step_index (cur, (int64_t) count * (int64_t) (v->rows * columns_per_page (v)), n);
        break;
      case GBK_MOVE_ENDS: target = count < 0 ? 0 : n - 1; break;
      default: gbk_fatal ("bad move step %d", (int) step);
      }

  GbkHandle h = handle_at_index (v, target);
  switch (mode)
    {
    case GBK_SELECT_REPLACE:
      g_hash_table_remove_all (v->selected);
      g_hash_table_add (v->selected, key_of (h));
      v->anchor = h;
      break;
    case GBK_SELECT_EXTEND:
      {
        size_t a = index_of (v, v->anchor);
        if (a == SIZE_MAX)
          {
            a = cur != SIZE_MAX ? cur : target;
            v->anchor = handle_at_index (v, a);
          }
        select_range (v, a, target);
        break;
      }
    case GBK_SELECT_NONE: break;
    default: gbk_fatal ("bad select mode %d", (int) mode);
    }
  set_cursor (v, h);
  scroll_to_index (v, target);
  emit_selection_changed (v);
}

void
gbk_view_toggle_cursor (GbkView *v)
{
  if (index_of (v, v->cursor) == SIZE_MAX)
    return;
  if (!g_hash_table_remove (v->selected, key_of (v->cursor)))
    g_hash_table_add (v->selected, key_of (v->cursor));
  v->anchor = v->cursor;
  emit_selection_changed (v);
}

/* ---- type-ahead ---- */

static bool
search_expired (void *data)
{
  GbkView *v = data;
  v->search_timeout = 0;
  g_string_truncate (v->search, 0);
  g_signal_emit (v, signals[SIGNAL_SEARCH_CHANGED], 0);
  return false;
}

static void
search_clear (GbkView *v)
{
  gbk_source_cancel (&v->search_timeout);
  if (v->search->len == 0)
    return;
  g_string_truncate (v->search, 0);
  g_signal_emit (v, signals[SIGNAL_SEARCH_CHANGED], 0);
}

static bool
search_jump (GbkView *v)
{
  size_t key_len;
  g_autofree char *key = gbk_sort_key_new (v->search->str, v->search->len, &key_len);
  size_t n = n_items (v);
  for (size_t i = 0; i < n; i++)
    {
      GbkHandle h = handle_at_index (v, i);
      const GbkEntry *e = gbk_dir_model_get (v->model, h);
      if (e->key_len >= key_len && memcmp (gbk_entry_key (e), key, key_len) == 0)
        {
          gbk_view_select_only (v, h);
          return true;
        }
    }
  return false;
}

bool
gbk_view_type_ahead (GbkView *v, const char *utf8)
{
  if (!g_utf8_validate (utf8, -1, nullptr) || utf8[0] == '\0')
    return false;
  g_string_append (v->search, utf8);
  gbk_source_cancel (&v->search_timeout);
  v->search_timeout = gbk_timeout_add (G_OBJECT (v), SEARCH_TIMEOUT_MS, search_expired, v);
  g_signal_emit (v, signals[SIGNAL_SEARCH_CHANGED], 0);
  return search_jump (v);
}

const char *
gbk_view_type_ahead_text (GbkView *v)
{
  return v->search->str;
}

/* ---- model ---- */

static void
on_model_changed (GbkDirModel *m, GbkView *v)
{
  if (m != v->model)
    return;
  bool sel_changed = prune_selection (v);
  relayout (v);
  gtk_widget_queue_draw (GTK_WIDGET (v));
  if (sel_changed)
    g_signal_emit (v, signals[SIGNAL_SELECTION_CHANGED], 0);
}

void
gbk_view_set_model (GbkView *v, GbkDirModel *model)
{
  if (v->model != nullptr)
    {
      g_signal_handler_disconnect (v->model, v->model_changed_id);
      v->model_changed_id = 0;
      g_clear_object (&v->model);
    }
  g_hash_table_remove_all (v->selected);
  v->cursor = v->anchor = GBK_HANDLE_NONE;
  v->cursor_hint = 0;
  search_clear (v);
  reset_anchor (v);
  /* Cached widths are keyed by slot and generation, which restart in every model. */
  g_array_set_size (v->widths, 0);

  if (model != nullptr)
    {
      v->model = g_object_ref (model);
      v->model_changed_id
          = gbk_connect (G_OBJECT (v), model, "changed", G_CALLBACK (on_model_changed), v);
    }
  relayout (v);
  gtk_widget_queue_draw (GTK_WIDGET (v));
  g_signal_emit (v, signals[SIGNAL_SELECTION_CHANGED], 0);
}

GbkDirModel *
gbk_view_get_model (const GbkView *v)
{
  return v->model;
}

/* ---- introspection ---- */

size_t
gbk_view_rows_per_column (const GbkView *v)
{
  return v->rows;
}

GbkHandle
gbk_view_item_at (GbkView *v, double x, double y)
{
  if (y < 0 || x < 0 || v->rows == 0 || n_columns (v) == 0)
    return GBK_HANDLE_NONE;
  int cx = (int) x + scroll_x (v);
  if (cx >= content_width (v))
    return GBK_HANDLE_NONE;
  size_t c = column_at (v, cx);
  size_t r = (size_t) (y / v->row_h);
  if (r >= v->rows)
    return GBK_HANDLE_NONE;
  size_t i = c * v->rows + r;
  return i < n_items (v) ? handle_at_index (v, i) : GBK_HANDLE_NONE;
}

/* ---- drawing ---- */

/* Where a drop would land, outlined in the theme's selection colour. */
static void
drop_outline (GbkView *v, cairo_t *cr, int x, int y, int w, int h)
{
  GtkStyleContext *sc = gtk_widget_get_style_context (GTK_WIDGET (v));
  GdkRGBA c;
  if (!gtk_style_context_lookup_color (sc, "theme_selected_bg_color", &c))
    gtk_style_context_get_color (sc, gtk_style_context_get_state (sc), &c);
  cairo_save (cr);
  gdk_cairo_set_source_rgba (cr, &c);
  cairo_set_line_width (cr, 2);
  cairo_rectangle (cr, x + 1, y + 1, MAX (0, w - 2), MAX (0, h - 2));
  cairo_stroke (cr);
  cairo_restore (cr);
}

static gboolean
gbk_view_draw (GtkWidget *w, cairo_t *cr)
{
  GbkView *v = GBK_VIEW (w);
  GtkStyleContext *sc = gtk_widget_get_style_context (w);
  int width = gtk_widget_get_allocated_width (w);
  int height = gtk_widget_get_allocated_height (w);
  gtk_render_background (sc, cr, 0, 0, width, height);

  size_t n = n_items (v);
  if (n == 0 || v->layout == nullptr)
    return false;

  GdkRectangle clip;
  if (!gdk_cairo_get_clip_rectangle (cr, &clip))
    return false;

  int sx = scroll_x (v);
  bool focused = gtk_widget_has_visible_focus (w);
  GtkStateFlags base = gtk_widget_get_state_flags (w);
  pango_layout_set_ellipsize (v->layout, PANGO_ELLIPSIZE_MIDDLE);
  int text_dy = (v->row_h - v->text_h) / 2;
  int icon_dy = (v->row_h - ICON_SIZE) / 2;

  size_t ncols = n_columns (v);
  for (size_t c = column_at (v, clip.x + sx); c < ncols; c++)
    {
      int x = col_start (v, c) - sx;
      int cw = col_start (v, c + 1) - col_start (v, c);
      if (x >= clip.x + clip.width)
        break;
      size_t r0 = (size_t) MAX (0, clip.y / v->row_h);
      for (size_t r = r0; r < v->rows; r++)
        {
          size_t i = c * v->rows + r;
          int y = (int) r * v->row_h;
          if (i >= n || y >= clip.y + clip.height)
            break;
          GbkHandle h = handle_at_index (v, i);
          const GbkEntry *e = gbk_dir_model_get (v->model, h);

          GtkStateFlags state = base;
          if (gbk_view_is_selected (v, h))
            state |= GTK_STATE_FLAG_SELECTED;
          gtk_style_context_save (sc);
          gtk_style_context_set_state (sc, state);
          if (state & GTK_STATE_FLAG_SELECTED)
            gtk_render_background (sc, cr, x, y, cw, v->row_h);

          cairo_surface_t *icon = icon_for (v, e->type);
          if (icon != nullptr)
            {
              cairo_set_source_surface (cr, icon, x + PAD_X, y + icon_dy);
              cairo_paint (cr);
            }
          cairo_surface_t *mark
              = v->marked != nullptr && g_hash_table_contains (v->marked, gbk_entry_name (e))
                    ? emblem (v)
                    : nullptr;
          if (mark != nullptr)
            {
              cairo_set_source_surface (cr, mark, x + PAD_X + ICON_SIZE - EMBLEM_SIZE + 2,
                                        y + icon_dy + ICON_SIZE - EMBLEM_SIZE + 2);
              cairo_paint (cr);
            }

          int text_w = cw - 2 * PAD_X - ICON_SIZE - ICON_GAP;
          pango_layout_set_text (v->layout, gbk_entry_display (e), -1);
          pango_layout_set_width (v->layout, MAX (1, text_w) * PANGO_SCALE);
          gtk_render_layout (sc, cr, x + PAD_X + ICON_SIZE + ICON_GAP, y + text_dy, v->layout);

          if (focused && h == v->cursor)
            gtk_render_focus (sc, cr, x, y, cw, v->row_h);
          gtk_style_context_restore (sc);
          if (v->drop_hover && h == v->drop_folder)
            drop_outline (v, cr, x, y, cw, v->row_h);
        }
    }
  if (v->drop_hover && v->drop_folder == GBK_HANDLE_NONE)
    drop_outline (v, cr, 0, 0, width, height);
  pango_layout_set_width (v->layout, -1);
  pango_layout_set_ellipsize (v->layout, PANGO_ELLIPSIZE_NONE);
  return false;
}

/* ---- input ---- */

static gboolean
gbk_view_button_press (GtkWidget *w, GdkEventButton *ev)
{
  GbkView *v = GBK_VIEW (w);
  if (!gtk_widget_has_focus (w))
    gtk_widget_grab_focus (w);
  search_clear (v);

  GbkHandle h = gbk_view_item_at (v, ev->x, ev->y);
  GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask ();
  bool ctrl = mods & GDK_CONTROL_MASK, shift = mods & GDK_SHIFT_MASK;

  if (ev->type == GDK_2BUTTON_PRESS && ev->button == GDK_BUTTON_PRIMARY)
    {
      if (h != GBK_HANDLE_NONE && !ctrl && !shift)
        g_signal_emit (v, signals[SIGNAL_ACTIVATE], 0);
      return true;
    }
  if (ev->type != GDK_BUTTON_PRESS)
    return false;

  if (ev->button == GDK_BUTTON_PRIMARY)
    {
      if (h == GBK_HANDLE_NONE)
        {
          if (!ctrl && !shift)
            gbk_view_unselect_all (v);
          return true;
        }
      v->press_armed = true;
      v->press_pending = false;
      v->press_x = ev->x;
      v->press_y = ev->y;
      v->press_item = h;
      if (!shift && !ctrl && gbk_view_is_selected (v, h))
        {
          /* Maybe the start of dragging the whole selection: decide on release. */
          set_cursor (v, h);
          v->press_pending = true;
          gtk_widget_queue_draw (w);
        }
      else if (shift)
        {
          size_t a = index_of (v, v->anchor);
          size_t t = index_of (v, h);
          select_range (v, a != SIZE_MAX ? a : t, t);
          if (a == SIZE_MAX)
            v->anchor = h;
          set_cursor (v, h);
          emit_selection_changed (v);
        }
      else if (ctrl)
        {
          set_cursor (v, h);
          gbk_view_toggle_cursor (v);
        }
      else
        gbk_view_select_only (v, h);
      return true;
    }

  if (ev->button == GDK_BUTTON_SECONDARY)
    {
      if (h == GBK_HANDLE_NONE)
        gbk_view_unselect_all (v);
      else if (!gbk_view_is_selected (v, h))
        gbk_view_select_only (v, h);
      g_signal_emit (v, signals[SIGNAL_CONTEXT_MENU], 0, (GdkEvent *) ev);
      return true;
    }
  return false;
}

static gboolean
gbk_view_button_release (GtkWidget *w, GdkEventButton *ev)
{
  GbkView *v = GBK_VIEW (w);
  if (ev->button != GDK_BUTTON_PRIMARY)
    return false;
  /* A plain click on a selected item (no drag happened): select just it. */
  if (v->press_pending && v->model != nullptr
      && gbk_dir_model_get (v->model, v->press_item) != nullptr)
    gbk_view_select_only (v, v->press_item);
  v->press_pending = false;
  v->press_armed = false;
  return false;
}

static gboolean
gbk_view_motion (GtkWidget *w, GdkEventMotion *ev)
{
  GbkView *v = GBK_VIEW (w);
  if (!v->press_armed || !(ev->state & GDK_BUTTON1_MASK))
    return false;
  if (!gtk_drag_check_threshold (w, (int) v->press_x, (int) v->press_y, (int) ev->x, (int) ev->y))
    return true;
  v->press_armed = false;
  v->press_pending = false;
  /* Files on another machine can't be handed to other apps (yet): no drag at all. */
  const GbkLoc *here = v->model != nullptr ? gbk_dir_model_loc (v->model) : nullptr;
  if (here != nullptr && !gbk_loc_is_local (here))
    return true;
  g_autoptr (GtkTargetList) targets = gbk_dnd_source_targets ();
  gtk_drag_begin_with_coordinates (w, targets, GBK_DND_ACTIONS, GDK_BUTTON_PRIMARY, (GdkEvent *) ev,
                                   (int) v->press_x, (int) v->press_y);
  return true;
}

/* ---- drag and drop ---- */

/* The selection as locations (GbkLoc*), in view order. */
static GPtrArray *
selected_locs (GbkView *v)
{
  GPtrArray *locs = gbk_loc_array_new ();
  const GbkLoc *dir = v->model != nullptr ? gbk_dir_model_loc (v->model) : nullptr;
  if (dir == nullptr)
    return locs;
  size_t n;
  g_autofree GbkHandle *sel = gbk_view_selected (v, &n);
  for (size_t i = 0; i < n; i++)
    {
      const GbkEntry *e = gbk_dir_model_get (v->model, sel[i]);
      if (e != nullptr)
        g_ptr_array_add (locs, gbk_loc_child (dir, gbk_entry_name (e)));
    }
  return locs;
}

static void
gbk_view_drag_begin (GtkWidget *w, GdkDragContext *ctx)
{
  GbkView *v = GBK_VIEW (w);
  /* Snapshot now: the folder can change while the drag is in flight. */
  g_clear_pointer (&v->drag_locs, g_ptr_array_unref);
  v->drag_locs = selected_locs (v);
  const GbkEntry *e = v->model != nullptr ? gbk_dir_model_get (v->model, v->press_item) : nullptr;
  cairo_surface_t *icon = e != nullptr ? icon_for (v, e->type) : nullptr;
  if (icon != nullptr)
    gtk_drag_set_icon_surface (ctx, icon);
  else
    gtk_drag_set_icon_default (ctx);
}

static void
gbk_view_drag_data_get (GtkWidget *w, GdkDragContext *ctx G_GNUC_UNUSED, GtkSelectionData *sd,
                        guint info, guint time G_GNUC_UNUSED)
{
  const GbkView *v = GBK_VIEW (w);
  /* Files on another machine can't be handed to other apps: nothing is set, no drop. */
  if (v->drag_locs != nullptr && v->drag_locs->len > 0
      && !gbk_dnd_set_data (sd, info, v->drag_locs))
    return;
}

static void
gbk_view_drag_end (GtkWidget *w, GdkDragContext *ctx G_GNUC_UNUSED)
{
  g_clear_pointer (&GBK_VIEW (w)->drag_locs, g_ptr_array_unref);
}

/* Where a drop at (x, y) would go: a folder item under the pointer, else the view's own
 * folder. Dragging from this very view: not onto an item being dragged (into itself), and
 * not onto the folder they're already in. */
static bool
drop_target (GbkView *v, double x, double y, bool from_self, GbkHandle *folder)
{
  *folder = GBK_HANDLE_NONE;
  if (v->model == nullptr || gbk_dir_model_state (v->model) == GBK_DIR_ERROR)
    return false;
  /* Drag and drop with another machine comes later (phase 8 of docs/remote-plan.md). */
  const GbkLoc *here = gbk_dir_model_loc (v->model);
  if (here != nullptr && !gbk_loc_is_local (here))
    return false;
  GbkHandle h = gbk_view_item_at (v, x, y);
  const GbkEntry *e = h != GBK_HANDLE_NONE ? gbk_dir_model_get (v->model, h) : nullptr;
  if (e != nullptr && gbk_entry_is_dir (e) && !(from_self && gbk_view_is_selected (v, h)))
    {
      *folder = h;
      return true;
    }
  return !from_self;
}

static void
set_drop_hover (GbkView *v, bool hover, GbkHandle folder)
{
  if (v->drop_hover == hover && v->drop_folder == folder)
    return;
  v->drop_hover = hover;
  v->drop_folder = folder;
  gtk_widget_queue_draw (GTK_WIDGET (v));
}

static bool
autoscroll_tick (void *data)
{
  GbkView *v = data;
  if (v->scroll_dir == 0 || v->hadj == nullptr)
    {
      v->autoscroll = 0;
      return false;
    }
  gtk_adjustment_set_value (v->hadj,
                            gtk_adjustment_get_value (v->hadj) + v->scroll_dir * DND_SCROLL_PX);
  return true;
}

static void
set_autoscroll (GbkView *v, int dir)
{
  v->scroll_dir = dir;
  if (dir == 0)
    gbk_source_cancel (&v->autoscroll);
  else if (v->autoscroll == 0)
    v->autoscroll = gbk_timeout_add (G_OBJECT (v), DND_SCROLL_MS, autoscroll_tick, v);
}

static gboolean
gbk_view_drag_motion (GtkWidget *w, GdkDragContext *ctx, int x, int y, guint time)
{
  GbkView *v = GBK_VIEW (w);
  int width = gtk_widget_get_allocated_width (w);
  set_autoscroll (v, x < DND_EDGE_PX ? -1 : x > width - DND_EDGE_PX ? 1 : 0);
  GbkHandle folder;
  bool ok = gtk_drag_dest_find_target (w, ctx, nullptr) != GDK_NONE
            && drop_target (v, x, y, gtk_drag_get_source_widget (ctx) == w, &folder);
  GdkDragAction action = ok ? gbk_dnd_action (ctx) : 0;
  set_drop_hover (v, action != 0, ok ? folder : GBK_HANDLE_NONE);
  gdk_drag_status (ctx, action, time);
  return true;
}

static void
gbk_view_drag_leave (GtkWidget *w, GdkDragContext *ctx G_GNUC_UNUSED, guint time G_GNUC_UNUSED)
{
  GbkView *v = GBK_VIEW (w);
  set_autoscroll (v, 0);
  set_drop_hover (v, false, GBK_HANDLE_NONE);
}

static gboolean
gbk_view_drag_drop (GtkWidget *w, GdkDragContext *ctx, int x, int y, guint time)
{
  GbkView *v = GBK_VIEW (w);
  set_autoscroll (v, 0);
  GdkAtom target = gtk_drag_dest_find_target (w, ctx, nullptr);
  GbkHandle folder;
  if (target == GDK_NONE || !drop_target (v, x, y, gtk_drag_get_source_widget (ctx) == w, &folder))
    {
      gtk_drag_finish (ctx, false, false, time);
      return true;
    }
  v->drop_folder = folder;
  v->drop_requested = true;
  gtk_drag_get_data (w, ctx, target, time);
  return true;
}

static void
deliver_drop (GbkView *v, GPtrArray *locs, GbkHandle folder, GdkEvent *trigger)
{
  if (locs->len > 0)
    g_signal_emit (v, signals[SIGNAL_DROP], 0, locs, (guint64) folder, trigger);
}

static void
gbk_view_drag_data_received (GtkWidget *w, GdkDragContext *ctx, int x G_GNUC_UNUSED,
                             int y G_GNUC_UNUSED, GtkSelectionData *sd, guint info G_GNUC_UNUSED,
                             guint time)
{
  GbkView *v = GBK_VIEW (w);
  GbkHandle folder = v->drop_folder;
  set_drop_hover (v, false, GBK_HANDLE_NONE);
  if (!v->drop_requested)
    return;
  v->drop_requested = false;
  g_autoptr (GPtrArray) locs = gbk_dnd_locs (sd);
  /* del = false always: whatever the user picks, we do it; the source deletes nothing. */
  gtk_drag_finish (ctx, locs->len > 0, false, time);
  g_autoptr (GdkEvent) trigger = gtk_get_current_event ();
  deliver_drop (v, locs, folder, trigger);
}

static gboolean
gbk_view_key_press (GtkWidget *w, GdkEventKey *ev)
{
  GbkView *v = GBK_VIEW (w);
  GdkModifierType mods = ev->state & gtk_accelerator_get_default_mod_mask ();
  if (mods & (GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_HYPER_MASK | GDK_META_MASK))
    return false; /* Alt+...: window-level navigation */
  bool ctrl = mods & GDK_CONTROL_MASK, shift = mods & GDK_SHIFT_MASK;
  GbkSelectMode mode = shift ? GBK_SELECT_EXTEND : ctrl ? GBK_SELECT_NONE : GBK_SELECT_REPLACE;

  switch (ev->keyval)
    {
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      search_clear (v);
      gbk_view_move_cursor (v, GBK_MOVE_ITEM, -1, mode);
      return true;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      search_clear (v);
      gbk_view_move_cursor (v, GBK_MOVE_ITEM, 1, mode);
      return true;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      search_clear (v);
      gbk_view_move_cursor (v, GBK_MOVE_COLUMN, -1, mode);
      return true;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      search_clear (v);
      gbk_view_move_cursor (v, GBK_MOVE_COLUMN, 1, mode);
      return true;
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:
      search_clear (v);
      gbk_view_move_cursor (v, GBK_MOVE_PAGE, -1, mode);
      return true;
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:
      search_clear (v);
      gbk_view_move_cursor (v, GBK_MOVE_PAGE, 1, mode);
      return true;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
      search_clear (v);
      gbk_view_move_cursor (v, GBK_MOVE_ENDS, -1, mode);
      return true;
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
      search_clear (v);
      gbk_view_move_cursor (v, GBK_MOVE_ENDS, 1, mode);
      return true;

    case GDK_KEY_space:
      if (v->search->len > 0 && !ctrl)
        gbk_view_type_ahead (v, " "); /* mid-search, space is part of the name */
      else
        gbk_view_toggle_cursor (v);
      return true;

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
      search_clear (v);
      if (gbk_view_n_selected (v) == 0 && index_of (v, v->cursor) != SIZE_MAX)
        gbk_view_select_only (v, v->cursor);
      if (gbk_view_n_selected (v) > 0)
        g_signal_emit (v, signals[SIGNAL_ACTIVATE], 0);
      return true;

    case GDK_KEY_Menu:
      search_clear (v);
      g_signal_emit (v, signals[SIGNAL_CONTEXT_MENU], 0, (GdkEvent *) nullptr);
      return true;
    case GDK_KEY_F10:
      if (!shift || ctrl)
        return false;
      search_clear (v);
      g_signal_emit (v, signals[SIGNAL_CONTEXT_MENU], 0, (GdkEvent *) nullptr);
      return true;

    case GDK_KEY_Escape:
      if (v->search->len == 0)
        return false;
      search_clear (v);
      return true;

    case GDK_KEY_BackSpace:
      if (v->search->len == 0)
        return false;
      {
        const char *end = v->search->str + v->search->len;
        const char *prev = g_utf8_find_prev_char (v->search->str, end);
        g_string_truncate (v->search, prev != nullptr ? (size_t) (prev - v->search->str) : 0);
        g_signal_emit (v, signals[SIGNAL_SEARCH_CHANGED], 0);
        if (v->search->len > 0)
          search_jump (v);
      }
      return true;

    default: break;
    }

  if (ctrl && (ev->keyval == GDK_KEY_a || ev->keyval == GDK_KEY_A))
    {
      gbk_view_select_all (v);
      return true;
    }
  if (ctrl)
    return false;

  gunichar uc = gdk_keyval_to_unicode (ev->keyval);
  if (uc != 0 && g_unichar_isprint (uc))
    {
      char buf[8] = { 0 };
      g_unichar_to_utf8 (uc, buf);
      if (!gbk_view_type_ahead (v, buf))
        gtk_widget_error_bell (w);
      return true;
    }
  return false;
}

static gboolean
gbk_view_scroll (GtkWidget *w, GdkEventScroll *ev)
{
  GbkView *v = GBK_VIEW (w);
  if (v->hadj == nullptr)
    return false;
  double delta = 0;
  switch (ev->direction)
    {
    case GDK_SCROLL_UP:
    case GDK_SCROLL_LEFT: delta = -1; break;
    case GDK_SCROLL_DOWN:
    case GDK_SCROLL_RIGHT: delta = 1; break;
    case GDK_SCROLL_SMOOTH:
      delta = fabs (ev->delta_x) > fabs (ev->delta_y) ? ev->delta_x : ev->delta_y;
      break;
    default: return false;
    }
  /* Vertical wheel scrolls horizontally: content only extends sideways. */
  gtk_adjustment_set_value (v->hadj, gtk_adjustment_get_value (v->hadj) + delta * WHEEL_STEP_PX);
  return true;
}

static gboolean
gbk_view_focus_change (GtkWidget *w, GdkEventFocus *ev G_GNUC_UNUSED)
{
  gtk_widget_queue_draw (w);
  return false;
}

/* ---- GtkWidget plumbing ---- */

static void
gbk_view_realize (GtkWidget *w)
{
  GtkAllocation a;
  gtk_widget_get_allocation (w, &a);
  gtk_widget_set_realized (w, true);

  GdkWindowAttr attrs = {
    .x = a.x,
    .y = a.y,
    .width = a.width,
    .height = a.height,
    .window_type = GDK_WINDOW_CHILD,
    .wclass = GDK_INPUT_OUTPUT,
    .visual = gtk_widget_get_visual (w),
    .event_mask = gtk_widget_get_events (w) | GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK
                  | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON1_MOTION_MASK | GDK_KEY_PRESS_MASK
                  | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_FOCUS_CHANGE_MASK,
  };
  GdkWindow *win = gdk_window_new (gtk_widget_get_parent_window (w), &attrs,
                                   GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
  gtk_widget_set_window (w, win);
  gtk_widget_register_window (w, win);
}

static void
gbk_view_size_allocate (GtkWidget *w, GtkAllocation *a)
{
  gtk_widget_set_allocation (w, a);
  if (gtk_widget_get_realized (w))
    gdk_window_move_resize (gtk_widget_get_window (w), a->x, a->y, a->width, a->height);
  relayout (GBK_VIEW (w));
}

static void
gbk_view_get_preferred_width (GtkWidget *w G_GNUC_UNUSED, int *min, int *nat)
{
  *min = 100;
  *nat = 400;
}

static void
gbk_view_get_preferred_height (GtkWidget *w G_GNUC_UNUSED, int *min, int *nat)
{
  *min = 60;
  *nat = 300;
}

static void
gbk_view_style_updated (GtkWidget *w)
{
  GTK_WIDGET_CLASS (gbk_view_parent_class)->style_updated (w);
  GbkView *v = GBK_VIEW (w);
  update_metrics (v);
  clear_icons (v);
  relayout (v);
  gtk_widget_queue_draw (w);
}

static void
on_hadj_changed (GtkAdjustment *adj G_GNUC_UNUSED, GbkView *v)
{
  if (!v->adjusting)
    record_anchor (v);
  gtk_widget_queue_draw (GTK_WIDGET (v));
}

static void
set_hadjustment (GbkView *v, GtkAdjustment *adj)
{
  if (v->hadj == adj)
    return;
  if (v->hadj != nullptr)
    {
      g_signal_handler_disconnect (v->hadj, v->hadj_changed_id);
      v->hadj_changed_id = 0;
      g_clear_object (&v->hadj);
    }
  if (adj == nullptr)
    adj = gtk_adjustment_new (0, 0, 0, 0, 0, 0);
  v->hadj = g_object_ref_sink (adj);
  v->hadj_changed_id
      = gbk_connect (G_OBJECT (v), adj, "value-changed", G_CALLBACK (on_hadj_changed), v);
  update_adjustment (v);
}

static void
set_vadjustment (GbkView *v, GtkAdjustment *adj)
{
  /* No vertical scrolling: hold the adjustment so GtkScrollable is satisfied, keep it 0. */
  g_clear_object (&v->vadj);
  if (adj != nullptr)
    {
      v->vadj = g_object_ref_sink (adj);
      gtk_adjustment_configure (adj, 0, 0, 0, 0, 0, 0);
    }
}

static void
gbk_view_set_property (GObject *o, guint id, const GValue *value, GParamSpec *pspec)
{
  GbkView *v = GBK_VIEW (o);
  switch (id)
    {
    case PROP_HADJUSTMENT: set_hadjustment (v, g_value_get_object (value)); break;
    case PROP_VADJUSTMENT: set_vadjustment (v, g_value_get_object (value)); break;
    case PROP_HSCROLL_POLICY: v->hscroll_policy = (guint) g_value_get_enum (value); break;
    case PROP_VSCROLL_POLICY: v->vscroll_policy = (guint) g_value_get_enum (value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, pspec);
    }
}

static void
gbk_view_get_property (GObject *o, guint id, GValue *value, GParamSpec *pspec)
{
  GbkView *v = GBK_VIEW (o);
  switch (id)
    {
    case PROP_HADJUSTMENT: g_value_set_object (value, v->hadj); break;
    case PROP_VADJUSTMENT: g_value_set_object (value, v->vadj); break;
    case PROP_HSCROLL_POLICY: g_value_set_enum (value, (int) v->hscroll_policy); break;
    case PROP_VSCROLL_POLICY: g_value_set_enum (value, (int) v->vscroll_policy); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, pspec);
    }
}

static void
gbk_view_dispose (GObject *o)
{
  GbkView *v = GBK_VIEW (o);
  gbk_source_cancel (&v->search_timeout);
  gbk_source_cancel (&v->autoscroll);
  if (v->model != nullptr)
    {
      g_signal_handler_disconnect (v->model, v->model_changed_id);
      g_clear_object (&v->model);
    }
  if (v->hadj != nullptr)
    {
      g_signal_handler_disconnect (v->hadj, v->hadj_changed_id);
      g_clear_object (&v->hadj);
    }
  g_clear_object (&v->vadj);
  g_clear_object (&v->layout);
  G_OBJECT_CLASS (gbk_view_parent_class)->dispose (o);
}

static void
gbk_view_finalize (GObject *o)
{
  GbkView *v = GBK_VIEW (o);
  clear_icons (v);
  g_ptr_array_unref (v->icons);
  g_array_unref (v->icons_tried);
  g_clear_pointer (&v->marked, g_hash_table_unref);
  g_array_unref (v->col_x);
  g_array_unref (v->widths);
  g_hash_table_unref (v->selected);
  g_string_free (v->search, true);
  g_clear_pointer (&v->drag_locs, g_ptr_array_unref);
  gbk_type_table_unref (v->types);
  G_OBJECT_CLASS (gbk_view_parent_class)->finalize (o);
}

static void
gbk_view_class_init (GbkViewClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  GtkWidgetClass *wc = GTK_WIDGET_CLASS (klass);
  oc->set_property = gbk_view_set_property;
  oc->get_property = gbk_view_get_property;
  oc->dispose = gbk_view_dispose;
  oc->finalize = gbk_view_finalize;
  wc->realize = gbk_view_realize;
  wc->size_allocate = gbk_view_size_allocate;
  wc->get_preferred_width = gbk_view_get_preferred_width;
  wc->get_preferred_height = gbk_view_get_preferred_height;
  wc->draw = gbk_view_draw;
  wc->style_updated = gbk_view_style_updated;
  wc->button_press_event = gbk_view_button_press;
  wc->button_release_event = gbk_view_button_release;
  wc->motion_notify_event = gbk_view_motion;
  wc->drag_begin = gbk_view_drag_begin;
  wc->drag_data_get = gbk_view_drag_data_get;
  wc->drag_end = gbk_view_drag_end;
  /* No drag_data_delete: we never delete files because a drop target asked. */
  wc->drag_motion = gbk_view_drag_motion;
  wc->drag_leave = gbk_view_drag_leave;
  wc->drag_drop = gbk_view_drag_drop;
  wc->drag_data_received = gbk_view_drag_data_received;
  wc->key_press_event = gbk_view_key_press;
  wc->scroll_event = gbk_view_scroll;
  wc->focus_in_event = gbk_view_focus_change;
  wc->focus_out_event = gbk_view_focus_change;

  g_object_class_override_property (oc, PROP_HADJUSTMENT, "hadjustment");
  g_object_class_override_property (oc, PROP_VADJUSTMENT, "vadjustment");
  g_object_class_override_property (oc, PROP_HSCROLL_POLICY, "hscroll-policy");
  g_object_class_override_property (oc, PROP_VSCROLL_POLICY, "vscroll-policy");

  signals[SIGNAL_ACTIVATE] = g_signal_new ("activate", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                           0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  signals[SIGNAL_SELECTION_CHANGED]
      = g_signal_new ("selection-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                      nullptr, nullptr, G_TYPE_NONE, 0);
  signals[SIGNAL_CONTEXT_MENU] = g_signal_new (
      "context-menu", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
      G_TYPE_NONE, 1, GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);
  signals[SIGNAL_SEARCH_CHANGED]
      = g_signal_new ("search-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                      nullptr, nullptr, G_TYPE_NONE, 0);

  signals[SIGNAL_DROP] = g_signal_new ("drop", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                                       nullptr, nullptr, nullptr, G_TYPE_NONE, 3, G_TYPE_POINTER,
                                       G_TYPE_UINT64, GDK_TYPE_EVENT | G_SIGNAL_TYPE_STATIC_SCOPE);

  /* Themes style icon views and their selection under these names. */
  gtk_widget_class_set_css_name (wc, "iconview");
}

static void
gbk_view_init (GbkView *v)
{
  gtk_widget_set_has_window (GTK_WIDGET (v), true);
  gtk_widget_set_can_focus (GTK_WIDGET (v), true);
  gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (v)), GTK_STYLE_CLASS_VIEW);
  v->col_x = g_array_new (false, true, sizeof (int));
  v->widths = g_array_new (false, true, sizeof (WidthCache));
  v->selected = g_hash_table_new (g_direct_hash, g_direct_equal);
  v->search = g_string_new ("");
  v->icons = g_ptr_array_new ();
  v->icons_tried = g_array_new (false, true, sizeof (bool));
  v->row_h = ICON_SIZE + 2 * PAD_Y;
  v->rows = 1;
  /* Flags 0: motion, highlighting and accepting are all ours (see dnd.h). */
  gtk_drag_dest_set (GTK_WIDGET (v), 0, nullptr, 0, GBK_DND_ACTIONS);
  g_autoptr (GtkTargetList) targets = gbk_dnd_dest_targets ();
  gtk_drag_dest_set_target_list (GTK_WIDGET (v), targets);
}

GtkWidget *
gbk_view_new (GbkTypeTable *types)
{
  GbkView *v = g_object_new (GBK_TYPE_VIEW, nullptr);
  v->types = gbk_type_table_ref (types);
  set_hadjustment (v, nullptr);
  gbk_connect (G_OBJECT (v), gtk_icon_theme_get_default (), "changed",
               G_CALLBACK (on_icon_theme_changed), v);
  return GTK_WIDGET (v);
}

char *
gbk_view_drag_uris (GbkView *v)
{
  g_autoptr (GPtrArray) locs = selected_locs (v);
  g_autoptr (GPtrArray) paths = gbk_loc_array_as_local (locs);
  return paths != nullptr ? gbk_clip_format_uri_list (paths) : g_strdup ("");
}

bool
gbk_view_drop_target_at (GbkView *v, double x, double y, bool from_self, GbkHandle *folder)
{
  return drop_target (v, x, y, from_self, folder);
}

bool
gbk_view_drop_for_test (GbkView *v, const char *uri_list, double x, double y, GdkEvent *trigger)
{
  GbkHandle folder;
  if (!drop_target (v, x, y, false, &folder))
    return false;
  bool cut;
  g_autoptr (GPtrArray) paths = gbk_clip_parse (uri_list, strlen (uri_list), false, &cut);
  g_autoptr (GPtrArray) locs = gbk_loc_array_from_local (paths);
  deliver_drop (v, locs, folder, trigger);
  return locs->len > 0;
}
