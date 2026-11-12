#include <pebble.h>
#include "detail.h"
#include "stations.h"

#define MAX_TRAINS 6
#define MAX_LAYOVERS 4
#define CACHE_META 100
#define CACHE_ROW 101
#define CACHE_VERSION 2
#define COMMAND_REFRESH 1
#define COMMAND_SET_ROUTE 3

typedef struct {
  uint32_t departure, arrival;
  int32_t delay;
  uint8_t transfers, canceled;
  char platform[12], train[24], via[48];
} Train;

typedef struct {
  uint32_t version, updated;
  uint8_t count;
  char from[STATION_NAME_MAX], to[STATION_NAME_MAX];
} Board;

static Window *s_window;
static Layer *s_layer;
static Board s_board, s_pending;
static Train s_trains[MAX_TRAINS], s_rows[MAX_TRAINS];
static uint8_t s_selected, s_received;
static int32_t s_token;
// Token of the board on screen when it came from the phone in this session; 0 for a cached board.
static int32_t s_board_token;
// Per journey on the board: its platform changed since the board was first seen (not persisted).
static bool s_platform_changed[MAX_TRAINS];
static bool s_loading, s_ready, s_stale = true;
// True from picking a new route until the phone answers with a board for it.
static bool s_route_pending;
static time_t s_requested;
// The route being shown. Each board carries its own names, which replace these.
static char s_from[STATION_NAME_MAX] = "Gent-Sint-Pieters", s_to[STATION_NAME_MAX] = "Brussel-Zuid";
static char s_status[64] = "Waiting for phone app";

typedef enum { ZoneNone, ZoneFrom, ZoneTo } Zone;
static Zone s_touch_zone;
static int16_t s_touch_start_y;

// ---- drawing helpers -----------------------------------------------------------

static void draw_text(GContext *ctx, const char *text, const char *font, GRect rect,
                      GTextAlignment alignment) {
  graphics_draw_text(ctx, text, fonts_get_system_font(font), rect,
                     GTextOverflowModeTrailingEllipsis, alignment, NULL);
}

// Draws with `big` unless the text would not fit the width, then with `small`.
static void draw_fit(GContext *ctx, const char *text, const char *big, const char *small, GRect rect,
                     GTextAlignment alignment) {
  GSize size = graphics_text_layout_get_content_size(text, fonts_get_system_font(big), GRect(0, 0, 500, 60),
                                                     GTextOverflowModeFill, GTextAlignmentLeft);
  draw_text(ctx, text, size.w > rect.size.w ? small : big, rect, alignment);
}

static int text_width(const char *text, const char *font) {
  return graphics_text_layout_get_content_size(text, fonts_get_system_font(font), GRect(0, 0, 500, 60),
                                               GTextOverflowModeFill, GTextAlignmentLeft).w;
}

static void clock_text(uint32_t timestamp, char *buffer, size_t size) {
  time_t value = timestamp;
  strftime(buffer, size, "%H:%M", localtime(&value));
}

// Whole seconds until departure as "5h 13m 30s"; leading zero units are omitted.
static void countdown_text(int32_t remaining, char *buffer, size_t size) {
  long hours = remaining / 3600, minutes = (remaining % 3600) / 60, seconds = remaining % 60;
  if (hours) snprintf(buffer, size, "%ldh %ldm %lds", hours, minutes, seconds);
  else if (minutes) snprintf(buffer, size, "%ldm %lds", minutes, seconds);
  else snprintf(buffer, size, "%lds", seconds);
}

// A delay in seconds as "4 min" or, from an hour up, "1h 05m". Rounded up to whole minutes.
static void delay_text(int32_t delay, char *buffer, size_t size) {
  long minutes = (delay + 59) / 60;
  if (minutes < 60) snprintf(buffer, size, "%ld min", minutes);
  else if (minutes % 60) snprintf(buffer, size, "%ldh %02ldm", minutes / 60, minutes % 60);
  else snprintf(buffer, size, "%ldh", minutes / 60);
}

// ---- layout ----------------------------------------------------------------------

typedef struct {
  bool large;
  int text_x, text_w, map_x, footer_y;
  int from_y, from_detail_y, to_y, to_detail_y;
  int dot_top, dot_bottom;
  int label_y, count_y, count_h, status_y, via_y;
  bool round;  // round screens: rows are fitted to the circle
  int arc;     // round screens: radius of the circle the map follows, centred on the screen
} Layout;

