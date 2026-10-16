#include "RegisterProvider.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <PlaqueBoard.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <esp_attr.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "AmbientSettings.h"
#include "CrossPointSettings.h"
#include "ReaderIdentity.h"
#include "RegisterWake.h"
#include "SleepFrame.h"
#include "activities/RenderLock.h"
#include "fontIds.h"
#include "network/ClockProvenance.h"
#include "network/HttpDownloader.h"
#include "network/MessageTls.h"

namespace ambient {
namespace {
// Kept across deep sleeps (and nothing else): when the next poll is planned for, and what the last draw left on the
// panel. See RegisterProvider.h.
RTC_DATA_ATTR int64_t rtcPollDueAt = 0;
RTC_DATA_ATTR bool rtcArmed = false;
RTC_DATA_ATTR uint8_t rtcCardRetries = 0;
RTC_DATA_ATTR uint32_t rtcPanelSig = 0;
RTC_DATA_ATTR int64_t rtcPanelAsOf = 0;

struct Face {
  int fontId;
  EpdFontFamily::Style style;
  int8_t tracking;
};

// The reader's built-in fonts, one per role: NotoSerif 18 bold is the largest serif the reader has, NotoSans 14 reads
// at arm's length, the footer and notes use the reader's own UI face, and the small caps labels the small face with a
// little letter-spacing. test/register_layout measures the same faces from the font tables.
constexpr int8_t OVERLINE_TRACKING = 2;
Face faceFor(const reg::FontRole role) {
  switch (role) {
    case reg::FontRole::Name:
    case reg::FontRole::State:
      return {NOTOSERIF_18_FONT_ID, EpdFontFamily::BOLD, 0};
    case reg::FontRole::Detail:
    case reg::FontRole::Row:
      return {NOTOSANS_14_FONT_ID, EpdFontFamily::REGULAR, 0};
    case reg::FontRole::RowBold:
      return {NOTOSANS_14_FONT_ID, EpdFontFamily::BOLD, 0};
    case reg::FontRole::Note:
      return {UI_10_FONT_ID, EpdFontFamily::REGULAR, 0};
    case reg::FontRole::NoteBold:
      return {UI_10_FONT_ID, EpdFontFamily::BOLD, 0};
    case reg::FontRole::Overline:
      return {SMALL_FONT_ID, EpdFontFamily::REGULAR, OVERLINE_TRACKING};
  }
  return {NOTOSANS_14_FONT_ID, EpdFontFamily::REGULAR, 0};
}

// Wi-Fi and the clock are the caller's; this is the request, as Messages makes it.
constexpr char LOG_TAG[] = "REG";
}  // namespace

RegisterProvider& RegisterProvider::instance() {
  static RegisterProvider provider;
  return provider;
}

// ---- words and options
// ------------------------------------------------------------------------------------------------

reg::Words registerWords() {
  reg::Words w;
  w.free = tr(STR_REGISTER_FREE);
  w.freeMaybe = tr(STR_REGISTER_FREE_MAYBE);
  w.inUse = tr(STR_REGISTER_IN_USE);
  w.notKnown = tr(STR_REGISTER_NOT_KNOWN);
  w.separator = tr(STR_REGISTER_SEPARATOR);
  w.until = tr(STR_REGISTER_UNTIL);
  w.allDayTail = tr(STR_REGISTER_ALL_DAY_TAIL);
  w.waiting = tr(STR_REGISTER_WAITING);
  w.noClock = tr(STR_REGISTER_NO_CLOCK);
  w.allDay = tr(STR_REGISTER_ALL_DAY);
  w.allDayOn = tr(STR_REGISTER_ALL_DAY_ON);
  w.untilTime = tr(STR_REGISTER_UNTIL_TIME);
  w.fromTime = tr(STR_REGISTER_FROM_TIME);
  w.midnight = tr(STR_REGISTER_MIDNIGHT);
  w.endOfDay = tr(STR_REGISTER_END_OF_DAY);
  w.tomorrow = tr(STR_REGISTER_TOMORROW_WORD);
  w.yesterday = tr(STR_REGISTER_YESTERDAY_WORD);
  w.dayTime = tr(STR_REGISTER_DAY_TIME);
  w.nothingToday = tr(STR_REGISTER_NOTHING_TODAY);
  w.nothingMoreToday = tr(STR_REGISTER_NOTHING_MORE_TODAY);
  w.nothingTomorrow = tr(STR_REGISTER_NOTHING_TOMORROW);
  w.more = tr(STR_REGISTER_MORE);
  w.asOf = tr(STR_REGISTER_AS_OF);
  w.unavailableSince = tr(STR_REGISTER_UNAVAILABLE_SINCE);
  w.notRead = tr(STR_REGISTER_NOT_READ);
  w.batteryLow = tr(STR_REGISTER_BATTERY_LOW);
  w.heading = tr(STR_REGISTER_HEADING);
  w.next = tr(STR_REGISTER_NEXT);
  w.today = tr(STR_REGISTER_TODAY);
  w.tomorrowHeading = tr(STR_REGISTER_TOMORROW);
  w.pageOf = tr(STR_REGISTER_PAGE_OF);
  // A board without a touchscreen says the key and not the tap (plaque::promptsTouch); the X4 Pro's words are
  // unchanged.
  const bool touch = plaque::promptsTouch(plaque::board().caps);
  w.hintNext = touch ? tr(STR_REGISTER_HINT_NEXT) : tr(STR_REGISTER_HINT_NEXT_KEYS);
  w.hintNextLast = touch ? tr(STR_REGISTER_HINT_NEXT_LAST) : tr(STR_REGISTER_HINT_NEXT_LAST_KEYS);
  w.hintPrev = tr(STR_REGISTER_HINT_PREV);
  w.hintPrevFirst = tr(STR_REGISTER_HINT_PREV_FIRST);
  const StrId days[7] = {StrId::STR_METHOD_SUN, StrId::STR_METHOD_MON, StrId::STR_METHOD_TUE, StrId::STR_METHOD_WED,
                         StrId::STR_METHOD_THU, StrId::STR_METHOD_FRI, StrId::STR_METHOD_SAT};
  for (int i = 0; i < 7; ++i) w.weekdays[i] = I18N.get(days[i]);
  const StrId months[12] = {StrId::STR_METHOD_JAN, StrId::STR_METHOD_FEB, StrId::STR_METHOD_MAR, StrId::STR_METHOD_APR,
                            StrId::STR_METHOD_MAY, StrId::STR_METHOD_JUN, StrId::STR_METHOD_JUL, StrId::STR_METHOD_AUG,
                            StrId::STR_METHOD_SEP, StrId::STR_METHOD_OCT, StrId::STR_METHOD_NOV, StrId::STR_METHOD_DEC};
  for (int i = 0; i < 12; ++i) w.months[i] = I18N.get(months[i]);
  return w;
}

reg::Options registerOptions(const bool batteryLow) {
  reg::Options o;
  o.twelveHour = SETTINGS.clockFormat == 1;
  o.pollMinutes = AMBIENT_SETTINGS.pollMinutes;
  o.batteryLow = batteryLow;
  return o;
}

namespace {
// Whether the retained clock is trusted, read at most every 30 s: it reads the RTC chip over I2C and logs, and a screen
// asks for the time several times in a pass. A sync this boot settles it without asking.
bool retainedClockTrusted() {
  static bool known = false;
  static bool trusted = false;
  static uint32_t at = 0;
  const uint32_t now = static_cast<uint32_t>(millis());
  if (!known || static_cast<uint32_t>(now - at) >= 30000) {
    trusted = clockprov::retained(0).trusted;
    known = true;
    at = now;
  }
  return trusted;
}
}  // namespace

int64_t registerNow() {
  const int64_t now = trustedtime::trustedNow();
  if (now == 0) return 0;
  const bool synced = message_tls::clockSyncedThisBoot();
  return reg::trustedReading(now, synced, !synced && retainedClockTrusted());
}

// ---- the card
// ---------------------------------------------------------------------------------------------------------

namespace {
// The card's files, for reg::loadCard().
class CardFilesOnSd final : public reg::CardFiles {
 public:
  bool exists(const char* path) override { return Storage.exists(path); }
  bool read(const char* path, size_t maxBytes, std::string& out) override {
    return Storage.readFileToString(LOG_TAG, path, maxBytes, out);
  }
  bool rename(const char* tmpPath, const char* path) override { return Storage.replaceFile(tmpPath, path); }
  bool remove(const char* path) override { return Storage.remove(path); }
};
}  // namespace

bool RegisterProvider::loadLocked() {
  loaded_ = true;
  valid_ = false;
  CardFilesOnSd card;
  reg::Schedule parsed;
  const reg::CardLoad how = reg::loadCard(card, parsed);
  if (how == reg::CardLoad::None) {
    if (Storage.exists(reg::CARD_PATH))
      LOG_ERR(LOG_TAG, "%s is not a schedule this reader accepts: ignored", reg::CARD_PATH);
    return false;
  }
  if (how == reg::CardLoad::Recovered)
    LOG_INF(LOG_TAG, "%s was missing: recovered from the temporary file", reg::CARD_PATH);
  schedule_ = std::move(parsed);
  valid_ = true;
  return true;
}

bool RegisterProvider::loadSchedule() {
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_ ? valid_ : loadLocked();
}

bool RegisterProvider::snapshot(reg::Schedule& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_) loadLocked();
  if (!valid_) return false;
  out = schedule_;
  return true;
}

