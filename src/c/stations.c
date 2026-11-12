#include "stations.h"

// Drill-down station picker.
//
// resources/stations.txt holds every station, one per line, sorted by folded
// name (scripts/build-stations.mjs). Because of that order all stations that
// start with a typed prefix form one contiguous range, so no search structure
// is needed: a level is just (first, last, prefix length). A level with many
// stations lists the groups of the next letter ("Br...  12"); a level with few
// lists the stations. Choosing a group zooms in, Back zooms out. Prefixes that
// all stations of a group share are skipped, so "Br" opens straight into
// "Bru..." and the user never taps through a letter that offers no choice.

#define LEAF_LIMIT 12     // list stations directly when a level has at most this many
#define MAX_ROWS 48
#define MAX_DEPTH 12
#define MAX_RECENT 5
#define RECENT_KEY 120

typedef enum { RowStation, RowRecent, RowGroup } RowKind;

typedef struct {
  uint8_t kind;
  uint8_t plen;          // group: prefix length (folded characters) after zooming in
  uint16_t first, last;  // station: first = index; recent: first = slot; group: [first, last)
} Row;

typedef struct {
  uint16_t first, last;
  uint8_t plen;
} Level;

typedef struct {
  char names[MAX_RECENT][STATION_NAME_MAX];
} Recents;

typedef struct {
  Window *window;
  MenuLayer *menu;
  char *data;
  uint16_t *offsets;
  uint16_t count;
  Level levels[MAX_DEPTH];
  int depth;
  Row rows[MAX_ROWS];
  int row_count, recent_rows;
  Recents recents;
  char title[12];
  StationPickedHandler handler;
  void *context;
} Picker;

static Picker *s_picker;

// ---- folding: must match fold() in scripts/build-stations.mjs ----------------

static char fold_next(const char **text) {
  static const char base[] = "aaaaaaaceeeeiiiidnoooooxouuuuyts";
  unsigned char c = (unsigned char)**text;
  if (!c) return 0;
  if (c == 0xC3 && (*text)[1]) {  // U+00C0..U+00FF
    unsigned char d = (unsigned char)(*text)[1];
    *text += 2;
    return base[(d + 0x40 - 0xC0) & 31];
  }
  (*text)++;
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
}

static void fold_into(const char *name, char *out, size_t size) {
  size_t i = 0;
  char c;
  while (i + 1 < size && (c = fold_next(&name)) != 0) out[i++] = c;
  out[i] = 0;
}

static const char *name_at(uint16_t index) { return s_picker->data + s_picker->offsets[index]; }

// The first `folded` folded characters of `name`, as display text.
static void display_prefix(const char *name, int folded, char *out, size_t size) {
  const char *p = name;
  while (folded > 0 && *p) { fold_next(&p); folded--; }
  size_t length = p - name;
  if (length >= size) length = size - 1;
  memcpy(out, name, length);
  out[length] = 0;
}

// ---- data ------------------------------------------------------------------

static bool load_stations(Picker *picker) {
  ResHandle handle = resource_get_handle(RESOURCE_ID_STATIONS);
  size_t size = resource_size(handle);
  picker->data = malloc(size + 1);
  if (!picker->data) return false;
  size = resource_load(handle, (uint8_t *)picker->data, size);
  picker->data[size] = 0;
  uint16_t lines = 0;
  for (size_t i = 0; i < size; i++) if (picker->data[i] == '\n') lines++;
  picker->offsets = malloc(sizeof(uint16_t) * (lines ? lines : 1));
  if (!picker->offsets) return false;
  size_t start = 0;
  for (size_t i = 0; i < size && picker->count < lines; i++) {
    if (picker->data[i] != '\n') continue;
    picker->data[i] = 0;
    picker->offsets[picker->count++] = start;
    start = i + 1;
  }
  return picker->count > 0;
}

