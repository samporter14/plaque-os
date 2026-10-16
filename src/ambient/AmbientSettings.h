#pragma once

#include <PersistableStore.h>

namespace ambient {
class AmbientSettings : public PersistableStore<AmbientSettings> {
  AmbientSettings() = default;
  friend class PersistableStore<AmbientSettings>;

 public:
  char baseUrl[192] = {};
  char deviceId[33] = {};
  char deviceToken[129] = {};
  uint16_t pollMinutes = 60;
  bool useAsSleepScreen = false;
  bool scheduledPolling = false;
  bool allowLocalHttp = false;
  // The Register screen (an instrument's schedule) as the sleep screen. A per-reader choice, written only by the
  // Messages settings screen: the mailbox gives a registered reader no notes, weather or agenda.
  bool useRegisterScreen = false;

  bool configured() const;
  uint64_t wakeTimerMicros() const;
  // The Register screen is the sleep screen on this board.
  bool registerSelected() const;
  // Scheduled polling is on and the mailbox is set up: the Register screen may ask for its schedule while asleep.
  bool pollingEnabled() const;
  static const char* getFilePath() { return "/.crosspoint/ambient-config.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
};

class MessageState : public PersistableStore<MessageState> {
  MessageState() = default;
  friend class PersistableStore<MessageState>;

 public:
  char lastMessageId[65] = {};
  char sha256[65] = {};
  uint8_t slot = 0;

  static const char* getFilePath() { return "/.crosspoint/ambient-state.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
};
}  // namespace ambient

#define AMBIENT_SETTINGS ambient::AmbientSettings::getInstance()
#define MESSAGE_STATE ambient::MessageState::getInstance()
