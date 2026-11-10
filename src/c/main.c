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

// Metro line from the departure station down to the destination, with a ring for each layover.
static void draw_map(GContext *ctx, int x, int top, int bottom, int layovers, bool large, GColor color) {
  const int end_radius = large ? 7 : 5, ring_radius = large ? 5 : 4;
  graphics_context_set_stroke_color(ctx, color);
  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_width(ctx, large ? 4 : 3);
  graphics_draw_line(ctx, GPoint(x, top), GPoint(x, bottom));
  graphics_fill_circle(ctx, GPoint(x, top), end_radius);
  graphics_fill_circle(ctx, GPoint(x, bottom), end_radius);
  graphics_context_set_stroke_width(ctx, 2);
  for (int i = 1; i <= layovers; i++) {
    GPoint centre = GPoint(x, top + (bottom - top) * i / (layovers + 1));
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, centre, ring_radius);
    graphics_draw_circle(ctx, centre, ring_radius);
  }
}

// ---- layout ----------------------------------------------------------------------

typedef struct {
  bool large;
  int text_x, text_w, map_x, footer_y;
  int from_y, from_detail_y, to_y, to_detail_y;
  int dot_top, dot_bottom;
  int label_y, count_y, count_h, status_y, via_y;
} Layout;

static Layout layout_for(GRect bounds) {
  const bool large = bounds.size.h >= 228;
  if (large) {
    return (Layout) { true, 6, 170, bounds.size.w - 14, 206, 2, 30, 152, 180, 18, 168, 58, 76, 36, 112, 134 };
  }
  return (Layout) { false, 4, 122, bounds.size.w - 10, 150, 0, 21, 112, 133, 12, 124, 40, 54, 30, 82, 96 };
}

static Zone zone_at(int y) {
  const Layout layout = layout_for(layer_get_bounds(s_layer));
  if (y < layout.label_y) return ZoneFrom;
  if (y >= layout.to_y - 2 && y < layout.footer_y) return ZoneTo;
  return ZoneNone;
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

  draw_fit(ctx, s_from, name_big, name_small, GRect(L.text_x, L.from_y, L.text_w, large ? 28 : 22), GTextAlignmentLeft);
  draw_fit(ctx, s_to, name_big, name_small, GRect(L.text_x, L.to_y, L.text_w, large ? 28 : 22), GTextAlignmentLeft);

  const bool has_board = s_board.count > 0;
  Train *train = has_board ? &s_trains[s_selected] : NULL;
  GColor state_color = PBL_IF_COLOR_ELSE(GColorDarkGreen, GColorBlack);
  GColor line_color = PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack);
  if (train && (train->canceled || train->delay > 0)) {
    state_color = line_color = PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack);
  }
  if (!has_board || s_stale) line_color = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
  if (has_board && s_stale) state_color = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
  draw_map(ctx, L.map_x, L.dot_top, L.dot_bottom, train ? (train->transfers > MAX_LAYOVERS ? MAX_LAYOVERS : train->transfers) : 0,
           large, line_color);

  if (!has_board) {
    draw_text(ctx, s_status, large ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD,
              GRect(L.text_x, L.label_y + (large ? 10 : 4), L.text_w, large ? 64 : 50), GTextAlignmentLeft);
    draw_text(ctx, large ? "Tap top/bottom or hold\nUP/DOWN to change stations" : "Hold UP/DOWN to\nchange stations",
              FONT_KEY_GOTHIC_14, GRect(L.text_x, large ? L.via_y - 20 : L.via_y - 6, L.text_w, 34), GTextAlignmentLeft);
    return;
  }

  int32_t remaining = (int32_t)train->departure - (int32_t)time(NULL);
  char departure[8], arrival[8];
  clock_text(train->departure, departure, sizeof(departure));
  clock_text(train->arrival, arrival, sizeof(arrival));
  snprintf(line, sizeof(line), large ? "%s  Platform %s" : "%s  Pl. %s", departure, train->platform);
  draw_text(ctx, line, detail_font, GRect(L.text_x, L.from_detail_y, L.text_w, large ? 22 : 18), GTextAlignmentLeft);
  draw_text(ctx, arrival, detail_font, GRect(L.text_x, L.to_detail_y, L.text_w, large ? 22 : 18), GTextAlignmentLeft);

  graphics_context_set_text_color(ctx, state_color);
  draw_text(ctx, train->canceled ? "JOURNEY" : "DEPARTS IN", FONT_KEY_GOTHIC_14_BOLD,
            GRect(L.text_x, L.label_y, L.text_w, 18), GTextAlignmentLeft);
  if (train->canceled) snprintf(line, sizeof(line), "CANCELED");
  else if (remaining < 0) snprintf(line, sizeof(line), "Departed");
  else countdown_text(remaining, line, sizeof(line));
  draw_fit(ctx, line, large ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD,
           large ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD,
           GRect(L.text_x, L.count_y, L.text_w, L.count_h), GTextAlignmentLeft);
  if (train->canceled) snprintf(line, sizeof(line), "Choose another journey");
  else if (train->delay > 0) snprintf(line, sizeof(line), "+%ld min late", (long)((train->delay + 59) / 60));
  else snprintf(line, sizeof(line), "On time");
  draw_text(ctx, line, large ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD,
            GRect(L.text_x, L.status_y, L.text_w, large ? 22 : 16), GTextAlignmentLeft);

  graphics_context_set_text_color(ctx, GColorBlack);
  if (train->transfers && train->via[0]) snprintf(line, sizeof(line), "via %s", train->via);
  else if (train->transfers) snprintf(line, sizeof(line), "%u change%s", train->transfers, train->transfers == 1 ? "" : "s");
  else snprintf(line, sizeof(line), "%s / direct", train->train);
  draw_text(ctx, line, FONT_KEY_GOTHIC_14, GRect(L.text_x, L.via_y, L.text_w, 18), GTextAlignmentLeft);

  graphics_draw_line(ctx, GPoint(0, L.footer_y), GPoint(width, L.footer_y));
  char updated[8];
  clock_text(s_board.updated, updated, sizeof(updated));
  snprintf(line, sizeof(line), "%u/%u  %s%s", s_selected + 1, s_board.count,
           s_loading ? "Updating" : s_stale ? "Cached " : "Updated ", s_loading ? "" : updated);
  draw_text(ctx, line, FONT_KEY_GOTHIC_14, GRect(2, L.footer_y, width - 4, 18), GTextAlignmentCenter);
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
    s_board = s_pending;
    memcpy(s_trains, s_rows, sizeof(s_trains));
    snprintf(s_from, sizeof(s_from), "%s", s_board.from);
    snprintf(s_to, sizeof(s_to), "%s", s_board.to);
    s_route_pending = false;
    s_board_token = s_token;
    s_selected = 0;
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
  if (s_board.updated && now - s_board.updated > 120) s_stale = true;
  if (s_ready && !s_loading && tick_time->tm_sec == 0) request(COMMAND_REFRESH);
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
  touch_service_subscribe(touch, NULL);
  window_stack_push(s_window, true);
  app_event_loop();
  touch_service_unsubscribe();
  tick_timer_service_unsubscribe();
  connection_service_unsubscribe();
  layer_destroy(s_layer);
  window_destroy(s_window);
}
