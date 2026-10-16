#include "ConnectionCheckActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "WifiCredentialStore.h"
#include "ambient/AmbientSettings.h"
#include "ambient/StepTimerService.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NetworkProfile.h"

namespace {
// The first paint comes before any network work; this only bounds the wait if a render never reports back.
constexpr uint32_t FIRST_PAINT_WAIT_MS = 3000;
constexpr int BUTTON_HEIGHT = 64;  // a touch target at least 9 mm tall
constexpr int ROW_MIN_HEIGHT = 48;

StrId stageLabel(const conncheck::StageId stage) {
  switch (stage) {
    case conncheck::StageId::Join:
      return StrId::STR_CHECK_STAGE_JOIN;
    case conncheck::StageId::Network:
      return StrId::STR_CHECK_STAGE_NETWORK;
    case conncheck::StageId::Dns:
      return StrId::STR_CHECK_STAGE_DNS;
    case conncheck::StageId::Time:
      return StrId::STR_CHECK_STAGE_TIME;
    case conncheck::StageId::Https:
      return StrId::STR_CHECK_STAGE_HTTPS;
    case conncheck::StageId::Mailbox:
      return StrId::STR_CHECK_STAGE_MAILBOX;
  }
  return StrId::STR_CHECK_STAGE_JOIN;
}

StrId messageLabel(const conncheck::Message message) {
  switch (message) {
    case conncheck::Message::JoinFailed:
      return StrId::STR_CHECK_MSG_JOIN;
    case conncheck::Message::NoAddress:
      return StrId::STR_CHECK_MSG_NETWORK;
    case conncheck::Message::DnsFailed:
      return StrId::STR_CHECK_MSG_DNS;
    case conncheck::Message::TimeFailed:
      return StrId::STR_CHECK_MSG_TIME;
    case conncheck::Message::CertificateFailed:
      return StrId::STR_CHECK_MSG_CERTIFICATE;
    case conncheck::Message::MailboxUnreachable:
      return StrId::STR_CHECK_MSG_UNREACHABLE;
    case conncheck::Message::AccessRejected:
      return StrId::STR_CHECK_MSG_REJECTED;
    case conncheck::Message::UnexpectedAnswer:
      return StrId::STR_CHECK_MSG_UNEXPECTED;
    case conncheck::Message::Connected:
      return StrId::STR_CHECK_MSG_OK;
    case conncheck::Message::Canceled:
      return StrId::STR_CHECK_MSG_CANCELED;
    case conncheck::Message::NotSetUp:
      return StrId::STR_CHECK_MSG_NOT_SET_UP;
    case conncheck::Message::None:
      break;
  }
  return StrId::STR_CHECK_MSG_OK;
}

// The deadline a timed-out stage had, in seconds, for "No answer within 8 seconds."
int stageLimitSeconds(const conncheck::StageId stage, const conncheck::Report& report) {
  const conncheck::Limits limits;
  switch (stage) {
    case conncheck::StageId::Join:
      return static_cast<int>((report.network.auth == "eap-tls" ? limits.joinEapMs : limits.joinMs) / 1000);
    case conncheck::StageId::Network:
      return static_cast<int>(limits.networkMs / 1000);
    case conncheck::StageId::Dns:
      return static_cast<int>(limits.dnsMs / 1000);
    case conncheck::StageId::Time:
      return static_cast<int>(limits.timeMs / 1000);
    case conncheck::StageId::Https:
      return static_cast<int>(limits.httpsMs / 1000);
    case conncheck::StageId::Mailbox:
      return static_cast<int>(limits.mailboxMs / 1000);
  }
  return 0;
}
}  // namespace

// ---- lifecycle ------------------------------------------------------------------------------------------------------

void ConnectionCheckActivity::onEnter() {
  Activity::onEnter();
  {
    // The stores are read under the lock, as WifiSelectionActivity reads them: a render must not see them half-loaded.
    RenderLock lock(*this);
    WIFI_STORE.loadFromFile();
    // A board that does not load the Messages settings at boot reads them here.
    if (!AMBIENT_SETTINGS.configured()) AMBIENT_SETTINGS.loadFromFile();
  }
  config = conncheck::mailboxConfig();
  ports = makeUniqueNoThrow<conncheck::DevicePorts>(renderer);
  if (!ports) LOG_ERR("CHECK", "OOM: connection check ports");
  firstPaintDone.store(false);
  enteredAt = millis();
  checkWanted = ports != nullptr;
  publish();
}

