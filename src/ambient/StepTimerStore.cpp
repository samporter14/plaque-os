#include "StepTimerStore.h"

#include <ArduinoJson.h>

#include <cstdlib>

namespace steptimer {

std::string recordToJson(const Record& record) {
  if (record.phase == Phase::None) return {};
  JsonDocument doc;
  doc["v"] = 1;
  doc["phase"] = static_cast<uint8_t>(record.phase);
  doc["book"] = record.bookPath;
  doc["page"] = record.page;
  doc["seconds"] = record.seconds;
  doc["label"] = record.label;
  JsonArray box = doc["box"].to<JsonArray>();
  box.add(record.boxX);
  box.add(record.boxY);
  box.add(record.boxW);
  box.add(record.boxH);
  // A 64-bit number goes through the text as a string: ArduinoJson is not built for 64-bit integers on every board.
  doc["end"] = std::to_string(static_cast<long long>(record.endWallMs));
  doc["panel"] = record.panelPage;
  std::string out;
  serializeJson(doc, out);
  return out;
}

Record recordFromJson(std::string_view json) {
  Record record;
  if (json.empty() || json.size() > MAX_RECORD_JSON_BYTES) return record;
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(3)) || !doc.is<JsonObject>())
    return record;
  const unsigned phase = doc["phase"] | 0u;
  if (phase != static_cast<unsigned>(Phase::Running) && phase != static_cast<unsigned>(Phase::Alerted)) return record;
  record.phase = static_cast<Phase>(phase);
  record.bookPath = doc["book"] | "";
  record.page = doc["page"] | static_cast<uint16_t>(0);
  record.seconds = doc["seconds"] | 0u;
  record.label = doc["label"] | "";
  const JsonArrayConst box = doc["box"];
  if (box.size() == 4) {
    record.boxX = box[0] | static_cast<uint16_t>(0);
    record.boxY = box[1] | static_cast<uint16_t>(0);
    record.boxW = box[2] | static_cast<uint16_t>(0);
    record.boxH = box[3] | static_cast<uint16_t>(0);
  }
  const char* end = doc["end"] | "0";
  record.endWallMs = static_cast<int64_t>(strtoll(end, nullptr, 10));
  record.panelPage = doc["panel"] | static_cast<uint16_t>(0);
  return recordValid(record) ? record : Record();
}

}  // namespace steptimer
