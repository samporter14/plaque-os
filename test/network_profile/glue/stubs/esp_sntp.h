#pragma once
// esp_sntp.h and lwIP's SNTP slots, as far as lib/TrustedTime uses them, over the fake SNTP client (GlueStubs.cpp).
#include <sys/time.h>

#include <cstdint>

enum sntp_sync_status_t { SNTP_SYNC_STATUS_RESET, SNTP_SYNC_STATUS_COMPLETED, SNTP_SYNC_STATUS_IN_PROGRESS };
void esp_sntp_set_sync_status(sntp_sync_status_t value);
sntp_sync_status_t sntp_get_sync_status();
void sntp_set_time_sync_notification_cb(void (*callback)(struct timeval*));
bool esp_sntp_enabled();
void esp_sntp_stop();

#define LWIP_DHCP_GET_NTP_SRV 1
#define LWIP_DHCP_MAX_NTP_SERVERS 1
using u8_t = unsigned char;
struct ip_addr_t {
  unsigned addr = 0;
};
#define ip_addr_isany(a) ((a) == nullptr || (a)->addr == 0)
const char* esp_sntp_getservername(u8_t idx);
const ip_addr_t* esp_sntp_getserver(u8_t idx);
void esp_sntp_setserver(u8_t idx, const ip_addr_t* addr);
void esp_sntp_servermode_dhcp(bool enable);