void ConnectionCheckActivity::onExit() {
  leaving = true;
  checkWanted = false;
  // Connection closed, clock sync and join abandoned, radio off, result saved: all before the next screen starts.
  stopCheck();
  check.reset();
  ports.reset();
  Activity::onExit();
}

bool ConnectionCheckActivity::stepTimerAlertDue() {
  // Due while the timer still runs past its end: the alert is wanted, or already made and on its way up.
  return steptimer::alertWanted(STEP_TIMER.running(), STEP_TIMER.remainingMs(), false, false);
}

// ---- the check ------------------------------------------------------------------------------------------------------

void ConnectionCheckActivity::startCheck() {
  checkWanted = false;
  if (!ports || stepTimerAlertDue()) return;
  check.reset();
  config = conncheck::mailboxConfig();
  const conncheck::Ports wiring{ports->network, ports->time, ports->http, &ports->log};
  check = makeUniqueNoThrow<conncheck::Check>(wiring, config);
  if (!check) {
    LOG_ERR("CHECK", "OOM: connection check");
    return;
  }
  resultWritten = false;
  resultSaved = false;
  LOG_INF("CHECK", "Check connection: start (free=%u max_block=%u)", static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  check->start();
  lastRevision = check->revision();
  if (!check->running()) {
    concludeCheck();
    return;
  }
  publish();
}

void ConnectionCheckActivity::stepCheck() {
  if (!check || !check->running()) return;
  check->step();
  if (!check->running()) {
    concludeCheck();
    return;
  }
  if (check->revision() != lastRevision) {
    lastRevision = check->revision();
    publish();
  }
}

void ConnectionCheckActivity::stopCheck() {
  if (!check || !check->running()) return;
  check->cancel();
  concludeCheck();
}

// Every ending, a canceled one included, is saved once.
void ConnectionCheckActivity::concludeCheck() {
  if (!check) return;
  if (!resultWritten) {
    resultWritten = true;
    resultSaved = conncheck::writeResult(check->report(), config);
  }
  lastRevision = check->revision();
  LOG_INF("CHECK", "Check connection: %s (%s)", conncheck::outcomeName(check->report().outcome),
          conncheck::messageText(check->report().message));
  if (!leaving) publish();
}

// ---- input ----------------------------------------------------------------------------------------------------------

ConnectionCheckActivity::Button ConnectionCheckActivity::buttonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  Button button;
  button.x = metrics.contentSidePadding;
  button.w = renderer.getScreenWidth() - 2 * metrics.contentSidePadding;
  button.h = BUTTON_HEIGHT;
  button.y = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing - BUTTON_HEIGHT;
  return button;
}

bool ConnectionCheckActivity::buttonTapped() {
  if (!mappedInput.hasTouch()) return false;
  int x = 0;
  int y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return false;
  const Button button = buttonRect();
  return x >= button.x && x < button.x + button.w && y >= button.y && y < button.y + button.h;
}

void ConnectionCheckActivity::loop() {
  if (leaving) return;
  if (check && check->running() && stepTimerAlertDue()) {
    // The alert takes over on this pass: give it the reader with the radio already off.
    LOG_INF("CHECK", "A step timer's alert is due: the check gives way");
    stopCheck();
    return;
  }
  const bool back = mappedInput.wasReleased(MappedInputManager::Button::Back);
  const bool confirm = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  const bool tapped = buttonTapped();

  if (check && check->running()) {
    if (back || tapped) {
      stopCheck();
      return;
    }
    stepCheck();
    return;
  }
  if (back) {
    leaving = true;
    finish();
    return;
  }
  if (checkWanted) {
    if (tapped) {  // Cancel, before the check had begun
      checkWanted = false;
      publish();
    } else if (firstPaintDone.load() || static_cast<uint32_t>(millis() - enteredAt) >= FIRST_PAINT_WAIT_MS) {
      startCheck();
    }
    return;
  }
  if (confirm || tapped) startCheck();
}

// ---- what the screen shows ------------------------------------------------------------------------------------------

