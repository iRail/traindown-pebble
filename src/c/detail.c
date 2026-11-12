#include "detail.h"
#include "stations.h"

#define COMMAND_DETAILS 4
#define MAX_LEGS 6    // keep in sync with src/pkjs/transport.js
#define MAX_STOPS 40
#define LOAD_TIMEOUT_MS 20000

typedef struct {
  uint32_t time;
  char name[28];
} Stop;

typedef struct {
  uint32_t departure, arrival;
  int32_t delay;
  uint8_t stop_count;
  char from[STATION_NAME_MAX], to[STATION_NAME_MAX], train[24], platform[12], to_platform[12];
  Stop stops[MAX_STOPS];
} Leg;

typedef struct {
  Window *window;
  ScrollLayer *scroll;
  Layer *content;
  TextLayer *message_layer;
  AppTimer *timer;
  int32_t token;
  uint8_t index, leg_count;
  bool loaded, large;
  char message[64];
  Leg legs[MAX_LEGS];
} Detail;

static Detail *s_detail;
static int16_t s_pan_base;  // content offset when the finger went down

typedef enum { ItemBoard, ItemTransfer, ItemStop, ItemArrive } ItemKind;

// ---- layout ----------------------------------------------------------------------------

typedef struct {
  int rail_x, text_x, text_w, line1, line, stop, gap, dot, small_dot, width;
  const char *font1, *font, *font_stop;
} Metrics;

static Metrics metrics(const Detail *d, int width) {
  if (d->large) {
    return (Metrics) { 14, 32, width - 32 - 6, 28, 22, 24, 8, 7, 3, 4, FONT_KEY_GOTHIC_24_BOLD, FONT_KEY_GOTHIC_18,
                       FONT_KEY_GOTHIC_18 };
  }
  return (Metrics) { 9, 22, width - 22 - 3, 22, 18, 19, 6, 5, 2, 3, FONT_KEY_GOTHIC_18_BOLD, FONT_KEY_GOTHIC_14,
                     FONT_KEY_GOTHIC_14 };
}

static int item_height(const Metrics *m, ItemKind kind) {
  switch (kind) {
    case ItemBoard: return m->line1 + 2 * m->line + m->gap;
    case ItemTransfer: return m->line1 + 3 * m->line + m->gap;
    case ItemArrive: return m->line1 + m->line + m->gap;
    default: return m->stop;
  }
}

static void clock_text(uint32_t timestamp, char *buffer, size_t size) {
  time_t value = timestamp;
  strftime(buffer, size, "%H:%M", localtime(&value));
}

static void text(GContext *ctx, const char *string, const char *font, int x, int y, int w, int h, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, string, fonts_get_system_font(font), GRect(x, y, w, h), GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
}