static void remember(const char *name) {
  Recents *recents = &s_picker->recents;
  int slot = MAX_RECENT - 1;
  for (int i = 0; i < MAX_RECENT; i++) if (strcmp(recents->names[i], name) == 0) { slot = i; break; }
  for (int i = slot; i > 0; i--) memcpy(recents->names[i], recents->names[i - 1], STATION_NAME_MAX);
  snprintf(recents->names[0], STATION_NAME_MAX, "%s", name);
  persist_write_data(RECENT_KEY, recents, sizeof(*recents));
}

// ---- levels ------------------------------------------------------------------

// Length of the prefix shared by every station in [first, last), at least `plen`.
// The list is sorted, so comparing the first and the last station is enough.
static uint8_t shared_prefix(uint16_t first, uint16_t last, uint8_t plen) {
  char a[48], b[48];
  fold_into(name_at(first), a, sizeof(a));
  fold_into(name_at(last - 1), b, sizeof(b));
  uint8_t length = plen;
  while (a[length] && a[length] == b[length]) length++;
  return length;
}

static void add_row(RowKind kind, uint8_t plen, uint16_t first, uint16_t last) {
  if (s_picker->row_count >= MAX_ROWS) return;
  s_picker->rows[s_picker->row_count++] = (Row){ .kind = kind, .plen = plen, .first = first, .last = last };
}

static void build_rows(void) {
  Picker *picker = s_picker;
  Level *level = &picker->levels[picker->depth];
  picker->row_count = picker->recent_rows = 0;
  if (picker->depth == 0) {
    for (int i = 0; i < MAX_RECENT; i++) if (picker->recents.names[i][0]) add_row(RowRecent, 0, i, i + 1);
    picker->recent_rows = picker->row_count;
  }
  if (level->last - level->first <= LEAF_LIMIT || picker->depth >= MAX_DEPTH - 1) {
    for (uint16_t i = level->first; i < level->last; i++) add_row(RowStation, 0, i, i + 1);
    return;
  }
  char a[48], b[48];
  uint16_t i = level->first;
  while (i < level->last && picker->row_count < MAX_ROWS) {
    fold_into(name_at(i), a, sizeof(a));
    char next = a[level->plen];
    uint16_t j = i + 1;
    while (next && j < level->last) {
      fold_into(name_at(j), b, sizeof(b));
      if (b[level->plen] != next) break;
      j++;
    }
    if (j - i == 1) add_row(RowStation, 0, i, j);
    else add_row(RowGroup, shared_prefix(i, j, level->plen + 1), i, j);
    i = j;
  }
}

static void reload(bool to_top) {
  build_rows();
  menu_layer_reload_data(s_picker->menu);
  if (to_top) {
    menu_layer_set_selected_index(s_picker->menu, MenuIndex(0, 0), MenuRowAlignNone, false);
    // Scroll fully up so the section header (title and typed prefix) is visible.
    scroll_layer_set_content_offset(menu_layer_get_scroll_layer(s_picker->menu), GPointZero, false);
  }
}

static void zoom_in(const Row *row) {
  Picker *picker = s_picker;
  if (picker->depth >= MAX_DEPTH - 1) return;
  picker->levels[++picker->depth] = (Level){ .first = row->first, .last = row->last, .plen = row->plen };
  reload(true);
}

// ---- menu ----------------------------------------------------------------------

static const Row *row_at(const MenuIndex *index) {
  int flat = index->section == 0 && s_picker->recent_rows ? index->row : index->row + s_picker->recent_rows;
  return flat < s_picker->row_count ? &s_picker->rows[flat] : NULL;
}

static uint16_t num_sections(MenuLayer *layer, void *context) { return s_picker->recent_rows ? 2 : 1; }

static uint16_t num_rows(MenuLayer *layer, uint16_t section, void *context) {
  if (s_picker->recent_rows) return section == 0 ? s_picker->recent_rows : s_picker->row_count - s_picker->recent_rows;
  return s_picker->row_count;
}

