#pragma once

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "MethodProtocol.h"

// Reading and writing the timers of a book (SPEC-DEVICES section 3). Kept apart from MethodProtocol.h so that
// header stays free of ArduinoJson. Used by the /books parser and by the library index, which must agree on what a
// valid set of timers is.
namespace method {

// True when `value` is a valid "timers" array for a book of `pageCount` pages, exactly as the Worker validates it:
// at most MAX_TIMERS objects with only the keys page, seconds, label and box; page 1 to pageCount and at most one
// timer per page; seconds 1 to MAX_TIMER_SECONDS; label of 1 to 40 characters when present; box of four whole
// numbers [x, y, w, h] with w and h at least 24, inside 480 x 800; and no more than MAX_TIMERS_JSON_BYTES of text.
// A label is well-formed UTF-8 with no control or invisible format characters, as the Worker requires. All or nothing:
// on false `out` is empty. An empty array is valid (no timers).
bool parseTimerArray(JsonVariantConst value, unsigned pageCount, std::vector<Timer>& out);

// The number of characters in `text` when it is well-formed UTF-8 with none of the characters the Worker refuses in
// text a person typed (control characters, invisible format characters, line and paragraph separators); -1 otherwise.
// A NUL inside the text is refused. The Register schedule's titles are held to the same rule.
int printableCharCount(const char* text, size_t size);

// Appends the timers as a JSON array in the Worker's own shape, in the order given:
// [{"page":4,"seconds":300,"label":"Incubate","box":[32,610,416,96]}]. The label key is left out when empty.
void appendTimersJson(std::string& out, const std::vector<Timer>& timers);

}  // namespace method