// Draws one entry of the trip (when `ctx` is set and the entry is on screen) and returns its height.
static int item(const Detail *d, const Metrics *m, GContext *ctx, ItemKind kind, int leg_index, int stop_index, int y,
                int visible_top, int visible_bottom) {
  const int height = item_height(m, kind);
  if (!ctx || y + height < visible_top || y > visible_bottom) return height;

  const GColor rail_color = PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack);
  const GColor muted = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
  const GColor late = PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack);
  const Leg *leg = &d->legs[leg_index];
  const Leg *previous = leg_index > 0 ? &d->legs[leg_index - 1] : NULL;
  const int centre = kind == ItemStop ? y + height / 2 : y + m->line1 / 2 + 1;
  char line[64], clock[8], other[8];

  // The rail runs through every entry; its ends sit on the first and last station.
  const int rail_top = kind == ItemBoard ? centre : y;
  const int rail_bottom = kind == ItemArrive ? centre : y + height;
  graphics_context_set_stroke_color(ctx, rail_color);
  graphics_context_set_stroke_width(ctx, m->width);
  graphics_draw_line(ctx, GPoint(m->rail_x, rail_top), GPoint(m->rail_x, rail_bottom));
  graphics_context_set_fill_color(ctx, rail_color);
  if (kind == ItemStop) {
    graphics_fill_circle(ctx, GPoint(m->rail_x, centre), m->small_dot);
  } else if (kind == ItemTransfer) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, GPoint(m->rail_x, centre), m->dot);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_circle(ctx, GPoint(m->rail_x, centre), m->dot);
  } else {
    graphics_fill_circle(ctx, GPoint(m->rail_x, centre), m->dot);
  }

  if (kind == ItemStop) {
    const Stop *stop = &leg->stops[stop_index];
    clock_text(stop->time, clock, sizeof(clock));
    snprintf(line, sizeof(line), "%s  %s", clock, stop->name);
    text(ctx, line, m->font_stop, m->text_x, y + (d->large ? 0 : -1), m->text_w, height + 4, muted);
    return height;
  }

  const char *station = kind == ItemBoard ? leg->from : kind == ItemTransfer ? previous->to : leg->to;
  GSize size = graphics_text_layout_get_content_size(station, fonts_get_system_font(m->font1), GRect(0, 0, 500, 60),
                                                     GTextOverflowModeFill, GTextAlignmentLeft);
  text(ctx, station, size.w > m->text_w ? (d->large ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD) : m->font1,
       m->text_x, y - (d->large ? 4 : 3), m->text_w, m->line1 + 6, GColorBlack);
  int line_y = y + m->line1;
  if (kind == ItemBoard) {
    // Planned time first (the train you are taking), then how late it is.
    clock_text(leg->departure - leg->delay, clock, sizeof(clock));
    if (leg->delay > 0) snprintf(line, sizeof(line), "%s (+%ld min)  Pl %s", clock, (long)((leg->delay + 59) / 60), leg->platform);
    else snprintf(line, sizeof(line), "%s  Pl %s", clock, leg->platform);
    text(ctx, line, m->font, m->text_x, line_y, m->text_w, m->line + 4, leg->delay > 0 ? late : GColorBlack);
    text(ctx, leg->train, m->font, m->text_x, line_y + m->line, m->text_w, m->line + 4, muted);
  } else if (kind == ItemTransfer) {
    const int32_t wait = ((int32_t)leg->departure - (int32_t)previous->arrival) / 60;
    clock_text(previous->arrival, clock, sizeof(clock));
    snprintf(line, sizeof(line), "Arrive %s  Pl %s", clock, previous->to_platform);
    text(ctx, line, m->font, m->text_x, line_y, m->text_w, m->line + 4, GColorBlack);
    if (wait < 0) snprintf(line, sizeof(line), "Missed connection");
    else snprintf(line, sizeof(line), "Change in %ld min", (long)wait);
    // A short change is the thing to watch out for.
    text(ctx, line, d->large ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_14_BOLD, m->text_x, line_y + m->line,
         m->text_w, m->line + 4, wait < 5 ? late : GColorBlack);
    clock_text(leg->departure, other, sizeof(other));
    snprintf(line, sizeof(line), "%s  %s  Pl %s", leg->train, other, leg->platform);
    text(ctx, line, m->font, m->text_x, line_y + 2 * m->line, m->text_w, m->line + 4, muted);
  } else {
    clock_text(leg->arrival, clock, sizeof(clock));
    snprintf(line, sizeof(line), "Arrive %s  Pl %s", clock, leg->to_platform);
    text(ctx, line, m->font, m->text_x, line_y, m->text_w, m->line + 4, GColorBlack);
  }
  return height;
}

// Lays out the whole trip top to bottom and returns its height; draws what is visible if `ctx` is set.
static int walk(const Detail *d, GContext *ctx, int width, int visible_top, int visible_bottom) {
  const Metrics m = metrics(d, width);
  int y = 6;
  for (int i = 0; i < d->leg_count; i++) {
    y += item(d, &m, ctx, i == 0 ? ItemBoard : ItemTransfer, i, 0, y, visible_top, visible_bottom);
    for (int s = 0; s < d->legs[i].stop_count; s++) y += item(d, &m, ctx, ItemStop, i, s, y, visible_top, visible_bottom);
  }
  if (d->leg_count) y += item(d, &m, ctx, ItemArrive, d->leg_count - 1, 0, y, visible_top, visible_bottom);
  return y + 6;
}

static void content_update(Layer *layer, GContext *ctx) {
  if (!s_detail || !s_detail->loaded) return;
  const int width = layer_get_bounds(layer).size.w;
  const int top = -scroll_layer_get_content_offset(s_detail->scroll).y;
  walk(s_detail, ctx, width, top - 40, top + layer_get_bounds(window_get_root_layer(s_detail->window)).size.h + 40);
}

// ---- state ------------------------------------------------------------------------------

