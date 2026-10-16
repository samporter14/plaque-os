#include "ConnectToMacActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "NetworkText.h"
#include "ambient/ReaderIdentity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NetworkProfileDevice.h"

namespace {
constexpr char CONNECTION_CHECK_PATH[] = "/Lamina/connection-check.json";
constexpr size_t READ_CHUNK = 128;
constexpr size_t MAX_READ_PER_PASS = 4096;
// HWCDC's receive queue while this screen listens: big enough for a whole legal transfer (the interrupt drops what does
// not fit, see netprov::RX_QUEUE_BYTES), then smaller ones if the memory is not there; and its own default
// (HWCDC::begin) to go back to.
constexpr size_t RX_QUEUE_LISTENING = netprov::RX_QUEUE_BYTES;
constexpr size_t RX_QUEUE_FALLBACK = 8192;
constexpr size_t RX_QUEUE_DEFAULT = 256;
constexpr uint32_t AWAKE_AFTER_TRAFFIC_MS = 120000;
constexpr uint32_t FAST_LOOP_AFTER_TRAFFIC_MS = 5000;
constexpr uint32_t SEND_TIMEOUT_MS = 1000;

portMUX_TYPE rxResizeMux = portMUX_INITIALIZER_UNLOCKED;

// HWCDC's interrupt always empties the hardware FIFO into its receive queue and drops whatever the queue cannot hold
// (it never leaves the endpoint unread, so the host is never held back), and the default 256 bytes is less than one
// DATA line. setRxBufferSize() deletes and recreates that queue. The USB Serial/JTAG interrupt was allocated by
// Serial.begin() in setup(), on the loop task's core, which is this one: masking this core's interrupts for the swap
// keeps the interrupt away from a queue that is being replaced.
bool resizeSerialRx(const size_t bytes) {
  portENTER_CRITICAL(&rxResizeMux);
  const size_t made = logSerial.setRxBufferSize(bytes);
  portEXIT_CRITICAL(&rxResizeMux);
  return made == bytes;
}

// The largest block internal RAM can still give: the receive queue is one such block, so this is what decides whether
// the next size can be had.
unsigned largestInternalBlock() { return static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)); }

bool cardReadable() { return Storage.ready() && Storage.usbDriveState() == UsbDriveState::Unsupported; }

bool readCardFile(const char* path, std::string& out) {
  return cardReadable() && Storage.exists(path) &&
         Storage.readFileToString("NETP", path, netprov::MAX_REPLY_FILE_BYTES, out);
}

bool within(const uint32_t since, const uint32_t windowMs) {
  return since != 0 && static_cast<uint32_t>(millis() - since) < windowMs;
}
}  // namespace

ConnectToMacActivity::ConnectToMacActivity(GfxRenderer& renderer, MappedInputManager& input)
    : Activity("ConnectToMac", renderer, input) {}

ConnectToMacActivity::~ConnectToMacActivity() { releaseResources(); }