void ConnectionCheckActivity::describeFailure(const conncheck::Report& report, View& out) const {
  using conncheck::Detail;
  using conncheck::StageId;
  out.fact[0] = '\0';
  if (out.failedStage < 0) return;
  const auto stage = static_cast<StageId>(out.failedStage);
  const Detail detail = report.stage(stage).detail;
  const char* host = config.host.c_str();
  switch (detail) {
    case Detail::NothingConfigured:
      snprintf(out.fact, sizeof(out.fact), "%s", tr(STR_CHECK_FACT_NOTHING_SAVED));
      return;
    case Detail::NotFound:
      if (stage == StageId::Join) {
        snprintf(out.fact, sizeof(out.fact), "%s", tr(STR_CHECK_FACT_NOT_SEEN));
        return;
      }
      break;  // the mailbox's 404: its status below
    case Detail::AuthFailed:
      snprintf(out.fact, sizeof(out.fact), tr(STR_CHECK_FACT_AUTH), report.reason);
      return;
    case Detail::Failed:
      if (stage == StageId::Join && report.reason != 0) {
        snprintf(out.fact, sizeof(out.fact), tr(STR_CHECK_FACT_REASON), report.reason);
      } else if (stage == StageId::Dns) {
        snprintf(out.fact, sizeof(out.fact), tr(STR_CHECK_FACT_DNS), host);
      } else if (stage == StageId::Time) {
        snprintf(out.fact, sizeof(out.fact), "%s", tr(STR_CHECK_FACT_NO_TIME));
      }
      return;
    case Detail::Timeout:
      snprintf(out.fact, sizeof(out.fact), tr(STR_CHECK_FACT_TIMEOUT), stageLimitSeconds(stage, report));
      return;
    case Detail::NoAddress:
      snprintf(out.fact, sizeof(out.fact), "%s", tr(STR_CHECK_FACT_NO_ADDRESS));
      return;
    case Detail::ConnectFailed:
      snprintf(out.fact, sizeof(out.fact), tr(STR_CHECK_FACT_CONNECT), host);
      return;
    case Detail::Certificate:
      snprintf(out.fact, sizeof(out.fact), tr(STR_CHECK_FACT_CERTIFICATE), host);
      return;
    case Detail::TlsFailed:
      snprintf(out.fact, sizeof(out.fact), tr(STR_CHECK_FACT_TLS), host);
      return;
    case Detail::Redirect:
      snprintf(out.fact, sizeof(out.fact), tr(STR_CHECK_FACT_REDIRECT), report.locationHost);
      return;
    case Detail::Unauthorized:
    case Detail::ServerError:
    case Detail::UnexpectedResponse:
    case Detail::None:
      break;
  }
  if (report.httpStatus > 0 && report.httpStatus != 200) {
    snprintf(out.fact, sizeof(out.fact), tr(STR_CHECK_FACT_STATUS), report.httpStatus);
  } else if (detail == Detail::UnexpectedResponse) {
    snprintf(out.fact, sizeof(out.fact), "%s", tr(STR_CHECK_FACT_BAD_REPLY));
  }
}

void ConnectionCheckActivity::publish() {
  View next;
  if (check) {
    const conncheck::Report& report = check->report();
    for (size_t i = 0; i < conncheck::STAGE_COUNT; ++i) {
      next.status[i] = report.stages[i].status;
      next.ms[i] = report.stages[i].ms;
      if (report.stages[i].status == conncheck::Status::Failed) next.failedStage = static_cast<int>(i);
    }
    next.retainedTime = report.timeSource == clockprov::Source::Retained;
    next.running = check->running();
    next.ended = check->finished();
    next.message = report.message;
    next.saved = resultSaved;
    if (report.network.configured) {
      snprintf(next.network, sizeof(next.network), tr(STR_CHECK_NETWORK_FORMAT),
               netprofile::displaySafe(report.network.ssid).c_str());  // an SSID may hold invisible characters
    } else if (next.ended) {
      snprintf(next.network, sizeof(next.network), "%s", tr(STR_CHECK_FACT_NOTHING_SAVED));
    }
    describeFailure(report, next);
  } else {
    next.running = checkWanted;
  }
  {
    std::lock_guard<std::mutex> guard(viewMutex);
    published = next;
  }
  requestUpdate();
}