static Layout layout_for(GRect bounds) {
#if defined(PBL_ROUND)
  // Round screens: the map runs down the left, the text sits right of it, and no row is wider
  // than the circle allows at its height (see row()). The via line is folded into the arrival
  // line to make room.
  if (bounds.size.w >= 240) {  // Pebble Round 2, 260 x 260: the large fonts of the Time 2
    return (Layout) { true, 50, 160, 32, 220, 28, 58, 164, 192, 68, 203, 84, 100, 36, 138, 0, true, 118 };
  }
  return (Layout) { false, 36, 108, 24, 157, 18, 38, 120, 140, 46, 134, 58, 72, 30, 102, 0, true, 82 };  // Time Round, 180 x 180
#endif
  const bool large = bounds.size.h >= 228;
  if (large) {
    return (Layout) { true, 30, 166, 12, 206, 2, 30, 152, 180, 18, 168, 58, 76, 36, 112, 134, false, 0 };
  }
  return (Layout) { false, 20, 121, 8, 150, 0, 21, 112, 133, 12, 124, 40, 54, 30, 82, 96, false, 0 };
}

// The area for text between y and y + h. On the round screen it is the chord of the circle at
// that height, kept clear of the map on the left unless `centered` (the footer).
static int isqrt(int n) {
  int r = 0;
  while ((r + 1) * (r + 1) <= n) r++;
  return r;
}

static GRect row(const Layout *L, int y, int h, bool centered) {
  if (!L->round) return GRect(L->text_x, y, L->text_w, h);
  const int radius = PBL_DISPLAY_WIDTH / 2;
  const int top = abs(y + 3 - radius), bottom = abs(y + h - 3 - radius);
  const int far = top > bottom ? top : bottom;
  const int inset = radius - (far >= radius ? 0 : isqrt(radius * radius - far * far)) + 3;
  const int x = !centered && inset < L->text_x ? L->text_x : inset;
  return GRect(x, y, PBL_DISPLAY_WIDTH - x - inset, h);
}

// Where the map is at height y: a straight line down the left, or on a round screen an arc of the
// circle of radius L->arc that runs along the curve of the screen.
static int map_x_at(const Layout *L, int y) {
  if (!L->round) return L->map_x;
  const int centre = PBL_DISPLAY_WIDTH / 2, dy = y - centre;
  return centre - isqrt(L->arc * L->arc - dy * dy);
}

// Metro line from the departure station down to the destination, with a ring for each layover.
static void draw_map(GContext *ctx, const Layout *L, int layovers, GColor color) {
  const int top = L->dot_top, bottom = L->dot_bottom;
  const int end_radius = L->large ? 7 : 5, ring_radius = L->large ? 5 : 4;
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_width(ctx, L->large ? 4 : 3);
  const int step = L->round ? 4 : bottom - top;  // short segments make the arc
  for (int y = top; y < bottom; y += step) {
    const int next = y + step < bottom ? y + step : bottom;
    graphics_draw_line(ctx, GPoint(map_x_at(L, y), y), GPoint(map_x_at(L, next), next));
  }
  graphics_fill_circle(ctx, GPoint(map_x_at(L, top), top), end_radius);
  graphics_fill_circle(ctx, GPoint(map_x_at(L, bottom), bottom), end_radius);
  graphics_context_set_stroke_width(ctx, 2);
  for (int i = 1; i <= layovers; i++) {
    const int y = top + (bottom - top) * i / (layovers + 1);
    GPoint centre = GPoint(map_x_at(L, y), y);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, centre, ring_radius);
    graphics_draw_circle(ctx, centre, ring_radius);
  }
}

static Zone zone_at(int y) {
  const Layout layout = layout_for(layer_get_bounds(s_layer));
  if (y < layout.label_y) return ZoneFrom;
  if (y >= layout.to_y - 2 && y < layout.footer_y) return ZoneTo;
  return ZoneNone;
}

// ---- footer: journey number, the time now and how far the data can be trusted --------------

typedef enum { FeedLive, FeedUpdating, FeedCached, FeedOffline } Feed;