// On the round screen rows and header are inset from the curve; the header gets room above its text.
// The 260 px Pebble Round 2 needs a wider inset and header than the 180 px Time Round.
static bool wide_round(const GRect bounds) { return PBL_IF_ROUND_ELSE(bounds.size.w >= 240, false); }
static int row_inset(const GRect bounds) { return PBL_IF_ROUND_ELSE(wide_round(bounds) ? 34 : 22, 5); }
static int16_t header_height(MenuLayer *layer, uint16_t section, void *context) {
  return PBL_IF_ROUND_ELSE(wide_round(layer_get_bounds(menu_layer_get_layer(layer))) ? 44 : 30, 18);
}
static int16_t cell_height(MenuLayer *layer, MenuIndex *index, void *context) {
  return layer_get_bounds(menu_layer_get_layer(layer)).size.h >= 228 ? 34 : 28;
}

static void draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *context) {
  Picker *picker = s_picker;
  GRect bounds = layer_get_bounds(cell);
  char text[56];
  if (picker->recent_rows && section == 0) snprintf(text, sizeof(text), "%s: recent", picker->title);
  else if (picker->depth == 0) snprintf(text, sizeof(text), "%s: stations A-Z", picker->title);
  else {
    char prefix[24];
    Level *level = &picker->levels[picker->depth];
    display_prefix(name_at(level->first), level->plen, prefix, sizeof(prefix));
    snprintf(text, sizeof(text), "%s: %s...", picker->title, prefix);
  }
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack));
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), GRect(PBL_IF_ROUND_ELSE(wide_round(bounds) ? 36 : 24, 4), PBL_IF_ROUND_ELSE(wide_round(bounds) ? 20 : 12, 0), bounds.size.w - PBL_IF_ROUND_ELSE(wide_round(bounds) ? 72 : 48, 8), 18),
                     GTextOverflowModeTrailingEllipsis, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
}

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *index, void *context) {
  const Row *row = row_at(index);
  if (!row) return;
  GRect bounds = layer_get_bounds(cell);
  bool selected = menu_cell_layer_is_highlighted(cell);
  GColor foreground = selected ? GColorWhite : GColorBlack;
  graphics_context_set_fill_color(ctx, selected ? PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack) : GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, foreground);

  char label[STATION_NAME_MAX + 4], count[8] = "";
  if (row->kind == RowRecent) snprintf(label, sizeof(label), "%s", s_picker->recents.names[row->first]);
  else if (row->kind == RowStation) snprintf(label, sizeof(label), "%s", name_at(row->first));
  else {
    char prefix[STATION_NAME_MAX];
    display_prefix(name_at(row->first), row->plen, prefix, sizeof(prefix));
    snprintf(label, sizeof(label), "%s...", prefix);
    snprintf(count, sizeof(count), "%u", (unsigned)(row->last - row->first));
  }
  const bool large = bounds.size.w >= 200;
  const int count_w = count[0] ? 28 : 0;
  const int inset = row_inset(bounds);
  GRect area = GRect(inset, 0, bounds.size.w - 2 * inset - count_w, bounds.size.h);
  const char *font = large ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD;
  if (row->kind != RowGroup) font = large ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18;
  // Long names step down one size instead of being cut off.
  if (graphics_text_layout_get_content_size(label, fonts_get_system_font(font), GRect(0, 0, 500, 40),
                                            GTextOverflowModeFill, GTextAlignmentLeft).w > area.size.w)
    font = large ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14;
  const int text_h = large ? 28 : 22;
  area.origin.y = (bounds.size.h - text_h) / 2 - (large ? 3 : 2);
  area.size.h = text_h + 6;
  graphics_draw_text(ctx, label, fonts_get_system_font(font), area, GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  if (count[0]) {
    graphics_draw_text(ctx, count, fonts_get_system_font(FONT_KEY_GOTHIC_14), GRect(bounds.size.w - count_w - inset + 2, bounds.size.h / 2 - 9, count_w, 18),
                       GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }
}

static void select_row(MenuLayer *layer, MenuIndex *index, void *context) {
  const Row *row = row_at(index);
  if (!row) return;
  if (row->kind == RowGroup) { zoom_in(row); return; }
  char name[STATION_NAME_MAX];
  snprintf(name, sizeof(name), "%s", row->kind == RowRecent ? s_picker->recents.names[row->first] : name_at(row->first));
  remember(name);
  StationPickedHandler handler = s_picker->handler;
  void *handler_context = s_picker->context;
  window_stack_pop(true);  // frees s_picker
  if (handler) handler(name, handler_context);
}

static void back_click(ClickRecognizerRef recognizer, void *context) {
  if (s_picker->depth > 0) { s_picker->depth--; reload(true); }
  else window_stack_pop(true);
}

// The buttons are subscribed here instead of with menu_layer_set_click_config_onto_window(),
// which installs its own provider and would replace this one (Back needs to zoom out).
static void up_click(ClickRecognizerRef recognizer, void *context) {
  menu_layer_set_selected_next(s_picker->menu, true, MenuRowAlignCenter, true);
}

static void down_click(ClickRecognizerRef recognizer, void *context) {
  menu_layer_set_selected_next(s_picker->menu, false, MenuRowAlignCenter, true);
}

static void select_click(ClickRecognizerRef recognizer, void *context) {
  MenuIndex index = menu_layer_get_selected_index(s_picker->menu);
  select_row(s_picker->menu, &index, NULL);
}

static void click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

// ---- window --------------------------------------------------------------------

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  s_picker->menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_picker->menu, NULL, (MenuLayerCallbacks) {
    .get_num_sections = num_sections, .get_num_rows = num_rows, .get_header_height = header_height,
    .get_cell_height = cell_height, .draw_header = draw_header, .draw_row = draw_row,
  });
  menu_layer_set_normal_colors(s_picker->menu, GColorWhite, GColorBlack);
  menu_layer_set_highlight_colors(s_picker->menu, PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack), GColorWhite);
  layer_add_child(root, menu_layer_get_layer(s_picker->menu));
  window_set_click_config_provider(window, click_config);
  reload(true);
}