void ConnectToMacActivity::onEnter() {
  Activity::onEnter();
  status = Status::Listening;
  psramBuffer = HalMemory::allocatePsram(netprofile::MAX_BUNDLE_BYTES);
  uint8_t* buffer = psramBuffer.get();
  if (!buffer) {
    internalBuffer = makeUniqueNoThrow<uint8_t[]>(netprofile::MAX_BUNDLE_BYTES);
    buffer = internalBuffer.get();
  }
  netprov::Host& host = *this;  // the private base converts here, not inside makeUniqueNoThrow
  if (buffer) session = makeUniqueNoThrow<netprov::Session>(host, buffer, netprofile::MAX_BUNDLE_BYTES);
  if (!session) {
    LOG_ERR("NETP", "OOM: USB setup buffer (%u bytes)", static_cast<unsigned>(netprofile::MAX_BUNDLE_BYTES));
    releaseResources();
    status = Status::NoMemory;
    requestUpdate();
    return;
  }
  if (logSerial.available() < 0) logSerial.begin(115200);  // a build without the serial log never started it
  // The old queue is deleted before the new one is made: if that fails there is no queue at all, so try a smaller one,
  // and in the end make the default. A smaller queue than a whole transfer can lose bytes if this task is held up (the
  // checksum then fails the transfer, and the Mac sends it again); the log says which size it is.
  size_t queue = 0;
  for (const size_t wanted : {RX_QUEUE_LISTENING, RX_QUEUE_FALLBACK}) {
    if (resizeSerialRx(wanted)) {
      queue = wanted;
      break;
    }
  }
  if (queue == 0) {
    LOG_ERR("NETP", "Could not enlarge the USB receive queue: a long transfer may be cut");
    if (resizeSerialRx(RX_QUEUE_DEFAULT)) {
      queue = RX_QUEUE_DEFAULT;
    } else {
      LOG_ERR("NETP", "USB input is unavailable");
    }
  } else if (queue < RX_QUEUE_LISTENING) {
    LOG_ERR("NETP", "USB receive queue of %u bytes, not %u: a transfer may be cut if this task is held up",
            static_cast<unsigned>(queue), static_cast<unsigned>(RX_QUEUE_LISTENING));
  }
  rxEnlarged = true;
  // What was granted, and what internal RAM had left in one piece, so that a transfer that lost bytes can be read from
  // the log.
  LOG_INF("NETP", "USB receive queue: %u bytes (wanted %u); largest free internal block %u",
          static_cast<unsigned>(queue), static_cast<unsigned>(RX_QUEUE_LISTENING), largestInternalBlock());
  netprov::claimSerialInput(true);
  LOG_INF("NETP", "Connect to Mac: listening (%s buffer)", psramBuffer ? "PSRAM" : "internal");
  requestUpdate();
}

void ConnectToMacActivity::onExit() {
  {
    RenderLock lock;  // the dialog is drawing state that render() reads
    dialog.dismiss();
  }
  releaseResources();
  Activity::onExit();
}

// Safe from the destructor too, where nothing else draws any more: the dialog is part of this object and goes with it.
void ConnectToMacActivity::releaseResources() {
  if (session) {
    session->close();  // a pending question is answered ERR declined; the buffer is wiped
    session.reset();
  }
  pending = netprofile::Profile();
  asking = Asking::None;
  if (rxEnlarged) {
    const bool restored = resizeSerialRx(RX_QUEUE_DEFAULT);
    if (!restored) LOG_ERR("NETP", "Could not restore the USB receive queue");
    LOG_INF("NETP", "USB receive queue back to %u bytes (%s); largest free internal block %u",
            static_cast<unsigned>(restored ? RX_QUEUE_DEFAULT : 0), restored ? "restored" : "NOT restored",
            largestInternalBlock());
    rxEnlarged = false;
    netprov::claimSerialInput(false);
    LOG_INF("NETP", "Connect to Mac: closed");
  }
  if (psramBuffer) {
    netprofile::wipe(psramBuffer.get(), netprofile::MAX_BUNDLE_BYTES);
    psramBuffer.reset();
  }
  if (internalBuffer) {
    netprofile::wipe(internalBuffer.get(), netprofile::MAX_BUNDLE_BYTES);
    internalBuffer.reset();
  }
}

void ConnectToMacActivity::pumpSerial() {
  uint8_t chunk[READ_CHUNK];
  size_t total = 0;
  while (session && total < MAX_READ_PER_PASS) {
    const int available = logSerial.available();
    if (available <= 0) break;
    const size_t got = logSerial.read(chunk, std::min<size_t>(static_cast<size_t>(available), sizeof(chunk)));
    if (got == 0 || got > sizeof(chunk)) break;
    session->feed(chunk, got, millis());
    total += got;
  }
  netprofile::wipe(chunk, sizeof(chunk));  // part of a DATA line is part of a secret
}

void ConnectToMacActivity::setStatus(const Status next, const std::string& detail) {
  {
    RenderLock lock;
    statusDetail = detail;
    status = next;
  }
  requestUpdate();
}