static Feed feed_state(void) {
  if (!connection_service_peek_pebble_app_connection()) return FeedOffline;
  if (s_loading) return FeedUpdating;
  return s_stale ? FeedCached : FeedLive;
}

static GColor feed_color(Feed state) {
  switch (state) {
    case FeedLive: return PBL_IF_COLOR_ELSE(GColorDarkGreen, GColorBlack);
    case FeedUpdating: return PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack);
    case FeedCached: return PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
    default: return PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack);
  }
}

// A 9 px symbol that also tells the states apart without color: a dot (live), a ring with a dot
// running around it (updating), a square (cached, standing still) and a cross (no phone).
static void draw_feed_symbol(GContext *ctx, GPoint c, Feed state, time_t now) {
  graphics_context_set_stroke_color(ctx, feed_color(state));
  graphics_context_set_fill_color(ctx, feed_color(state));
  graphics_context_set_stroke_width(ctx, 1);
  if (state == FeedLive) {
    graphics_fill_circle(ctx, c, 4);
  } else if (state == FeedUpdating) {
    static const int8_t orbit[4][2] = { { 0, -4 }, { 4, 0 }, { 0, 4 }, { -4, 0 } };
    graphics_draw_circle(ctx, c, 4);
    graphics_fill_circle(ctx, GPoint(c.x + orbit[now % 4][0], c.y + orbit[now % 4][1]), 2);
  } else if (state == FeedCached) {
    graphics_fill_rect(ctx, GRect(c.x - 3, c.y - 3, 7, 7), 0, GCornerNone);
  } else {
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(c.x - 3, c.y - 3), GPoint(c.x + 3, c.y + 3));
    graphics_draw_line(ctx, GPoint(c.x - 3, c.y + 3), GPoint(c.x + 3, c.y - 3));
  }
}

static void feed_word(Feed state, time_t now, char *buffer, size_t size) {
  if (state == FeedCached) {
    const long minutes = s_board.updated && now > (time_t)s_board.updated ? (now - s_board.updated) / 60 : 0;
    if (minutes >= 60) snprintf(buffer, size, "Cached %ldh", minutes / 60);
    else if (minutes >= 1) snprintf(buffer, size, "Cached %ldm", minutes);
    else snprintf(buffer, size, "Cached");
  } else {
    snprintf(buffer, size, "%s", state == FeedLive ? "Live" : state == FeedUpdating ? "Updating" : "Offline");
  }
}

// "1/2  22:07  ● Live", centered; where the screen is narrow (the Time Round) only the symbol is shown.
static void draw_footer(GContext *ctx, const Layout *L, int width) {
  const time_t now = time(NULL);
  const Feed state = feed_state();
  const bool words = !L->round || L->large;
  const char *font = FONT_KEY_GOTHIC_14, *bold = FONT_KEY_GOTHIC_14_BOLD;
  char index[8], clock[8], word[24] = "";
  snprintf(index, sizeof(index), "%u/%u", s_selected + 1, s_board.count);
  clock_text(now, clock, sizeof(clock));
  if (words) feed_word(state, now, word, sizeof(word));
  const int gap = 8, symbol = 9, label_gap = 4;
  const int index_w = text_width(index, font), clock_w = text_width(clock, bold), word_w = words ? text_width(word, font) : 0;
  const int total = index_w + gap + clock_w + gap + symbol + (words ? label_gap + word_w : 0);
  const GRect area = L->round ? row(L, L->footer_y, 16, true) : GRect(0, L->footer_y, width, 18);
  int x = area.origin.x + (area.size.w - total) / 2;
  if (x < area.origin.x) x = area.origin.x;
  const int y = area.origin.y;

  graphics_context_set_text_color(ctx, GColorBlack);
  draw_text(ctx, index, font, GRect(x, y, index_w + 6, 18), GTextAlignmentLeft);
  x += index_w + gap;
  draw_text(ctx, clock, bold, GRect(x, y, clock_w + 6, 18), GTextAlignmentLeft);
  x += clock_w + gap;
  draw_feed_symbol(ctx, GPoint(x + symbol / 2, y + 9), state, now);
  if (words) {
    graphics_context_set_text_color(ctx, feed_color(state));
    draw_text(ctx, word, font, GRect(x + symbol + label_gap, y, word_w + 6, 18), GTextAlignmentLeft);
  }
}

