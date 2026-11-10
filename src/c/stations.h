#pragma once
#include <pebble.h>

#define STATION_NAME_MAX 44

typedef void (*StationPickedHandler)(const char *name, void *context);

// Pushes the drill-down station picker. `current` (may be NULL) is listed first
// under "Recent". `handler` is called once with the chosen name before the
// picker closes; Back steps up one level and closes the picker from the top.
void station_picker_push(const char *title, const char *current, StationPickedHandler handler, void *context);
