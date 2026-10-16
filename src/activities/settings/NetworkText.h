#pragma once

#include <I18n.h>

#include <cstdio>
#include <string>

#include "network/NetworkProfile.h"

// The words the Network screens use for a profile and for what went wrong with one.
namespace netui {

inline const char* authLabel(const netprofile::Auth auth) {
  switch (auth) {
    case netprofile::Auth::Psk:
      return tr(STR_NET_AUTH_PSK);
    case netprofile::Auth::EapTls:
      return tr(STR_NET_AUTH_EAP_TLS);
    case netprofile::Auth::Open:
      break;
  }
  return tr(STR_NET_AUTH_OPEN);
}

// A plain sentence for the common failures; the shared code for the rest.
inline std::string errorText(const netprofile::Error error) {
  using netprofile::Error;
  switch (error) {
    case Error::CardKeyRefused:
      return tr(STR_NET_ERR_KEY_ON_CARD);
    case Error::StorageFull:
      return tr(STR_NET_ERR_STORAGE_FULL);
    case Error::StorageFailed:
      return tr(STR_NET_ERR_STORAGE);
    case Error::ClientCertExpired:
      return tr(STR_NET_ERR_EXPIRED);
    case Error::ClientCertNotYetValid:
      return tr(STR_NET_ERR_NOT_YET_VALID);
    default:
      break;
  }
  char text[160];
  snprintf(text, sizeof(text), tr(STR_NET_ERR_INVALID), netprofile::errorCode(error));
  return text;
}

// What installing a profile would replace: the one installed now, by name and kind, and for a certificate profile that
// its key is deleted with it (Remove and Replace both do; the reader keeps no usable copy, though NVS only marks the
// entries deleted: the bytes stay in flash until the page is reclaimed). Empty with nothing installed.
inline std::string replaceNote(const netprofile::Summary& current) {
  if (!current.installed) return std::string();
  char text[200];
  snprintf(text, sizeof(text), tr(STR_NET_REPLACES), netprofile::displaySafe(current.name).c_str(),
           authLabel(current.auth));
  std::string note = text;
  if (current.auth == netprofile::Auth::EapTls) {
    note += ' ';
    note += tr(STR_NET_REPLACES_DELETES_KEY);
  }
  return note;
}

// The lines the install question shows: network, security, and for EAP-TLS the server and the certificate; what the
// reader cannot do for the person (judge the server certificate's dates) and how the secret is kept (unencrypted, in
// the reader's flash); and, when a profile is installed already, what installing replaces. The network name is drawn
// without its invisible and direction-changing characters.
inline std::string profileDetails(const netprofile::Summary& s, const bool datesJudged,
                                  const netprofile::Summary* replacing = nullptr) {
  std::string text;
  const auto line = [&text](const char* label, const std::string& value) {
    if (!text.empty()) text += '\n';
    text += label;
    text += ": ";
    text += value;
  };
  const auto note = [&text](const char* words) {
    if (!text.empty()) text += '\n';
    text += words;
  };
  line(tr(STR_NET_SSID), netprofile::displaySafe(s.ssid));
  line(tr(STR_NET_SECURITY), authLabel(s.auth));
  if (s.auth == netprofile::Auth::EapTls) {
    line(tr(STR_NET_SERVER_NAME), s.serverDomain);
    line(tr(STR_NET_CERTIFICATE), netprofile::displaySafe(s.clientCertSubject));
    if (s.hasCertDates) line(tr(STR_NET_CERT_EXPIRES), netprofile::isoDate(s.clientCertNotAfter));
    if (!datesJudged) note(tr(STR_NET_DATES_UNCHECKED));
    note(tr(STR_NET_SERVER_DATES_UNCHECKED));
    note(tr(STR_NET_KEY_UNENCRYPTED));
  } else if (s.auth == netprofile::Auth::Psk) {
    note(tr(STR_NET_PSK_UNENCRYPTED));
  }
  if (replacing) {
    const std::string replaces = replaceNote(*replacing);
    if (!replaces.empty()) note(replaces.c_str());
  }
  return text;
}

}  // namespace netui