static void render(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds(layer);
  const Layout L = layout_for(bounds);
  const bool large = L.large;
  const int width = bounds.size.w;
  const char *name_big = large ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD;
  const char *name_small = large ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD;
  const char *detail_font = large ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorBlack);
  char line[100];

  draw_fit(ctx, s_from, name_big, name_small, row(&L, L.from_y, large ? 28 : 22, false), GTextAlignmentLeft);
  draw_fit(ctx, s_to, name_big, name_small, row(&L, L.to_y, large ? 28 : 22, false), GTextAlignmentLeft);

  const bool has_board = s_board.count > 0;
  Train *train = has_board ? &s_trains[s_selected] : NULL;
  GColor state_color = PBL_IF_COLOR_ELSE(GColorDarkGreen, GColorBlack);
  GColor line_color = PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack);
  if (train && (train->canceled || train->delay > 0)) {
    state_color = line_color = PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack);
  }
  if (!has_board || s_stale) line_color = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
  if (has_board && s_stale) state_color = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
  draw_map(ctx, &L, train ? (train->transfers > MAX_LAYOVERS ? MAX_LAYOVERS : train->transfers) : 0, line_color);

  if (!has_board) {
    draw_text(ctx, s_status, large ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD,
              row(&L, L.round ? L.from_detail_y : L.label_y + (large ? 10 : 4), large ? 64 : 50, false), GTextAlignmentLeft);
    draw_text(ctx, large ? "Tap top/bottom or hold\nUP/DOWN to change stations" : "Hold UP/DOWN to\nchange stations",
              FONT_KEY_GOTHIC_14, row(&L, L.round ? L.from_detail_y + (large ? 66 : 46) : large ? L.via_y - 20 : L.via_y - 6, 34, false), GTextAlignmentLeft);
    return;
  }

  int32_t remaining = (int32_t)train->departure - (int32_t)time(NULL);
  char departure[8], arrival[8];
  // The top shows the planned time (the train you are taking); the delay is shown below.
  clock_text(train->departure - train->delay, departure, sizeof(departure));
  clock_text(train->arrival, arrival, sizeof(arrival));
  snprintf(line, sizeof(line), large ? "%s  Platform %s" : "%s  Pl. %s", departure, train->platform);
  const GRect departure_area = row(&L, L.from_detail_y, large ? 22 : 18, false);
  if (s_platform_changed[s_selected]) {
    // The platform changed: it is drawn bold on an accent colored box (black on a black and white watch).
    const char *bold_font = large ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD;
    char platform[24];
    snprintf(platform, sizeof(platform), large ? "Platform %s" : "Pl. %s", train->platform);
    snprintf(line, sizeof(line), "%s  x", departure);
    const int offset = text_width(line, detail_font) - text_width("x", detail_font);
    const GRect platform_area = GRect(departure_area.origin.x + offset, departure_area.origin.y,
                                      departure_area.size.w - offset, departure_area.size.h);
    draw_text(ctx, departure, detail_font, departure_area, GTextAlignmentLeft);
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorOrange, GColorBlack));
    graphics_fill_rect(ctx, GRect(platform_area.origin.x - 2, platform_area.origin.y + (large ? 3 : 2),
                                  text_width(platform, bold_font) + 4, large ? 19 : 15), 0, GCornerNone);
    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite));
    draw_text(ctx, platform, bold_font, platform_area, GTextAlignmentLeft);
    graphics_context_set_text_color(ctx, GColorBlack);
  } else {
    draw_text(ctx, line, detail_font, departure_area, GTextAlignmentLeft);
  }
  char via_line[64];
  if (train->transfers && train->via[0]) snprintf(via_line, sizeof(via_line), "via %s", train->via);
  else if (train->transfers) snprintf(via_line, sizeof(via_line), "%u change%s", train->transfers, train->transfers == 1 ? "" : "s");
  else snprintf(via_line, sizeof(via_line), "%s / direct", train->train);
  // The round screen has no room for a line of its own, so the route shares the arrival line.
  if (L.round) snprintf(line, sizeof(line), "%s  %s", arrival, via_line);
  else snprintf(line, sizeof(line), "%s", arrival);
  draw_text(ctx, line, detail_font, row(&L, L.to_detail_y, large ? 22 : 18, false), GTextAlignmentLeft);

  graphics_context_set_text_color(ctx, state_color);
  // The countdown runs to the expected departure, so a delayed train says by how much it is late.
  char label[32] = "DEPARTS IN";
  if (train->canceled) snprintf(label, sizeof(label), "JOURNEY");
  else if (train->delay > 0) {
    char amount[16];
    delay_text(train->delay, amount, sizeof(amount));
    snprintf(label, sizeof(label), "DELAYED %s", amount);
    for (char *c = label; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 'a' - 'A';
  }
  draw_text(ctx, label, FONT_KEY_GOTHIC_14_BOLD, row(&L, L.label_y, 18, false), GTextAlignmentLeft);
  if (train->canceled) snprintf(line, sizeof(line), "CANCELED");
  else if (remaining < 0) snprintf(line, sizeof(line), "Departed");
  else countdown_text(remaining, line, sizeof(line));
  draw_fit(ctx, line, large ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD,
           large ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD,
           row(&L, L.count_y, L.count_h, false), GTextAlignmentLeft);
  // The small screen has room for fewer characters.
  if (train->canceled) snprintf(line, sizeof(line), large ? "Choose another journey" : "Pick another");
  else if (train->delay > 0) {
    char expected[8];
    clock_text(train->departure, expected, sizeof(expected));
    snprintf(line, sizeof(line), "Leaves at %s", expected);
  }
  else snprintf(line, sizeof(line), "On time");
  draw_text(ctx, line, large ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD,
            row(&L, L.status_y, large ? 22 : 16, false), GTextAlignmentLeft);

  graphics_context_set_text_color(ctx, GColorBlack);
  if (!L.round) draw_text(ctx, via_line, FONT_KEY_GOTHIC_14, row(&L, L.via_y, 18, false), GTextAlignmentLeft);

  if (!L.round) graphics_draw_line(ctx, GPoint(0, L.footer_y), GPoint(width, L.footer_y));
  draw_footer(ctx, &L, width);
}