void ConnectToMacActivity::loop() {
  if (session) {
    const uint32_t errorsBefore = session->errorCount();
    pumpSerial();
    session->tick(millis(), HWCDC::isPlugged());
    const netprov::Session::State state = session->state();
    if (session->errorCount() != errorsBefore) {
      setStatus(Status::Rejected, netui::errorText(session->lastError()));
    } else if (state != shownState && state == netprov::Session::State::Receiving) {
      setStatus(Status::Receiving);
    }
    shownState = state;
  }
  if (dialog.isActive()) {
    bool repaint = false;
    const NetChoiceDialog::Choice choice = dialog.handleInput(mappedInput, repaint);
    if (choice != NetChoiceDialog::Choice::None) decide(choice == NetChoiceDialog::Choice::Action);
    if (repaint) {
      if (session) session->questionTouched(millis());  // the person is working it: the time to answer starts again
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
}

void ConnectToMacActivity::decide(const bool act) {
  using netprofile::Error;
  if (asking == Asking::Install) {
    const Error result = act ? netprofile::install(pending) : Error::Declined;
    pending = netprofile::Profile();  // the secrets go as soon as the answer is in
    asking = Asking::None;
    if (session) session->installDecided(result);
    if (result == Error::None) {
      setStatus(Status::Installed);
    } else if (result == Error::Declined) {
      setStatus(Status::Declined);
    } else {
      setStatus(Status::Rejected, netui::errorText(result));
    }
  } else if (asking == Asking::Remove) {
    Error result = Error::Declined;
    if (act) result = netprofile::remove() ? Error::None : Error::StorageFailed;
    asking = Asking::None;
    if (session) session->removeDecided(result);
    if (result == Error::None) {
      setStatus(Status::Removed);
    } else if (result == Error::Declined) {
      setStatus(Status::Listening);
    } else {
      setStatus(Status::RemoveFailed);
    }
  }
}

// ---- the protocol's view of the reader -------------------------------------------------------------------------

void ConnectToMacActivity::sendLine(const std::string_view line) {
  std::string out(line);
  out.push_back('\n');
  // One write when the port keeps up, so a log line cannot land inside the reply; the rest if the host was slow.
  size_t done = 0;
  const uint32_t until = millis() + SEND_TIMEOUT_MS;
  while (done < out.size()) {
    done += logSerial.write(reinterpret_cast<const uint8_t*>(out.data()) + done, out.size() - done);
    if (done >= out.size() || static_cast<int32_t>(millis() - until) >= 0) break;
    delay(1);
  }
  if (done < out.size())
    LOG_ERR("NETP", "USB reply cut short (%u of %u bytes)", static_cast<unsigned>(done),
            static_cast<unsigned>(out.size()));
}

// What reader.json holds, from the reader itself: the Mac gets the Wi-Fi MAC even when the card is out or the file
// could not be written. The card's copy is the fallback, for an identity NVS could not give.
bool ConnectToMacActivity::readerInfo(std::string& out) {
  out = ambient::readerCardText();
  if (!out.empty()) return true;
  return readCardFile(readerid::CARD_PATH, out);
}

bool ConnectToMacActivity::profileSummary(std::string& out) {
  out = netprofile::summaryText();  // what /Lamina/network-profile.json holds, from NVS itself
  return true;
}

bool ConnectToMacActivity::checkResult(std::string& out) { return readCardFile(CONNECTION_CHECK_PATH, out); }

netprofile::Error ConnectToMacActivity::bundleArrived(const uint8_t* data, const size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const netprofile::Error error = netprofile::validate(text, netprofile::Source::Usb, pending);
  if (error != netprofile::Error::None) {
    pending = netprofile::Profile();
    return error;
  }
  // What is installed now is replaced (and a certificate profile's key with it): the question says so.
  const netprofile::Summary installed = netprofile::summary();
  const std::string details = netui::profileDetails(pending.info, netprofile::trustedTime() > 0, &installed);
  // Nobody's finger is on the screen (the Mac asked): the question is centred, Back highlighted.
  {
    RenderLock lock;  // the dialog is drawing state that render() reads
    dialog.show(tr(STR_NET_INSTALL_TITLE), pending.info.name.c_str(), details.c_str(), tr(STR_NET_BACK),
                tr(STR_NET_INSTALL), false, 0, 0);
  }
  asking = Asking::Install;
  requestUpdate();
  return netprofile::Error::None;
}

bool ConnectToMacActivity::removeArrived() {
  const netprofile::Summary installed = netprofile::summary();
  if (!installed.installed) return false;
  {
    RenderLock lock;  // the dialog is drawing state that render() reads
    dialog.show(tr(STR_NET_REMOVE_TITLE), installed.name.c_str(), nullptr, tr(STR_NET_KEEP), tr(STR_NET_REMOVE), false,
                0, 0);
  }
  asking = Asking::Remove;
  requestUpdate();
  return true;
}

void ConnectToMacActivity::questionEnded(const Ended why) {
  // Called from the session's tick on this task, with no answer from the person: nothing is installed or removed.
  {
    RenderLock lock;  // the dialog is drawing state that render() reads
    dialog.dismiss();
    pending = netprofile::Profile();  // the bundle's secrets go with the question
    asking = Asking::None;
  }
  LOG_INF("NETP", "The question was dropped: %s", why == Ended::TimedOut ? "no answer" : "the USB link is gone");
  setStatus(why == Ended::TimedOut ? Status::QuestionTimedOut : Status::QuestionLinkLost);
}

// ---- screen ----------------------------------------------------------------------------------------------------

bool ConnectToMacActivity::preventAutoSleep() {
  if (!session) return false;
  return session->state() != netprov::Session::State::Idle ||  // receiving, or a question open: awake until it ends
         within(session->lastTrafficMs(), AWAKE_AFTER_TRAFFIC_MS);
}

bool ConnectToMacActivity::skipLoopDelay() {
  if (!session) return false;
  return session->state() == netprov::Session::State::Receiving ||
         within(session->lastTrafficMs(), FAST_LOOP_AFTER_TRAFFIC_MS);
}

void ConnectToMacActivity::render(RenderLock&&) {
  if (dialog.processRender(renderer, mappedInput)) return;
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_NET_CONNECT_MAC));

  const int side = metrics.contentSidePadding > 0 ? metrics.contentSidePadding : 20;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const int bottom = height - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int band = (bottom - top) / 3;
  UITheme::drawCenteredWrappedText(renderer, Rect{side, top, width - 2 * side, band}, UI_12_FONT_ID,
                                   tr(STR_NET_MAC_INTRO), 6, true, EpdFontFamily::REGULAR,
                                   UITheme::TextVerticalAlignment::TOP);

  char line[200];
  const char* text = line;
  switch (status.load()) {
    case Status::Listening:
      text = tr(STR_NET_MAC_LISTENING);
      break;
    case Status::Receiving:
      snprintf(line, sizeof(line), tr(STR_NET_MAC_RECEIVING), session ? static_cast<int>(session->expected()) : 0);
      break;
    case Status::Installed:
      text = tr(STR_NET_MAC_INSTALLED);
      break;
    case Status::Declined:
      text = tr(STR_NET_MAC_DECLINED);
      break;
    case Status::Rejected:
      text = statusDetail.c_str();
      break;
    case Status::Removed:
      text = tr(STR_NET_REMOVED);
      break;
    case Status::RemoveFailed:
      text = tr(STR_NET_REMOVE_FAILED);
      break;
    case Status::NoMemory:
      text = tr(STR_NET_MAC_NO_MEMORY);
      break;
    case Status::QuestionTimedOut:
      text = tr(STR_NET_MAC_QUESTION_TIMEOUT);
      break;
    case Status::QuestionLinkLost:
      text = tr(STR_NET_MAC_QUESTION_LINK);
      break;
  }
  UITheme::drawCenteredWrappedText(renderer, Rect{side, top + band, width - 2 * side, band}, UI_12_FONT_ID, text, 5,
                                   true, EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::TOP);
  if (status.load() != Status::Listening && session)
    UITheme::drawCenteredWrappedText(renderer, Rect{side, top + 2 * band, width - 2 * side, band}, UI_10_FONT_ID,
                                     tr(STR_NET_MAC_LISTENING), 3, true, EpdFontFamily::REGULAR,
                                     UITheme::TextVerticalAlignment::TOP);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
