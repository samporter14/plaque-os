#include "NetworkActivity.h"

#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <PlaqueBoard.h>
#include <esp_mac.h>

#include <cstdio>

#include "ConnectToMacActivity.h"
#include "ConnectionCheckActivity.h"
#include "MappedInputManager.h"
#include "NetworkText.h"
#include "ambient/MethodLayout.h"
#include "components/UITheme.h"
#include "network/NetworkProfileDevice.h"

namespace fui = freeink::ui;

namespace {
// A finger seen this recently opened the dialog: its safe choice goes under it.
constexpr uint32_t FINGER_FRESH_MS = 1500;
}  // namespace

NetworkActivity::NetworkActivity(GfxRenderer& renderer, MappedInputManager& input)
    : UiListActivity("Network", renderer, input) {}

const char* NetworkActivity::headerTitle() const { return tr(STR_NET_NETWORK); }

void NetworkActivity::onEnter() {
  importStatus.clear();
  removeStatus.clear();
  reload();
  rebuildRows();
  UiListActivity::onEnter();
}

void NetworkActivity::onExit() {
  // A bundle read from the card and not answered is declined: its file is gone, and nothing is installed unasked.
  if (cardRead.pending) netprofile::finishCardImport(cardRead, false);
  UiListActivity::onExit();
}

void NetworkActivity::reload() {
  profile = netprofile::summary();
  // The station address: what a registration page (U-M MSetup and the like) asks for. Never the hotspot's.
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char text[24];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  address = text;
  profileSubtitle.clear();
  if (profile.installed) {
    profileSubtitle = netprofile::displaySafe(profile.ssid);  // without direction overrides and the like
    profileSubtitle += " · ";
    profileSubtitle += netui::authLabel(profile.auth);
  }
  expires = profile.hasCertDates ? netprofile::isoDate(profile.clientCertNotAfter) : std::string();
}

void NetworkActivity::rebuildRows() {
  rowCount = 0;
  const auto add = [this](const Row row, const char* label) -> fui::ListItem& {
    fui::ListItem& item = rows[rowCount];
    item = fui::ListItem();
    item.label = label;
    item.actionValue = static_cast<int16_t>(rowCount);
    order[rowCount++] = row;
    return item;
  };
  fui::ListItem& addressRow = add(Row::Address, tr(STR_NET_WIFI_ADDRESS));
  addressRow.value = address.c_str();
  addressRow.subtitle = tr(STR_NET_WIFI_ADDRESS_HINT);
  add(Row::Band, tr(STR_NET_BAND)).value = tr(STR_NET_BAND_24);
  fui::ListItem& profileRow = add(Row::Profile, profile.installed ? profile.name.c_str() : tr(STR_NET_NO_PROFILE));
  if (profile.installed) {
    profileRow.subtitle = profileSubtitle.c_str();
  } else if (!removeStatus.empty()) {
    profileRow.subtitle = removeStatus.c_str();
  }
  if (profile.installed && profile.auth == netprofile::Auth::EapTls) {
    add(Row::Server, tr(STR_NET_SERVER_NAME)).value = profile.serverDomain.c_str();
    if (!expires.empty()) add(Row::Expires, tr(STR_NET_CERT_EXPIRES)).value = expires.c_str();
  }
  const plaque::Capabilities caps = plaque::board().caps;
  if (plaque::offersConnectionCheck(caps)) add(Row::CheckConnection, tr(STR_CHECK_CONNECTION));
  if (plaque::offersConnectToMac(caps)) add(Row::ConnectMac, tr(STR_NET_CONNECT_MAC));
  if (plaque::offersCardImport(caps)) {
    fui::ListItem& importRow = add(Row::ImportCard, tr(STR_NET_IMPORT_CARD));
    if (!importStatus.empty()) importRow.subtitle = importStatus.c_str();
  }
  if (profile.installed) add(Row::Remove, tr(STR_NET_REMOVE_PROFILE));
}

void NetworkActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  fui::ListProps props;
  props.items = rows;
  props.count = static_cast<uint16_t>(rowCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical keys stay in loop()
  props.valueInset = 8;
  props.rowHeight = method::MIN_ROW_HEIGHT_PX;  // the whole row is the target, at least 9 mm tall
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.maxLines = 3;
  syncListViewport(screen, props);
  screen.list(props);
}

void NetworkActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.hasTouch() && mappedInput.isScreenTouchHeld(x, y)) finger = {x, y, millis(), true};
  UiListActivity::loop();
}