bool RegisterProvider::peek(reg::Schedule& out, int64_t* decidedView) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!loaded_ || !valid_) return false;
  out = schedule_;
  if (decidedView) *decidedView = preparedView_;
  return true;
}

// The same write the reader's identity file gets: a temporary file, flushed, then renamed over the real one.
bool RegisterProvider::saveLocked(const reg::Schedule& schedule) {
  const std::string text = reg::toJson(schedule);
  if (text.empty() || text.size() > reg::MAX_CARD_BYTES) return false;
  if (!Storage.exists(reg::CARD_FOLDER) && !Storage.mkdir(reg::CARD_FOLDER)) return false;
  HalFile file;
  if (!Storage.openFileForWrite(LOG_TAG, reg::CARD_TMP_PATH, file)) return false;
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  file.flush();
  if (!file.close() || written != text.size()) {
    Storage.remove(reg::CARD_TMP_PATH);
    return false;
  }
  if (Storage.replaceFile(reg::CARD_TMP_PATH, reg::CARD_PATH)) return true;
  Storage.remove(reg::CARD_TMP_PATH);
  return false;
}

// ---- the poll
// ---------------------------------------------------------------------------------------------------------

FetchResult RegisterProvider::fetch() {
  const auto& config = AMBIENT_SETTINGS;
  if (!config.configured()) return FetchResult::NotConfigured;
  if (WiFi.status() != WL_CONNECTED) return FetchResult::Failed;
  // The same clock sync as a message poll: verified TLS needs the time.
  if (std::strncmp(config.baseUrl, "https://", 8) == 0 && !message_tls::prepareClock()) return FetchResult::Failed;
  std::vector<HttpDownloader::Header> headers;
  headers.reserve(4);
  headers.emplace_back("Authorization", std::string("Bearer ") + config.deviceToken);
  appendIdentityHeaders(headers);
  const std::string url = std::string(config.baseUrl) + "/api/device/" + config.deviceId + "/register";
  auto body = makeUniqueNoThrow<char[]>(reg::MAX_REPLY_BYTES + 1);
  if (!body) {
    LOG_ERR(LOG_TAG, "OOM: reply buffer");
    return FetchResult::Failed;
  }
  size_t used = 0;
  const auto fetched = HttpDownloader::fetchBounded(
      url,
      [&](const uint8_t* data, size_t len) {
        if (len > reg::MAX_REPLY_BYTES - used) return false;
        memcpy(body.get() + used, data, len);
        used += len;
        return true;
      },
      headers, reg::MAX_REPLY_BYTES);
  if (fetched != HttpDownloader::OK) return FetchResult::Failed;
  reg::Schedule parsed;
  if (!reg::parse(std::string_view(body.get(), used), parsed)) {
    LOG_ERR(LOG_TAG, "The reply is not a schedule this reader accepts: thrown away");
    return FetchResult::Failed;
  }
  body.reset();
  std::lock_guard<std::mutex> lock(mutex_);
  // The card keeps the last good schedule: a reply that cannot be written is still used for this boot, and the next
  // poll writes it again.
  if (!saveLocked(parsed)) LOG_ERR(LOG_TAG, "Could not write %s", reg::CARD_PATH);
  schedule_ = std::move(parsed);
  loaded_ = true;
  valid_ = true;
  LOG_INF(LOG_TAG, "Schedule read: %s, %u today, %u tomorrow", schedule_.instrument.c_str(),
          static_cast<unsigned>(schedule_.today.size()), static_cast<unsigned>(schedule_.tomorrow.size()));
  return FetchResult::Updated;
}