static void redraw(void) { if (s_layer) layer_mark_dirty(s_layer); }

// ---- phone link --------------------------------------------------------------------

static void request(int command) {
  if (s_loading) return;
  if (!connection_service_peek_pebble_app_connection()) {
    snprintf(s_status, sizeof(s_status), "Phone disconnected");
    s_stale = true;
  } else if (!s_ready) {
    snprintf(s_status, sizeof(s_status), "Waiting for phone app");
  } else {
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK && iter) {
      dict_write_int32(iter, MESSAGE_KEY_Command, command);
      if (app_message_outbox_send() == APP_MSG_OK) {
        s_loading = true;
        s_requested = time(NULL);
        snprintf(s_status, sizeof(s_status), "Updating...");
      }
    }
  }
  redraw();
}

// Tells the phone which stations to plan, and shows them straight away.
static void set_route(const char *from, const char *to) {
  if (!connection_service_peek_pebble_app_connection()) {
    snprintf(s_status, sizeof(s_status), "Phone disconnected");
    s_stale = true;
    redraw();
    return;
  }
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK || !iter) return;
  dict_write_int32(iter, MESSAGE_KEY_Command, COMMAND_SET_ROUTE);
  dict_write_cstring(iter, MESSAGE_KEY_From, from);
  dict_write_cstring(iter, MESSAGE_KEY_To, to);
  if (app_message_outbox_send() != APP_MSG_OK) return;
  snprintf(s_from, sizeof(s_from), "%s", from);
  snprintf(s_to, sizeof(s_to), "%s", to);
  // The old board belongs to the old route; drop it until the phone answers.
  memset(&s_board, 0, sizeof(s_board));
  memset(s_platform_changed, 0, sizeof(s_platform_changed));
  s_board_token = 0;
  persist_delete(CACHE_META);
  s_selected = 0;
  s_stale = true;
  s_loading = true;
  s_route_pending = true;
  s_requested = time(NULL);
  snprintf(s_status, sizeof(s_status), "Updating...");
  redraw();
}

