#include <pebble.h>

#define MAX_TRAINS 6
#define CACHE_META 100
#define CACHE_ROW 101
#define CACHE_VERSION 1

typedef struct {
  uint32_t departure, arrival;
  int32_t delay;
  uint8_t transfers, canceled;
  char platform[12], train[24];
} Train;

typedef struct {
  uint32_t version, updated;
  uint8_t count;
  char from[48], to[48];
} Board;

static Window *s_window;
static Layer *s_layer;
static Board s_board, s_pending;
static Train s_trains[MAX_TRAINS], s_rows[MAX_TRAINS];
static uint8_t s_selected, s_received;
static int32_t s_token;
static bool s_loading, s_ready, s_stale = true;
static time_t s_requested;
static char s_status[64] = "Open settings in the phone app";

static void draw_text(GContext *ctx, const char *text, const char *font, GRect rect,
                      GTextAlignment alignment) {
  graphics_draw_text(ctx, text, fonts_get_system_font(font), rect,
                     GTextOverflowModeTrailingEllipsis, alignment, NULL);
}

static void clock_text(uint32_t timestamp, char *buffer, size_t size) {
  time_t value = timestamp;
  strftime(buffer, size, "%H:%M", localtime(&value));
}

static void render(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds(layer);
  const int width = bounds.size.w;
  const bool large = bounds.size.h >= 228;
  const int footer_y = bounds.size.h - (large ? 22 : 18);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack));
  graphics_fill_rect(ctx, GRect(0, 0, width, large ? 52 : 38), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  draw_text(ctx, s_board.from[0] ? s_board.from : "Traindown",
            large ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD,
            GRect(large ? 8 : 5, 0, width - (large ? 16 : 10), large ? 25 : 18), GTextAlignmentLeft);
  char line[100];
  snprintf(line, sizeof(line), "to %s", s_board.to[0] ? s_board.to : "Belgian trains");
  draw_text(ctx, line, large ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14,
            GRect(large ? 8 : 5, large ? 24 : 17, width - (large ? 16 : 10), large ? 25 : 19), GTextAlignmentLeft);
  graphics_context_set_text_color(ctx, GColorBlack);

  if (!s_board.count) {
    draw_text(ctx, s_status, large ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD,
              GRect(7, large ? 74 : 52, width - 14, large ? 80 : 64), GTextAlignmentCenter);
    draw_text(ctx, "SELECT: refresh\nHold SELECT: reverse", large ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14,
              GRect(4, large ? 174 : 123, width - 8, large ? 50 : 40), GTextAlignmentCenter);
    return;
  }

  Train *train = &s_trains[s_selected];
  int32_t remaining = (int32_t)train->departure - (int32_t)time(NULL);
  GColor state_color = PBL_IF_COLOR_ELSE(GColorDarkGreen, GColorBlack);
  if (train->canceled || train->delay > 0) state_color = PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack);
  if (s_stale) state_color = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
  graphics_context_set_text_color(ctx, state_color);
  if (train->canceled) snprintf(line, sizeof(line), "CANCELED");
  else if (remaining < 0) snprintf(line, sizeof(line), "Departed");
  else if (remaining < 60) snprintf(line, sizeof(line), "%lds", (long)remaining);
  else snprintf(line, sizeof(line), "%ld min", (long)((remaining + 59) / 60));
  if (large) {
    draw_text(ctx, train->canceled ? "JOURNEY" : "DEPARTS IN", FONT_KEY_GOTHIC_14_BOLD,
              GRect(4, 55, width - 8, 18), GTextAlignmentCenter);
    if (train->canceled || remaining < 0) {
      draw_text(ctx, line, FONT_KEY_GOTHIC_28_BOLD, GRect(4, 82, width - 8, 42), GTextAlignmentCenter);
    } else {
      snprintf(line, sizeof(line), "%ld", (long)(remaining < 60 ? remaining : (remaining + 59) / 60));
      draw_text(ctx, line, FONT_KEY_BITHAM_42_BOLD, GRect(4, 70, width - 8, 52), GTextAlignmentCenter);
      draw_text(ctx, remaining < 60 ? "seconds" : "minutes", FONT_KEY_GOTHIC_18,
                GRect(4, 117, width - 8, 23), GTextAlignmentCenter);
    }
  } else {
    draw_text(ctx, line, FONT_KEY_GOTHIC_28_BOLD, GRect(2, 43, width - 4, 40), GTextAlignmentCenter);
  }
  graphics_context_set_text_color(ctx, GColorBlack);

  char departure[8], arrival[8];
  clock_text(train->departure, departure, sizeof(departure));
  clock_text(train->arrival, arrival, sizeof(arrival));
  snprintf(line, sizeof(line), "%s > %s", departure, arrival);
  draw_text(ctx, line, FONT_KEY_GOTHIC_24_BOLD, GRect(2, large ? 137 : 80, width - 4, 30), GTextAlignmentCenter);
  if (train->canceled) snprintf(line, sizeof(line), "Choose another journey");
  else if (train->delay > 0) snprintf(line, sizeof(line), "Platform %s  +%ld min", train->platform, (long)((train->delay + 59) / 60));
  else snprintf(line, sizeof(line), "Platform %s  On time", train->platform);
  graphics_context_set_text_color(ctx, state_color);
  draw_text(ctx, line, large ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD,
            GRect(2, large ? 168 : 111, width - 4, large ? 23 : 19), GTextAlignmentCenter);
  graphics_context_set_text_color(ctx, GColorBlack);
  snprintf(line, sizeof(line), "%s / %u change%s", train->train, train->transfers, train->transfers == 1 ? "" : "s");
  draw_text(ctx, line, FONT_KEY_GOTHIC_14, GRect(2, large ? 190 : 130, width - 4, 18), GTextAlignmentCenter);
  graphics_draw_line(ctx, GPoint(0, footer_y), GPoint(width, footer_y));
  char updated[8];
  clock_text(s_board.updated, updated, sizeof(updated));
  snprintf(line, sizeof(line), "%u/%u  %s%s", s_selected + 1, s_board.count,
           s_loading ? "Updating" : s_stale ? "Cached " : "Updated ", s_loading ? "" : updated);
  draw_text(ctx, line, FONT_KEY_GOTHIC_14, GRect(2, footer_y, width - 4, 18), GTextAlignmentCenter);
}