static void window_unload(Window *window) {
  Picker *picker = s_picker;
  s_picker = NULL;
  menu_layer_destroy(picker->menu);
  window_destroy(picker->window);
  free(picker->offsets);
  free(picker->data);
  free(picker);
}

void station_picker_push(const char *title, const char *current, StationPickedHandler handler, void *context) {
  if (s_picker) return;
  Picker *picker = calloc(1, sizeof(Picker));
  if (!picker) return;
  s_picker = picker;
  if (!load_stations(picker)) {
    free(picker->offsets);
    free(picker->data);
    free(picker);
    s_picker = NULL;
    return;
  }
  snprintf(picker->title, sizeof(picker->title), "%s", title);
  picker->handler = handler;
  picker->context = context;
  picker->levels[0] = (Level){ .first = 0, .last = picker->count, .plen = 0 };
  Recents stored;
  if (persist_read_data(RECENT_KEY, &stored, sizeof(stored)) == (int)sizeof(stored)) picker->recents = stored;
  // The station being replaced goes first; the stored list follows without duplicates.
  if (current && current[0]) {
    Recents merged;
    memset(&merged, 0, sizeof(merged));
    snprintf(merged.names[0], STATION_NAME_MAX, "%s", current);
    int used = 1;
    for (int i = 0; i < MAX_RECENT && used < MAX_RECENT; i++) {
      if (picker->recents.names[i][0] && strcmp(picker->recents.names[i], current) != 0)
        memcpy(merged.names[used++], picker->recents.names[i], STATION_NAME_MAX);
    }
    picker->recents = merged;
  }
  picker->window = window_create();
  window_set_window_handlers(picker->window, (WindowHandlers) { .load = window_load, .unload = window_unload });
  window_stack_push(picker->window, true);
}
