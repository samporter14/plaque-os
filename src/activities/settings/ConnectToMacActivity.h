#pragma once

#include <HalMemory.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "NetChoiceDialog.h"
#include "activities/Activity.h"
#include "network/NetProvisionProtocol.h"
#include "network/NetworkProfile.h"

// Settings › System › Network › Connect to Mac: the USB setup link (network/NetProvisionProtocol.h). The reader
// listens on its USB serial port only while this screen is open, and the screen owns that input meanwhile (the main
// loop's own command reader stands aside). A profile is installed, or removed, only after the person chooses so on
// the screen; Back declines. The receive buffer is allocated once (PSRAM when there is some) and wiped after every
// transfer and on the way out. Only sizes and result codes reach the log. A question nobody answers for two minutes, or
// whose USB link has been gone for a few seconds, is taken as Back: the dialog goes, what was validated is forgotten,
// and the screen says why.
class ConnectToMacActivity final : public Activity, private netprov::Host {
 public:
  ConnectToMacActivity(GfxRenderer& renderer, MappedInputManager& input);
  ~ConnectToMacActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override;
  bool skipLoopDelay() override;

 private:
  enum class Asking : uint8_t { None, Install, Remove };
  enum class Status : uint8_t {
    Listening,
    Receiving,
    Installed,
    Declined,
    Rejected,
    Removed,
    RemoveFailed,
    NoMemory,
    QuestionTimedOut,
    QuestionLinkLost,
  };

  HalMemory::PsramBuffer psramBuffer;
  std::unique_ptr<uint8_t[]> internalBuffer;
  std::unique_ptr<netprov::Session> session;
  netprofile::Profile pending;  // validated, waiting for the person's answer
  NetChoiceDialog dialog;
  Asking asking = Asking::None;
  std::atomic<Status> status{Status::Listening};
  std::string statusDetail;  // the error sentence for Rejected (written under the render lock)
  std::string question;      // the install question's details
  netprov::Session::State shownState = netprov::Session::State::Idle;
  bool rxEnlarged = false;

  void pumpSerial();
  void decide(bool act);
  void setStatus(Status next, const std::string& detail = std::string());
  void releaseResources();

  // netprov::Host
  void sendLine(std::string_view line) override;
  bool readerInfo(std::string& out) override;
  bool profileSummary(std::string& out) override;
  bool checkResult(std::string& out) override;
  netprofile::Error bundleArrived(const uint8_t* data, size_t size) override;
  bool removeArrived() override;
  void questionEnded(Ended why) override;
};