static void show_message(const char *message) {
  if (!s_detail) return;
  snprintf(s_detail->message, sizeof(s_detail->message), "%s", message);
  s_detail->loaded = false;
  text_layer_set_text(s_detail->message_layer, s_detail->message);
  layer_set_hidden(text_layer_get_layer(s_detail->message_layer), false);
  layer_set_hidden(s_detail->content, true);
}

static void show_trip(void) {
  Detail *d = s_detail;
  d->loaded = true;
  const int width = layer_get_bounds(scroll_layer_get_layer(d->scroll)).size.w;
  const int height = walk(d, NULL, width, 0, 0);
  layer_set_frame(d->content, GRect(0, 0, width, height));
  scroll_layer_set_content_size(d->scroll, GSize(width, height));
  scroll_layer_set_content_offset(d->scroll, GPointZero, false);
  layer_set_hidden(text_layer_get_layer(d->message_layer), true);
  layer_set_hidden(d->content, false);
  layer_mark_dirty(d->content);
}

static void timed_out(void *context) {
  if (!s_detail) return;
  s_detail->timer = NULL;
  if (!s_detail->loaded) show_message("Could not load the trip\nPress Back and retry");
}

static void copy_text(DictionaryIterator *iter, uint32_t key, char *buffer, size_t size) {
  Tuple *tuple = dict_find(iter, key);
  if (tuple && tuple->type == TUPLE_CSTRING) snprintf(buffer, size, "%s", tuple->value->cstring);
}

static int32_t number(DictionaryIterator *iter, uint32_t key) {
  Tuple *tuple = dict_find(iter, key);
  return tuple ? tuple->value->int32 : 0;
}

// "<epoch> <name>" lines, in the order of the stops.
static void append_stops(Leg *leg, const char *lines) {
  const char *p = lines;
  while (*p && leg->stop_count < MAX_STOPS) {
    char *end;
    long time = strtol(p, &end, 10);
    if (end == p) break;
    while (*end == ' ') end++;
    const char *newline = strchr(end, '\n');
    size_t length = newline ? (size_t)(newline - end) : strlen(end);
    Stop *stop = &leg->stops[leg->stop_count++];
    stop->time = time;
    if (length >= sizeof(stop->name)) length = sizeof(stop->name) - 1;
    memcpy(stop->name, end, length);
    stop->name[length] = 0;
    p = newline ? newline + 1 : end + strlen(end);
  }
}

void detail_handle_message(DictionaryIterator *iter, int kind) {
  Detail *d = s_detail;
  if (!d || number(iter, MESSAGE_KEY_Token) != d->token || number(iter, MESSAGE_KEY_Index) != d->index) return;
  if (kind == 4) {
    memset(d->legs, 0, sizeof(d->legs));
    int count = number(iter, MESSAGE_KEY_Count);
    d->leg_count = count < 0 ? 0 : count > MAX_LEGS ? MAX_LEGS : count;
  } else if (kind == 5) {
    int number_of_leg = number(iter, MESSAGE_KEY_Leg);
    if (number_of_leg < 0 || number_of_leg >= d->leg_count) return;
    Leg *leg = &d->legs[number_of_leg];
    copy_text(iter, MESSAGE_KEY_From, leg->from, sizeof(leg->from));
    copy_text(iter, MESSAGE_KEY_To, leg->to, sizeof(leg->to));
    copy_text(iter, MESSAGE_KEY_Train, leg->train, sizeof(leg->train));
    copy_text(iter, MESSAGE_KEY_Platform, leg->platform, sizeof(leg->platform));
    copy_text(iter, MESSAGE_KEY_ToPlatform, leg->to_platform, sizeof(leg->to_platform));
    leg->departure = number(iter, MESSAGE_KEY_Departure);
    leg->arrival = number(iter, MESSAGE_KEY_Arrival);
    leg->delay = number(iter, MESSAGE_KEY_Delay);
  } else if (kind == 6) {
    int number_of_leg = number(iter, MESSAGE_KEY_Leg);
    Tuple *stops = dict_find(iter, MESSAGE_KEY_Stops);
    if (number_of_leg >= 0 && number_of_leg < d->leg_count && stops && stops->type == TUPLE_CSTRING)
      append_stops(&d->legs[number_of_leg], stops->value->cstring);
  } else if (kind == 7) {
    if (d->leg_count) {
      if (d->timer) { app_timer_cancel(d->timer); d->timer = NULL; }
      show_trip();
    } else show_message("No trip details");
  } else if (kind == 8) {
    char message[64] = "Trip details unavailable";
    copy_text(iter, MESSAGE_KEY_Status, message, sizeof(message));
    show_message(message);
  }
}

