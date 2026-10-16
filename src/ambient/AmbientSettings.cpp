#include "AmbientSettings.h"

#include <PlaqueBoard.h>

#include <cstdio>
#include <cstring>

#include "MessageProtocol.h"
#include "RegisterChoice.h"

namespace ambient {
bool AmbientSettings::configured() const {
  return validOrigin(baseUrl, allowLocalHttp) && validIdentifier(deviceId, 32) && validIdentifier(deviceToken, 128) &&
         strlen(deviceToken) >= 32;
}

uint64_t AmbientSettings::wakeTimerMicros() const {
  return plaque::board().caps.scheduledDownloads && useAsSleepScreen && scheduledPolling && configured()
             ? static_cast<uint64_t>(pollMinutes) * 60 * 1000000
             : 0;
}

// Register is a per-reader choice that only counts on a board that offers it (plaque::offersRegister: the sleep screen,
// timed wake and the mailbox, the same boards as Notes). A choice left in the settings file of a board that does not is
// kept but never acted on.
bool AmbientSettings::registerSelected() const {
  return plaque::offersRegister(plaque::board().caps) && useRegisterScreen;
}

bool AmbientSettings::pollingEnabled() const {
  return plaque::board().caps.scheduledDownloads && scheduledPolling && configured();
}

void AmbientSettings::toJson(JsonDocument& doc) const {
  doc["base_url"] = baseUrl;
  doc["device_id"] = deviceId;
  doc["device_token"] = deviceToken;
  doc["poll_minutes"] = pollMinutes;
  doc["use_as_sleep_screen"] = useAsSleepScreen;
  doc["scheduled_polling"] = scheduledPolling;
  doc["allow_local_http"] = allowLocalHttp;
  doc["use_register_screen"] = useRegisterScreen;
}

bool AmbientSettings::fromJson(JsonVariantConst doc) {
  if (!doc.is<JsonObjectConst>()) return false;
  const char* url = doc["base_url"] | "";
  const char* id = doc["device_id"] | "";
  const char* token = doc["device_token"] | "";
  if (strlen(url) >= sizeof(baseUrl) || strlen(id) >= sizeof(deviceId) || strlen(token) >= sizeof(deviceToken))
    return false;
  snprintf(baseUrl, sizeof(baseUrl), "%s", url);
  snprintf(deviceId, sizeof(deviceId), "%s", id);
  snprintf(deviceToken, sizeof(deviceToken), "%s", token);
  const int minutes = doc["poll_minutes"] | 60;
  pollMinutes = minutes >= 5 && minutes <= 1440 ? minutes : 60;
  useAsSleepScreen = doc["use_as_sleep_screen"] | false;
  scheduledPolling = doc["scheduled_polling"] | false;
  allowLocalHttp = doc["allow_local_http"] | false;
  useRegisterScreen = doc["use_register_screen"] | false;
  // One sleep screen at a time, however the file came to say otherwise: Register is kept.
  useAsSleepScreen = reg::normalised({useAsSleepScreen, useRegisterScreen, scheduledPolling, pollMinutes}).message;
  return true;
}

void MessageState::toJson(JsonDocument& doc) const {
  doc["last_message_id"] = lastMessageId;
  doc["sha256"] = sha256;
  doc["slot"] = slot;
}

bool MessageState::fromJson(JsonVariantConst doc) {
  const char* id = doc["last_message_id"] | "";
  const char* hash = doc["sha256"] | "";
  const int savedSlot = doc["slot"] | -1;
  if (!validIdentifier(id, 64) || strlen(hash) != 64 || savedSlot < 0 || savedSlot > 1) return false;
  snprintf(lastMessageId, sizeof(lastMessageId), "%s", id);
  snprintf(sha256, sizeof(sha256), "%s", hash);
  slot = savedSlot;
  return true;
}
}  // namespace ambient
