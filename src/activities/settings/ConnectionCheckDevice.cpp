#include "ConnectionCheckDevice.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_mac.h>

#include <cstdio>
#include <cstring>

#include "ambient/AmbientSettings.h"
#include "ambient/MethodProtocol.h"
#include "ambient/ReaderIdentity.h"
#include "network/MessageTls.h"

namespace conncheck {
namespace {
unsigned heapFree() { return static_cast<unsigned>(ESP.getFreeHeap()); }
unsigned heapBlock() { return static_cast<unsigned>(ESP.getMaxAllocHeap()); }

const char* modelToken() {
  switch (BoardConfig::ACTIVE.board) {
    case BoardConfig::Board::XteinkX4Pro:
      return "x4pro";
    case BoardConfig::Board::XteinkX4Classic:
      return "x4c";
    default:
      return "";
  }
}

std::string stationMac() {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return {};
  return macText(mac);
}

// The card is the reader's to write: mounted, and not handed to a computer as a USB drive.
bool cardWritable() { return Storage.ready() && Storage.usbDriveState() == UsbDriveState::Unsupported; }
}  // namespace

// ---- time and log ---------------------------------------------------------------------------------------------------

bool DeviceTime::useRetained() { return message_tls::useRetainedClock(); }

void DeviceLog::stageEnded(const StageId stage, const Status status, const Detail detail, const uint32_t ms) {
  LOG_INF("CHECK", "stage %s: %s%s%s after %u ms (free=%u max_block=%u)", stageName(stage), statusName(status),
          detail == Detail::None ? "" : " ", detailName(detail), static_cast<unsigned>(ms), heapFree(), heapBlock());
}

// ---- configuration and the result file ------------------------------------------------------------------------------

Config mailboxConfig() {
  const auto& settings = AMBIENT_SETTINGS;
  Config config;
  config.configured = settings.configured();
  config.deviceId = settings.deviceId;
  config.host = method::urlHost(settings.baseUrl);
  return config;
}

bool writeResult(const Report& report, const Config& config) {
  if (!cardWritable()) {
    LOG_INF("CHECK", "The card is not the reader's to write now: result not saved");
    return false;
  }
  ResultContext context;
  message_tls::CertificateClock clock;
  if (message_tls::certificateClock(clock)) {
    context.finishedAt = clock.now;
    context.clock =
        clock.source == message_tls::ClockSource::Sntp ? clockprov::Source::Sntp : clockprov::Source::Retained;
  }
  context.bootMs = millis();
  context.readerUuid = ambient::readerUuid();
  context.model = modelToken();
  context.firmware = readerid::cleanFirmware(CROSSPOINT_VERSION);
  context.stationMac = stationMac();
  if (config.configured) context.mailboxHost = config.host;
  const std::string json = resultJson(context, report);

  if (!Storage.exists(RESULT_FOLDER) && !Storage.mkdir(RESULT_FOLDER)) {
    LOG_ERR("CHECK", "Could not make %s", RESULT_FOLDER);
    return false;
  }
  {
    HalFile file;
    if (!Storage.openFileForWrite("CHECK", RESULT_TMP_PATH, file)) return false;
    const size_t written = file.write(json.data(), json.size());
    file.flush();
    if (!file.close() || written != json.size()) {
      LOG_ERR("CHECK", "Short write of the result");
      Storage.remove(RESULT_TMP_PATH);
      return false;
    }
  }
  if (!Storage.replaceFile(RESULT_TMP_PATH, RESULT_PATH)) {
    LOG_ERR("CHECK", "Could not replace %s", RESULT_PATH);
    Storage.remove(RESULT_TMP_PATH);
    return false;
  }
  LOG_INF("CHECK", "Result saved (%u bytes): %s", static_cast<unsigned>(json.size()), outcomeName(report.outcome));
  return true;
}

}  // namespace conncheck
