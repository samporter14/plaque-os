#pragma once
enum sntp_sync_status_t { SNTP_SYNC_STATUS_RESET, SNTP_SYNC_STATUS_COMPLETED, SNTP_SYNC_STATUS_IN_PROGRESS };
void esp_sntp_set_sync_status(sntp_sync_status_t value);