void RegisterProvider::notePolled() {
  const int64_t now = registerNow();
  rtcPollDueAt = reg::pollDueAfter(now, static_cast<int64_t>(AMBIENT_SETTINGS.pollMinutes) * 60);
}

// ---- the wake
// ---------------------------------------------------------------------------------------------------------

namespace {
reg::WakeInputs wakeInputs(const uint16_t battery) {
  reg::WakeInputs in;
  in.now = registerNow();
  in.pollEnabled = AMBIENT_SETTINGS.pollingEnabled();
  in.pollIntervalS = static_cast<int64_t>(AMBIENT_SETTINGS.pollMinutes) * 60;
  in.pollDueAt = rtcPollDueAt;
  in.batteryPercent = battery;
  return in;
}
}  // namespace

bool RegisterProvider::pollWake(const uint16_t battery) {
  return battery >= sleepframe::MIN_POLL_BATTERY_PERCENT && reg::pollDue(wakeInputs(battery));
}

uint64_t RegisterProvider::wakeMicros(const uint16_t battery, const bool methodStepOnPanel) {
  reg::WakeInputs in = wakeInputs(battery);
  in.boundaryWakes = !methodStepOnPanel;
  reg::Schedule schedule;
  int64_t decidedView = 0;
  if (in.boundaryWakes && peek(schedule, &decidedView)) {
    // From the view the screen was decided for, when that is earlier: a draw or a poll comes between deciding and
    // arming.
    in.boundaryAt = reg::boundaryForArm(schedule, decidedView, in.now,
                                        registerOptions(battery < sleepframe::MIN_POLL_BATTERY_PERCENT));
  }
  const reg::WakePlan plan = reg::planWake(in);
  rtcPollDueAt = plan.pollDueAt;
  rtcArmed = true;
  rtcCardRetries = 0;
  [[maybe_unused]] static const char* const names[] = {"nothing", "the poll", "a boundary", "the step timer"};
  LOG_INF(LOG_TAG, "Sleeping for %llu s, for %s (poll planned %lld, boundary %lld, battery %u%%)",
          static_cast<unsigned long long>(plan.micros / 1000000), names[static_cast<int>(plan.reason)],
          static_cast<long long>(plan.pollDueAt), static_cast<long long>(in.boundaryAt),
          static_cast<unsigned>(battery));
  return plan.micros;
}