static void station_picked(const char *name, void *context) {
  const bool editing_from = context != NULL;
  char from[STATION_NAME_MAX], to[STATION_NAME_MAX];
  snprintf(from, sizeof(from), "%s", s_from);
  snprintf(to, sizeof(to), "%s", s_to);
  if (strcmp(name, editing_from ? from : to) == 0) return;  // unchanged
  if (strcmp(name, editing_from ? to : from) == 0) {         // picked the other end: swap
    set_route(to, from);
  } else if (editing_from) {
    set_route(name, to);
  } else {
    set_route(from, name);
  }
}

static void open_picker(Zone zone) {
  if (zone == ZoneFrom) station_picker_push("From", s_from, station_picked, (void *)1);
  else if (zone == ZoneTo) station_picker_push("To", s_to, station_picked, NULL);
}

static void copy_text(DictionaryIterator *iter, uint32_t key, char *buffer, size_t size) {
  Tuple *tuple = dict_find(iter, key);
  if (tuple && tuple->type == TUPLE_CSTRING) snprintf(buffer, size, "%s", tuple->value->cstring);
}

static int32_t number(DictionaryIterator *iter, uint32_t key) {
  Tuple *tuple = dict_find(iter, key);
  return tuple ? tuple->value->int32 : 0;
}

// A journey keeps its planned departure and train when its delay changes, so those identify it
// across refreshes (the expected departure moves with the delay).
static bool same_journey(const Train *a, const Train *b) {
  return (int32_t)a->departure - a->delay == (int32_t)b->departure - b->delay && strcmp(a->train, b->train) == 0;
}

// "?" means the platform is not known (yet), which is not a change.
static bool platform_known(const char *platform) { return platform[0] && strcmp(platform, "?") != 0; }
static bool platform_moved(const Train *before, const Train *after) {
  return platform_known(before->platform) && platform_known(after->platform) && strcmp(before->platform, after->platform) != 0;
}

// Buzzes when the journey the user follows was canceled (long), moved to another platform (two
// pulses) or its delay changed (short).
static void buzz_on_change(const Train *before, const Train *after) {
  if (!before->canceled && after->canceled) vibes_long_pulse();
  else if (!after->canceled && platform_moved(before, after)) vibes_double_pulse();
  else if (!after->canceled && before->delay != after->delay) vibes_short_pulse();
}

