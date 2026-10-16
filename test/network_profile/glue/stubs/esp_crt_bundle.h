#pragma once
// The SDK's certificate bundle verifier: the glue tests do not make TLS connections, so attaching it is a no-op.
#include <esp_err.h>
#include <mbedtls/ssl.h>
esp_err_t esp_crt_bundle_attach(void* config);
bool esp_crt_bundle_in_use(const mbedtls_x509_crt* certificate);