bool NetworkActivity::handleCustomInput() {
  if (importDialog.isActive()) {
    bool repaint = false;
    const NetChoiceDialog::Choice choice = importDialog.handleInput(mappedInput, repaint);
    if (choice != NetChoiceDialog::Choice::None) finishImport(choice == NetChoiceDialog::Choice::Action);
    if (repaint) requestUpdate();
    return true;
  }
  if (!removeDialog.isActive()) return false;
  bool repaint = false;
  const NetChoiceDialog::Choice choice = removeDialog.handleInput(mappedInput, repaint);
  if (choice == NetChoiceDialog::Choice::Action) {
    const bool removed = netprofile::remove();
    RenderLock lock;
    removeStatus = removed ? tr(STR_NET_REMOVED) : tr(STR_NET_REMOVE_FAILED);
    importStatus.clear();
    reload();
    rebuildRows();
    activeNav().selected = 0;
  }
  if (repaint) requestUpdate();
  return true;
}

void NetworkActivity::activateIndex(const int index) {
  if (index < 0 || index >= rowCount) return;
  switch (order[index]) {
    case Row::CheckConnection:
      openCheckConnection();
      return;
    case Row::ConnectMac:
      openConnectToMac();
      return;
    case Row::ImportCard:
      importFromCard();
      return;
    case Row::Remove:
      promptRemove();
      return;
    case Row::Address:
    case Row::Band:
    case Row::Profile:
    case Row::Server:
    case Row::Expires:
      return;  // information only
  }
}

void NetworkActivity::promptRemove() {
  if (!profile.installed) return;
  app.clearTapFlash();
  const bool hasFinger = finger.valid && static_cast<uint32_t>(millis() - finger.at) < FINGER_FRESH_MS;
  {
    RenderLock lock;  // the dialog is drawing state that render() reads
    removeDialog.show(tr(STR_NET_REMOVE_TITLE), profile.name.c_str(), nullptr, tr(STR_NET_KEEP), tr(STR_NET_REMOVE),
                      hasFinger, finger.x, finger.y);
  }
  requestUpdate();
}

void NetworkActivity::importFromCard() {
  app.clearTapFlash();
  cardRead = netprofile::beginCardImport();
  if (cardRead.pending) {
    // The file is gone already. Say what is about to be installed and what it replaces, and wait for the answer: Back
    // is the highlighted choice, and a profile (a certificate one, with its key) is not replaced by anything unasked.
    const std::string details = netui::profileDetails(cardRead.profile.info, netprofile::trustedTime() > 0, &profile);
    const bool hasFinger = finger.valid && static_cast<uint32_t>(millis() - finger.at) < FINGER_FRESH_MS;
    {
      RenderLock lock;
      importDialog.show(tr(STR_NET_INSTALL_TITLE), cardRead.profile.info.name.c_str(), details.c_str(),
                        tr(STR_NET_BACK), tr(STR_NET_INSTALL), hasFinger, finger.x, finger.y);
    }
    requestUpdate();
    return;
  }
  std::string status;
  switch (cardRead.outcome) {
    case netprofile::CardOutcome::NoCard:
      status = tr(STR_NET_IMPORT_NO_CARD);
      break;
    case netprofile::CardOutcome::NoFile:
      status = tr(STR_NET_IMPORT_NO_FILE);
      break;
    case netprofile::CardOutcome::Rejected:
      status = netui::errorText(cardRead.error);
      break;
    case netprofile::CardOutcome::Installed:
      break;
  }
  if (cardRead.outcome == netprofile::CardOutcome::Rejected && !cardRead.wiped) {
    status += ' ';
    status += tr(STR_NET_IMPORT_NOT_WIPED);
  }
  RenderLock lock;
  importStatus = std::move(status);
  removeStatus.clear();
  reload();
  rebuildRows();
  requestUpdate();
}

void NetworkActivity::finishImport(const bool install) {
  const netprofile::CardResult result = netprofile::finishCardImport(cardRead, install);
  std::string status;
  if (result.outcome == netprofile::CardOutcome::Installed) {
    status = tr(STR_NET_IMPORT_INSTALLED);
  } else if (result.error == netprofile::Error::Declined) {
    status = tr(STR_NET_IMPORT_DECLINED);
  } else {
    status = netui::errorText(result.error);
  }
  if (!result.wiped) {
    status += ' ';
    status += tr(STR_NET_IMPORT_NOT_WIPED);
  }
  RenderLock lock;
  importStatus = std::move(status);
  removeStatus.clear();
  reload();
  rebuildRows();
  requestUpdate();
}

void NetworkActivity::openCheckConnection() {
  app.clearTapFlash();
  auto activity = makeUniqueNoThrow<ConnectionCheckActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("NETP", "OOM: connection check");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) { requestUpdate(); });
}

void NetworkActivity::openConnectToMac() {
  app.clearTapFlash();
  auto activity = makeUniqueNoThrow<ConnectToMacActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("NETP", "OOM: Connect to Mac");
    return;
  }
  startActivityForResult(std::move(activity), [this](const ActivityResult&) {
    RenderLock lock;
    importStatus.clear();
    removeStatus.clear();
    reload();
    rebuildRows();
  });
}

void NetworkActivity::render(RenderLock&& lock) {
  if (importDialog.processRender(renderer, mappedInput)) return;
  if (removeDialog.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
