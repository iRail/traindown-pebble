#pragma once
#include <pebble.h>

// Trip overview of one journey: every train with platforms, the stops in between and, for
// each change, how long there is to make it.
//
// The watch keeps only the board's headline numbers. The trains and stops of the journey
// being viewed are requested from the phone (Command 4) and arrive as Kind 4..8 messages.

// Opens the overview for journey `journey_index` of the board with `token`.
void detail_push(uint8_t journey_index, int32_t token);

// Handles Kind 4..8 messages; ignores those not meant for the open overview.
void detail_handle_message(DictionaryIterator *iter, int kind);

// True while the overview is on screen.
bool detail_is_open(void);

// Reports a failed outgoing message to the overview. Returns true if it was the overview's.
bool detail_outbox_failed(void);