// ---- the panel
// --------------------------------------------------------------------------------------------------------

bool RegisterProvider::prepare(const uint16_t battery) {
  std::lock_guard<std::mutex> lock(mutex_);
  prepared_ = false;
  if (!loaded_) loadLocked();
  if (!valid_) return false;
  preparedView_ = reg::viewTime(registerNow());
  model_ = reg::buildModel(schedule_, preparedView_, registerOptions(battery < sleepframe::MIN_POLL_BATTERY_PERCENT),
                           registerWords());
  panel_ = reg::panelFor(model_);
  prepared_ = true;
  return true;
}

reg::Why RegisterProvider::planRedraw(const uint16_t battery, const bool methodStepOnPanel) {
  if (!prepare(battery)) return reg::Why::Same;
  const reg::Panel shown{rtcPanelSig, rtcPanelAsOf};
  std::lock_guard<std::mutex> lock(mutex_);
  const reg::Why why = reg::redrawReason(shown, model_, methodStepOnPanel);
  [[maybe_unused]] static const char* const names[] = {"nothing changed", "a Method step is on the panel",
                                                       "the panel shows something else", "the screen changed",
                                                       "the footer is an hour behind"};
  LOG_INF(LOG_TAG, "Redraw: %s", names[static_cast<int>(why)]);
  return why;
}