bool detail_is_open(void) { return s_detail != NULL; }

bool detail_outbox_failed(void) {
  if (!s_detail || s_detail->loaded) return false;
  show_message("Could not reach the phone\nPress Back and retry");
  return true;
}

static void send_request(void) {
  if (!connection_service_peek_pebble_app_connection()) { show_message("Phone disconnected"); return; }
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK || !iter) { show_message("Phone busy\nPress Back and retry"); return; }
  dict_write_int32(iter, MESSAGE_KEY_Command, COMMAND_DETAILS);
  dict_write_int32(iter, MESSAGE_KEY_Token, s_detail->token);
  dict_write_int32(iter, MESSAGE_KEY_Index, s_detail->index);
  if (app_message_outbox_send() != APP_MSG_OK) show_message("Phone busy\nPress Back and retry");
}

// ---- touch --------------------------------------------------------------------------------

// A bare ScrollLayer does not scroll by touch, so a vertical pan drives it (the SDK's own recipe).
static void pan_handler(const Recognizer *recognizer, RecognizerEvent event) {
  Detail *d = s_detail;
  if (!d || !d->loaded) return;
  if (event == RecognizerEvent_Started) {
    s_pan_base = scroll_layer_get_content_offset(d->scroll).y;
  } else if (event == RecognizerEvent_Updated) {
    int y = s_pan_base + pan_recognizer_get_delta_since_start(recognizer).y;
    const int lowest = layer_get_bounds(window_get_root_layer(d->window)).size.h - layer_get_frame(d->content).size.h;
    if (y < lowest) y = lowest;
    if (y > 0) y = 0;
    scroll_layer_set_content_offset(d->scroll, GPoint(0, y), false);
  } else if (event == RecognizerEvent_Cancelled) {
    scroll_layer_set_content_offset(d->scroll, GPoint(0, s_pan_base), false);
  }
}

// ---- window -------------------------------------------------------------------------------

static void window_load(Window *window) {
  Detail *d = s_detail;
  Layer *root = window_get_root_layer(window);
  const GRect bounds = layer_get_bounds(root);
  d->large = bounds.size.h >= 228;
  d->scroll = scroll_layer_create(bounds);
  scroll_layer_set_shadow_hidden(d->scroll, true);
  d->content = layer_create(GRect(0, 0, bounds.size.w, bounds.size.h));
  layer_set_update_proc(d->content, content_update);
  scroll_layer_add_child(d->scroll, d->content);
  // Up and Down scroll; Back keeps its default (close the overview).
  scroll_layer_set_click_config_onto_window(d->scroll, window);
  // Touch: drag to scroll. The system's touch-to-button bridge is off here so a swipe cannot also
  // press Up/Down.
  window_attach_recognizer(window, pan_recognizer_create(pan_handler, NULL, PanAxis_Vertical));
  window_set_touch_bridge_disabled(window, true);
  layer_add_child(root, scroll_layer_get_layer(d->scroll));
  d->message_layer = text_layer_create(GRect(6, bounds.size.h / 2 - 40, bounds.size.w - 12, 80));
  text_layer_set_font(d->message_layer, fonts_get_system_font(d->large ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(d->message_layer, GTextAlignmentCenter);
  text_layer_set_background_color(d->message_layer, GColorClear);
  layer_add_child(root, text_layer_get_layer(d->message_layer));
  show_message("Loading trip...");
}

static void window_unload(Window *window) {
  Detail *d = s_detail;
  s_detail = NULL;
  if (d->timer) app_timer_cancel(d->timer);
  text_layer_destroy(d->message_layer);
  layer_destroy(d->content);
  scroll_layer_destroy(d->scroll);
  window_destroy(d->window);
  free(d);
}

void detail_push(uint8_t journey_index, int32_t token) {
  if (s_detail) return;
  Detail *d = calloc(1, sizeof(Detail));
  if (!d) return;
  s_detail = d;
  d->token = token;
  d->index = journey_index;
  d->window = window_create();
  window_set_background_color(d->window, GColorWhite);
  window_set_window_handlers(d->window, (WindowHandlers) { .load = window_load, .unload = window_unload });
  window_stack_push(d->window, true);
  d->timer = app_timer_register(LOAD_TIMEOUT_MS, timed_out, NULL);
  send_request();
}