void ConnectionCheckActivity::render(RenderLock&&) {
  {
    std::lock_guard<std::mutex> guard(viewMutex);
    view = published;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int left = metrics.contentSidePadding;
  const int textWidth = width - 2 * metrics.contentSidePadding;
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int labelLine = renderer.getLineHeight(UI_12_FONT_ID);
  const int valueLine = renderer.getLineHeight(UI_10_FONT_ID);
  const int rowHeight = std::max(ROW_MIN_HEIGHT, labelLine + 2 * metrics.verticalSpacing);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_CHECK_CONNECTION));
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (view.network[0]) {
    const std::string line = renderer.truncatedText(SMALL_FONT_ID, view.network, textWidth);
    renderer.drawText(SMALL_FONT_ID, left, y, line.c_str());
  }
  y += smallLine + metrics.verticalSpacing;

  char value[48];
  for (size_t i = 0; i < conncheck::STAGE_COUNT; ++i) {
    const auto status = view.status[i];
    const bool active = status == conncheck::Status::Running;
    switch (status) {
      case conncheck::Status::Pending:
        snprintf(value, sizeof(value), "%s", view.ended ? tr(STR_CHECK_STATUS_NOT_RUN) : tr(STR_CHECK_STATUS_PENDING));
        break;
      case conncheck::Status::Running:
        snprintf(value, sizeof(value), "%s", tr(STR_CHECK_STATUS_RUNNING));
        break;
      case conncheck::Status::Ok:
      case conncheck::Status::Failed:
        snprintf(value, sizeof(value), tr(STR_CHECK_STATUS_TIMED),
                 status == conncheck::Status::Ok ? tr(STR_CHECK_STATUS_OK) : tr(STR_CHECK_STATUS_FAILED),
                 static_cast<unsigned>(view.ms[i] / 1000), static_cast<unsigned>(view.ms[i] % 1000 / 100));
        break;
      case conncheck::Status::Canceled:
        snprintf(value, sizeof(value), "%s", tr(STR_CHECK_STATUS_CANCELED));
        break;
      case conncheck::Status::NotRun:
        snprintf(value, sizeof(value), "%s", tr(STR_CHECK_STATUS_NOT_RUN));
        break;
    }
    const auto style = active || status == conncheck::Status::Failed ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value, style);
    const char* label = I18N.get(stageLabel(static_cast<conncheck::StageId>(i)));
    const std::string shown =
        renderer.truncatedText(UI_12_FONT_ID, label, textWidth - valueWidth - metrics.verticalSpacing, style);
    renderer.drawText(UI_12_FONT_ID, left, y + (rowHeight - labelLine) / 2, shown.c_str(), true, style);
    renderer.drawText(UI_10_FONT_ID, left + textWidth - valueWidth, y + (rowHeight - valueLine) / 2, value, true,
                      style);
    y += rowHeight;

    const char* note = nullptr;
    if (static_cast<int>(i) == view.failedStage && view.fact[0]) note = view.fact;
    if (i == static_cast<size_t>(conncheck::StageId::Time) && status == conncheck::Status::Ok && view.retainedTime)
      note = tr(STR_CHECK_FACT_RETAINED);
    if (note) {
      for (const auto& line : renderer.wrappedText(SMALL_FONT_ID, note, textWidth, 2)) {
        renderer.drawText(SMALL_FONT_ID, left, y, line.c_str());
        y += smallLine;
      }
      y += metrics.verticalSpacing / 2;
    }
    renderer.fillRect(left, y - 1, textWidth, 1, true);
  }

  if (view.ended && view.message != conncheck::Message::None) {
    y += metrics.verticalSpacing;
    for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, I18N.get(messageLabel(view.message)), textWidth, 3)) {
      renderer.drawText(UI_12_FONT_ID, left, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += labelLine;
    }
    y += metrics.verticalSpacing / 2;
    renderer.drawText(SMALL_FONT_ID, left, y, view.saved ? tr(STR_CHECK_SAVED) : tr(STR_CHECK_NOT_SAVED));
  }

  if (mappedInput.hasTouch()) {
    const Button button = buttonRect();
    renderer.drawRoundedRect(button.x, button.y, button.w, button.h, 2, 8, true);
    const char* label = view.running ? tr(STR_CANCEL) : tr(STR_CHECK_AGAIN);
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, button.x + (button.w - labelWidth) / 2, button.y + (button.h - labelLine) / 2,
                      label, true, EpdFontFamily::BOLD);
  }

  const auto labels = mappedInput.mapLabels(view.running ? tr(STR_CANCEL) : tr(STR_BACK),
                                            view.running ? "" : tr(STR_CHECK_AGAIN), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
  firstPaintDone.store(true);
}
