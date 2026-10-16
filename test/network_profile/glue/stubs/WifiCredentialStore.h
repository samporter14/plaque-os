#pragma once
// The saved networks, in memory (FakeWorld.h Store), with the interface of the real WifiCredentialStore.
#include <optional>
#include <string>
#include <vector>

struct WifiCredential {
  std::string ssid;
  std::string password;
};
struct WifiCredentialSummary {
  std::string ssid;
  bool hasPassword = false;
  bool isLastConnected = false;
};

class WifiCredentialStore {
 public:
  static WifiCredentialStore& getInstance();
  void loadFromFile();
  std::optional<WifiCredential> getCredentialAt(size_t index) const;
  std::optional<std::string> getSsidAt(size_t index) const;
  size_t getCredentialCount() const;
  std::vector<WifiCredentialSummary> getCredentialSummaries() const;
  void setLastConnectedSsid(const std::string& ssid);
  std::string getLastConnectedSsid() const;
};
#define WIFI_STORE WifiCredentialStore::getInstance()
