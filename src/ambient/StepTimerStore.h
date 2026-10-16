#pragma once

#include <string>
#include <string_view>

#include "StepTimer.h"

// How a step timer's record is written down (the text kept in NVS by StepTimerService). Pure, so the host tests in
// test/step_timer cover the round trip and what a damaged record reads as.
namespace steptimer {

// Longest text recordToJson can produce for a valid record: the book path, the label with every character escaped,
// and the fixed keys. NVS strings are good for far more.
constexpr size_t MAX_RECORD_JSON_BYTES = 1024;

// A record with phase None is written as an empty string.
std::string recordToJson(const Record& record);

// Reads a record back. Anything missing, partial or not one this reader could have written reads as no timer at all.
Record recordFromJson(std::string_view json);

}  // namespace steptimer
