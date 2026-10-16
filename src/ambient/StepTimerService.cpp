#include "StepTimerService.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sys/time.h>

#include "StepTimerStore.h"

namespace steptimer {

namespace {
constexpr char NVS_NAMESPACE[] = "cpstep";
constexpr char NVS_KEY[] = "rec";
}  // namespace

Service& Service::instance() {
  static Service service;
  return service;
}

Now readClocks() {
  Now now;
  now.monoUs = esp_timer_get_time();
  timeval tv{};
  gettimeofday(&tv, nullptr);
  now.wallMs = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
  return now;
}

bool powerWasLost() {
  const esp_reset_reason_t reason = esp_reset_reason();
  return reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT;
}

Now Service::now() const { return readClocks(); }

int64_t Service::remainingMs() const { return clock_.remainingMs(esp_timer_get_time()); }

void Service::load() {
  record_ = Record();
  Preferences prefs;
  // Opened read-write so that a reader that has never run a timer does not log a missing namespace at every boot.
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERR("TIMER", "Could not open the timer store");
    return;
  }
  const String text = prefs.getString(NVS_KEY, "");
  prefs.end();
  record_ = recordFromJson(std::string_view(text.c_str(), text.length()));
  if (text.length() > 0 && record_.phase == Phase::None)
    LOG_ERR("TIMER", "The saved timer was not one this reader wrote; dropped");
}

void Service::save() const {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERR("TIMER", "Could not open the timer store to save");
    return;
  }
  const std::string text = recordToJson(record_);
  bool ok = true;
  if (text.empty()) {
    prefs.remove(NVS_KEY);
  } else {
    ok = prefs.putString(NVS_KEY, text.c_str()) > 0;
  }
  prefs.end();
  if (!ok) LOG_ERR("TIMER", "Could not save the timer");
}

BootAction Service::boot(const Now& bootClocks, bool timerWake) {
  load();
  const bool lost = powerWasLost();
  const BootFacts facts = clock_.boot(bootClocks, lost, timerWake);
  lostPower_ = facts.powerLost;
  const BootAction action = planBoot(facts);
  if (facts.phase != Phase::None) {
    LOG_INF("TIMER", "Boot: phase=%u remaining=%lld ms powerLost=%d timerWake=%d -> action=%u",
            static_cast<unsigned>(facts.phase), static_cast<long long>(facts.remainingMs), facts.powerLost, timerWake,
            static_cast<unsigned>(action));
  }
  return action;
}

void Service::start(const std::string& bookPath, unsigned page, uint32_t seconds, const std::string& label,
                    const Rect& box, unsigned panelPage) {
  done_ = DoneNote();  // a new timer; whatever the last one left behind is out of date
  clock_.start(bookPath, page, seconds, label, box, readClocks(), panelPage);
  save();
  LOG_INF("TIMER", "Started %lu s on page %u of %s", static_cast<unsigned long>(seconds), page, bookPath.c_str());
}

void Service::clear() {
  const bool had = record_.phase != Phase::None;
  clock_.clear();
  if (had) {
    save();
    LOG_INF("TIMER", "Cleared");
  }
}

void Service::markAlerted() {
  clock_.markAlerted();
  save();
  LOG_INF("TIMER", "Alerted");
}

void Service::persist(int panelPage) {
  if (!clock_.running()) return;
  const int64_t oldEndMs = record_.endWallMs;
  const uint16_t oldPanelPage = record_.panelPage;
  clock_.beforeSleep(readClocks(), panelPage >= 0 ? static_cast<unsigned>(panelPage) : oldPanelPage);
  // An ordinary poll finds the end where it left it, give or take the milliseconds between two clock reads; only a
  // clock that was stepped (or a new page on the panel) is worth a flash write. Keeping the stored end also stops the
  // jitter adding up.
  const int64_t moved = record_.endWallMs > oldEndMs ? record_.endWallMs - oldEndMs : oldEndMs - record_.endWallMs;
  if (moved < 500 && record_.panelPage == oldPanelPage) {
    record_.endWallMs = oldEndMs;
    return;
  }
  save();
}

uint64_t Service::rearm(uint64_t pollMicros, int panelPage) {
  persist(panelPage);
  const int64_t monoUs = esp_timer_get_time();
  const uint64_t wake = clock_.wakeMicros(pollMicros, monoUs);
  if (clock_.running())
    LOG_INF("TIMER", "Sleeping with %lld ms left; wake in %llu ms (poll %llu ms)",
            static_cast<long long>(clock_.remainingMs(monoUs)), static_cast<unsigned long long>(wake / 1000),
            static_cast<unsigned long long>(pollMicros / 1000));
  return wake;
}

}  // namespace steptimer