bool RegisterProvider::armed() { return rtcArmed; }

void RegisterProvider::disarm() {
  rtcArmed = false;
  rtcCardRetries = 0;
}

uint64_t RegisterProvider::cardRetryMicros(const uint16_t batteryPercent) {
  const reg::CardRetry plan = reg::planCardRetry(rtcArmed, rtcCardRetries, batteryPercent);
  rtcCardRetries = plan.retries;
  LOG_INF(LOG_TAG, "The card or the settings could not be read: trying again in %llu s (try %u)",
          static_cast<unsigned long long>(plan.micros / 1000000), static_cast<unsigned>(plan.retries));
  return plan.micros;
}

void RegisterProvider::clearPanel() {
  rtcPanelSig = 0;
  rtcPanelAsOf = 0;
}

bool RegisterProvider::drawPrepared(GfxRenderer& renderer) {
  reg::Model model;
  reg::Panel panel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!prepared_) return false;
    model = model_;
    panel = panel_;
  }
  RenderLock lock;  // the font caches and the framebuffer belong to the render path
  const auto orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const RendererMeter meter(renderer);
  const reg::Layout layout = reg::layoutScreen(model, registerWords(), meter);
  display.setInverted(false);
  renderer.setRenderMode(GfxRenderer::BW);
  drawRegisterLayout(renderer, layout);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  renderer.setOrientation(orientation);
  rtcPanelSig = panel.sig;
  rtcPanelAsOf = panel.asOf;
  LOG_INF(LOG_TAG, "Drawn: %s, %s", model.stateWord.c_str(), model.footer.c_str());
  return true;
}

// ---- drawing
// ----------------------------------------------------------------------------------------------------------

int RendererMeter::width(const reg::FontRole role, const std::string& text) const {
  const Face face = faceFor(role);
  return renderer.getTextAdvanceX(face.fontId, text.c_str(), face.style, face.tracking);
}

int RendererMeter::lineHeight(const reg::FontRole role) const { return renderer.getLineHeight(faceFor(role).fontId); }

void drawRegisterLayout(GfxRenderer& renderer, const reg::Layout& layout) {
  renderer.clearScreen();
  for (const reg::Item& item : layout.items) {
    switch (item.kind) {
      case reg::Item::Kind::Text: {
        const Face face = faceFor(item.role);
        renderer.drawText(face.fontId, item.x, item.y, item.text.c_str(), true, face.style,
                          BidiUtils::BidiBaseDir::AUTO, face.tracking);
        break;
      }
      case reg::Item::Kind::Rule:
        renderer.fillRect(item.x, item.y, item.w, item.thickness, true);
        break;
      case reg::Item::Kind::Frame:
        renderer.fillRect(item.x, item.y, item.w, item.thickness, true);
        renderer.fillRect(item.x, item.y + item.h - item.thickness, item.w, item.thickness, true);
        renderer.fillRect(item.x, item.y, item.thickness, item.h, true);
        renderer.fillRect(item.x + item.w - item.thickness, item.y, item.thickness, item.h, true);
        break;
    }
  }
}

}  // namespace ambient