static void redraw(void) { if (s_layer) layer_mark_dirty(s_layer); }

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
  if (kind == 0) {
    copy_text(iter, MESSAGE_KEY_Status, s_status, sizeof(s_status));
    s_loading = strcmp(s_status, "Updating...") == 0 || strcmp(s_status, "Already updating") == 0;
    if (s_loading) s_requested = time(NULL);
    else s_stale = true;
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
    s_received |= 1 << index;
  } else if (kind == 3 && s_loading && number(iter, MESSAGE_KEY_Token) == s_token &&
             s_received == (1 << s_pending.count) - 1) {
    s_board = s_pending;
    memcpy(s_trains, s_rows, sizeof(s_trains));
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
  snprintf(s_status, sizeof(s_status), "Transfer failed\nSELECT to retry");
  redraw();
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *context) { failed(reason, context); }
static void select_click(ClickRecognizerRef recognizer, void *context) { request(1); }
static void reverse_click(ClickRecognizerRef recognizer, void *context) { request(2); }
static void up_click(ClickRecognizerRef recognizer, void *context) {
  if (s_board.count) s_selected = (s_selected + s_board.count - 1) % s_board.count;
  redraw();
}
static void down_click(ClickRecognizerRef recognizer, void *context) {
  if (s_board.count) s_selected = (s_selected + 1) % s_board.count;
  redraw();
}

static void clicks(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 700, reverse_click, NULL);
}

static void tick(struct tm *tick_time, TimeUnits units) {
  time_t now = time(NULL);
  if (s_loading && now - s_requested >= 60) failed(APP_MSG_SEND_TIMEOUT, NULL);
  if (s_board.updated && now - s_board.updated > 120) s_stale = true;
  if (s_ready && !s_loading && tick_time->tm_sec == 0) request(1);
  redraw();
}

static void connection_changed(bool connected) {
  if (!connected) {
    s_stale = true;
    s_loading = false;
    snprintf(s_status, sizeof(s_status), "Phone disconnected");
  } else if (s_ready) request(1);
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
      break;
    }
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
  app_message_open(1024, 128);
  connection_service_subscribe((ConnectionHandlers) { .pebble_app_connection_handler = connection_changed });
  tick_timer_service_subscribe(SECOND_UNIT, tick);
  window_stack_push(s_window, true);
  app_event_loop();
  tick_timer_service_unsubscribe();
  connection_service_unsubscribe();
  layer_destroy(s_layer);
  window_destroy(s_window);
}
