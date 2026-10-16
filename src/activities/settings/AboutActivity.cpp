#include "AboutActivity.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalFrontlight.h>
#include <HalTiltSensor.h>
#include <PlaqueBoard.h>
#include <esp_mac.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "ambient/AmbientSettings.h"
#include "ambient/ReaderIdentity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
// The row labels are deliberately hardcoded English, exempt from the tr() rule: support reads these screenshots across
// every device language, so the labels must be identical on every unit. That includes the Plaque readers' Model and
// Wi-Fi MAC rows and the note under the MAC: the person registering the reader reads the same words support does.

// Chip part numbers, not user prose — deliberately untranslated.
const char* displayControllerName(const BoardConfig::DisplayController c) {
  switch (c) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::ED2208:
      return "ED2208";
    case BoardConfig::DisplayController::LgfxEpd:
      return "LovyanGFX EPD";
    case BoardConfig::DisplayController::IT8951:
      return "IT8951";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    case BoardConfig::DisplayController::UC8279C:
      return "UC8279C";
  }
  return "?";
}

const char* touchControllerName(const BoardConfig::TouchController c) {
  switch (c) {
    case BoardConfig::TouchController::None:
      return nullptr;
    case BoardConfig::TouchController::Chsc6x:
      return "CHSC6X";
    case BoardConfig::TouchController::Gt911:
      return "GT911";
    case BoardConfig::TouchController::Ft5x06:
      return "FT5x06";
    case BoardConfig::TouchController::Ft6336u:
      return "FT6336U";
    case BoardConfig::TouchController::Gslx680:
      return "GSLX680";
  }
  return nullptr;
}
}  // namespace

AboutActivity::AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("About", renderer, mappedInput) {}

const char* AboutActivity::headerTitle() const { return "About"; }

void AboutActivity::addRow(const char* label, std::string value, const char* subtitle) {
  if (count_ >= MAX_ITEMS) return;
  rowItems_[count_] = freeink::ui::ListItem{};
  rowItems_[count_].label = label;
  rowItems_[count_].subtitle = subtitle;
  rowItems_[count_].actionValue = static_cast<int16_t>(count_);
  rowValues_[count_] = std::move(value);
  ++count_;
}

void AboutActivity::onEnter() {
  // Hardware and firmware information is fixed after boot; the rows are filled once, before the first render can read
  // them. BoardConfig::ACTIVE reflects RUNTIME detection: selectDevice() picked the profile and
  // applyXteinkDisplayController() may have promoted the display controller to the panel actually found on the bus.
  count_ = 0;
  const plaque::Board board = plaque::board();
  const plaque::Model model = board.model;
  const bool plaqueReader = model != plaque::Model::Unsupported;
  char buf[32];
  addRow("Device", BoardConfig::ACTIVE.name);
  if (plaqueReader) addRow("Model", plaque::modelName(model));
  // The name on the row, the build below it (the string is too long for the value column), and the credit the MIT
  // license of CrossPoint Reader asks for.
  addRow("Firmware", "Plaque", CROSSPOINT_VERSION);
  addRow("Based on", "CrossPoint Reader", "MIT License");
  // The reader's own identity on the mailbox: its first 8 characters, and the whole UUID on the second line.
  readerUuid_ = ambient::readerUuid();
  addRow("Reader ID", readerUuid_.empty() ? std::string("Unavailable") : readerid::shortId(readerUuid_),
         readerUuid_.empty() ? nullptr : readerUuid_.c_str());
  addRow("Mailbox device", AMBIENT_SETTINGS.deviceId[0] ? AMBIENT_SETTINGS.deviceId : "Not set");
  snprintf(buf, sizeof(buf), "%s rev %u", ESP.getChipModel(), static_cast<unsigned>(ESP.getChipRevision()));
  addRow("Chip", buf);
  snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(ESP.getFlashChipSize() / (1024u * 1024u)));
  addRow("Flash", buf);
  addRow("Display Controller", displayControllerName(BoardConfig::ACTIVE.displayController));
  snprintf(buf, sizeof(buf), "%ux%u", static_cast<unsigned>(BoardConfig::ACTIVE.displayWidth),
           static_cast<unsigned>(BoardConfig::ACTIVE.displayHeight));
  addRow("Resolution", buf);
  // A Plaque reader without a touchscreen or a light (the X4 Classic) does not mention them: its model row says which
  // reader it is.
  const char* touch = touchControllerName(BoardConfig::ACTIVE.touch.controller);
  if (!plaqueReader || board.caps.touch) addRow("Touch", touch ? touch : "No");
  if (!plaqueReader || board.caps.frontlight) addRow("Frontlight", Frontlight.present() ? "Yes" : "No");
  addRow("RTC", halClock.isAvailable() ? "Yes" : "No");
  addRow("Tilt Sensor (IMU)", halTiltSensor.isAvailable() ? "Yes" : "No");
  // The station address: the one Wi-Fi registration (MSetup and the like) needs, and the one reader.json calls
  // station_mac. The File Transfer hotspot has an address of its own, which is never registered, so it is not shown.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (plaqueReader) {
    addRow("Wi-Fi MAC", readerid::macText(mac), "The address to register for Wi-Fi");
  } else {
    addRow("MAC Address", readerid::macText(mac));
  }
  UiListActivity::onEnter();
}

void AboutActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (int i = 0; i < count_; i++) {
    rowItems_[i].value = rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(count_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