static void inbox(DictionaryIterator *iter, void *context) {
  s_ready = true;
  int kind = number(iter, MESSAGE_KEY_Kind);
  if (kind >= 4) {  // trip overview data
    detail_handle_message(iter, kind);
    return;
  }
  if (kind == 0) {
    copy_text(iter, MESSAGE_KEY_Status, s_status, sizeof(s_status));
    s_loading = strcmp(s_status, "Updating...") == 0 || strcmp(s_status, "Already updating") == 0;
    if (s_loading) s_requested = time(NULL);
    else { s_stale = true; s_route_pending = false; }
  } else if (kind == 1) {
    int count = number(iter, MESSAGE_KEY_Count);
    if (count < 0 || count > MAX_TRAINS) return;
    memset(&s_pending, 0, sizeof(s_pending));
    memset(s_rows, 0, sizeof(s_rows));
    s_pending.count = count;
    s_pending.version = CACHE_VERSION;
    s_pending.updated = number(iter, MESSAGE_KEY_Updated);
    copy_text(iter, MESSAGE_KEY_From, s_pending.from, sizeof(s_pending.from));
    copy_text(iter, MESSAGE_KEY_To, s_pending.to, sizeof(s_pending.to));
    if (s_route_pending && (strcmp(s_pending.from, s_from) != 0 || strcmp(s_pending.to, s_to) != 0)) {
      // A board for the route we just left, still in flight. Wait for the new one.
      s_token = -1;
      return;
    }
    s_token = number(iter, MESSAGE_KEY_Token);
    s_received = 0;
    s_loading = true;
    s_requested = time(NULL);
  } else if (kind == 2 && s_loading && number(iter, MESSAGE_KEY_Token) == s_token) {
    int index = number(iter, MESSAGE_KEY_Index);
    if (index < 0 || index >= s_pending.count) return;
    Train *train = &s_rows[index];
    train->departure = number(iter, MESSAGE_KEY_Departure);
    train->arrival = number(iter, MESSAGE_KEY_Arrival);
    train->delay = number(iter, MESSAGE_KEY_Delay);
    train->transfers = number(iter, MESSAGE_KEY_Transfers);
    train->canceled = number(iter, MESSAGE_KEY_Canceled) != 0;
    copy_text(iter, MESSAGE_KEY_Platform, train->platform, sizeof(train->platform));
    copy_text(iter, MESSAGE_KEY_Train, train->train, sizeof(train->train));
    copy_text(iter, MESSAGE_KEY_Via, train->via, sizeof(train->via));
    s_received |= 1 << index;
  } else if (kind == 3 && s_loading && number(iter, MESSAGE_KEY_Token) == s_token &&
             s_received == (1 << s_pending.count) - 1) {
    // A refresh of the same route must not move the user off the journey they are following.
    const bool follow = s_board.count > 0 && s_selected < s_board.count &&
                        strcmp(s_board.from, s_pending.from) == 0 && strcmp(s_board.to, s_pending.to) == 0;
    const bool live = s_board_token != 0;  // the old board came from the phone, not from the cache
    const Train followed = follow ? s_trains[s_selected] : (Train) { 0 };
    // The journeys of the old board, to find out which platforms changed.
    const bool same_route = s_board.count > 0 && strcmp(s_board.from, s_pending.from) == 0 &&
                            strcmp(s_board.to, s_pending.to) == 0;
    const int previous_count = same_route ? s_board.count : 0;
    Train previous[MAX_TRAINS];
    bool previous_changed[MAX_TRAINS];
    memcpy(previous, s_trains, sizeof(previous));
    memcpy(previous_changed, s_platform_changed, sizeof(previous_changed));
    s_board = s_pending;
    memcpy(s_trains, s_rows, sizeof(s_trains));
    snprintf(s_from, sizeof(s_from), "%s", s_board.from);
    snprintf(s_to, sizeof(s_to), "%s", s_board.to);
    s_route_pending = false;
    s_board_token = s_token;
    s_selected = 0;
    memset(s_platform_changed, 0, sizeof(s_platform_changed));
    for (int i = 0; i < s_board.count; i++) {
      for (int j = 0; j < previous_count; j++) {
        if (!same_journey(&previous[j], &s_trains[i])) continue;
        // Once changed it stays marked, also when this refresh brings no further change.
        s_platform_changed[i] = previous_changed[j] || (live && platform_moved(&previous[j], &s_trains[i]));
        break;
      }
    }
    for (int i = 0; follow && i < s_board.count; i++) {
      if (!same_journey(&followed, &s_trains[i])) continue;
      s_selected = i;
      if (live) buzz_on_change(&followed, &s_trains[i]);
      break;
    }
    s_loading = false;
    s_stale = false;
    snprintf(s_status, sizeof(s_status), "No upcoming trains\nSELECT to try again");
    for (int i = 0; i < s_board.count; i++) persist_write_data(CACHE_ROW + i, &s_trains[i], sizeof(Train));
    persist_write_data(CACHE_META, &s_board, sizeof(s_board));
  }
  redraw();
}

static void failed(AppMessageResult reason, void *context) {
  s_loading = false;
  s_stale = true;
  s_route_pending = false;
  snprintf(s_status, sizeof(s_status), "Transfer failed\nSELECT to retry");
  redraw();
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) {
  if (!detail_outbox_failed()) failed(reason, context);
}

// ---- input ----------------------------------------------------------------------------

// Select opens the trip overview of the journey on screen. The board refreshes itself every
// minute, so Select only refreshes when there is nothing to show yet (or the board is a cached
// one from an earlier session, for which the phone has no details).
static void select_click(ClickRecognizerRef recognizer, void *context) {
  if (s_board.count && s_board_token) detail_push(s_selected, s_board_token);
  else request(COMMAND_REFRESH);
}
static void reverse_click(ClickRecognizerRef recognizer, void *context) {
  char from[STATION_NAME_MAX], to[STATION_NAME_MAX];
  snprintf(from, sizeof(from), "%s", s_from);
  snprintf(to, sizeof(to), "%s", s_to);
  set_route(to, from);
}
static void up_click(ClickRecognizerRef recognizer, void *context) {
  if (s_board.count) s_selected = (s_selected + s_board.count - 1) % s_board.count;
  redraw();
}
static void down_click(ClickRecognizerRef recognizer, void *context) {
  if (s_board.count) s_selected = (s_selected + 1) % s_board.count;
  redraw();
}
static void change_from_click(ClickRecognizerRef recognizer, void *context) { open_picker(ZoneFrom); }
static void change_to_click(ClickRecognizerRef recognizer, void *context) { open_picker(ZoneTo); }

