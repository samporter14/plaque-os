#pragma once
// The EAP client calls of the ESP-IDF supplicant that the join code makes, recorded by the fake radio.
#include <esp_err.h>

typedef enum {
  ESP_EAP_TYPE_NONE = 0,
  ESP_EAP_TYPE_TLS = (1 << 0),
  ESP_EAP_TYPE_TTLS = (1 << 1),
  ESP_EAP_TYPE_PEAP = (1 << 2),
  ESP_EAP_TYPE_FAST = (1 << 3),
  ESP_EAP_TYPE_ALL = (ESP_EAP_TYPE_TLS | ESP_EAP_TYPE_TTLS | ESP_EAP_TYPE_PEAP | ESP_EAP_TYPE_FAST),
} esp_eap_method_t;

esp_err_t esp_eap_client_set_eap_methods(esp_eap_method_t methods);
esp_err_t esp_eap_client_use_default_cert_bundle(bool use);
esp_err_t esp_eap_client_set_identity(const unsigned char* identity, int len);
esp_err_t esp_eap_client_set_ca_cert(const unsigned char* ca_cert, int ca_cert_len);
esp_err_t esp_eap_client_set_certificate_and_key(const unsigned char* client_cert, int client_cert_len,
                                                 const unsigned char* private_key, int private_key_len,
                                                 const unsigned char* private_key_passwd, int private_key_passwd_len);
esp_err_t esp_eap_client_set_domain_name(const char* domain_name);
esp_err_t esp_eap_client_set_disable_time_check(bool disable);
void esp_eap_client_clear_identity();
void esp_eap_client_clear_ca_cert();
void esp_eap_client_clear_certificate_and_key();