static void clicks(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, reverse_click, NULL);
  // Hold the button on the side of the station you want to change: top = from, bottom = to.
  window_long_click_subscribe(BUTTON_ID_UP, 500, change_from_click, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 500, change_to_click, NULL);
}

// Touch (Pebble Time 2): tapping the departure or destination block opens its picker.
static void touch(const TouchEvent *event, void *context) {
  if (event->non_navigational || window_stack_get_top_window() != s_window) return;
  if (event->type == TouchEvent_Touchdown) {
    s_touch_zone = zone_at(event->y);
    s_touch_start_y = event->y;
  } else if (event->type == TouchEvent_PositionUpdate) {
    if (event->y - s_touch_start_y > 20 || s_touch_start_y - event->y > 20) s_touch_zone = ZoneNone;  // a swipe
  } else if (event->type == TouchEvent_Liftoff) {
    Zone zone = s_touch_zone;
    s_touch_zone = ZoneNone;
    if (zone != ZoneNone && zone_at(event->y) == zone) open_picker(zone);
  }
}

static void tick(struct tm *tick_time, TimeUnits units) {
  time_t now = time(NULL);
  if (s_loading && now - s_requested >= 60) failed(APP_MSG_SEND_TIMEOUT, NULL);
  // Signed on purpose: a watch clock a little behind the phone's must not make fresh data look old.
  if (s_board.updated && (int32_t)now - (int32_t)s_board.updated > 120) s_stale = true;
  // Refresh every minute, and the moment the selected train's countdown ends: a delay that is
  // announced at the last second then shows at once instead of "Departed" until the next minute.
  const bool countdown_ended = s_board.count && s_selected < s_board.count &&
                               (int32_t)s_trains[s_selected].departure == (int32_t)now;
  if (s_ready && !s_loading && (tick_time->tm_sec == 0 || countdown_ended)) request(COMMAND_REFRESH);
  redraw();
}

static void connection_changed(bool connected) {
  if (!connected) {
    s_stale = true;
    s_loading = false;
    snprintf(s_status, sizeof(s_status), "Phone disconnected");
  } else if (s_ready) request(COMMAND_REFRESH);
  redraw();
}

static void load_cache(void) {
  if (persist_read_data(CACHE_META, &s_board, sizeof(s_board)) != sizeof(s_board) ||
      s_board.version != CACHE_VERSION || s_board.count > MAX_TRAINS) {
    memset(&s_board, 0, sizeof(s_board));
    return;
  }
  for (int i = 0; i < s_board.count; i++) {
    if (persist_read_data(CACHE_ROW + i, &s_trains[i], sizeof(Train)) != sizeof(Train)) {
      memset(&s_board, 0, sizeof(s_board));
      return;
    }
  }
  if (s_board.from[0] && s_board.to[0]) {
    snprintf(s_from, sizeof(s_from), "%s", s_board.from);
    snprintf(s_to, sizeof(s_to), "%s", s_board.to);
  }
}

int main(void) {
  load_cache();
  s_window = window_create();
  window_set_click_config_provider(s_window, clicks);
  Layer *root = window_get_root_layer(s_window);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, render);
  layer_add_child(root, s_layer);
  app_message_register_inbox_received(inbox);
  app_message_register_inbox_dropped(failed);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(1024, 192);
  connection_service_subscribe((ConnectionHandlers) { .pebble_app_connection_handler = connection_changed });
  tick_timer_service_subscribe(SECOND_UNIT, tick);
  // Without this the Time 2 does not let a finger scroll the station list.
  app_touch_navigation_enable(true);
  touch_service_subscribe(touch, NULL);
  window_stack_push(s_window, true);
  app_event_loop();
  touch_service_unsubscribe();
  tick_timer_service_unsubscribe();
  connection_service_unsubscribe();
  layer_destroy(s_layer);
  window_destroy(s_window);
}
