// Appended to Mbed TLS's default configuration for the host tests: the reader's ESP-IDF build has no
// MBEDTLS_HAVE_TIME_DATE (sdkconfig: CONFIG_MBEDTLS_HAVE_TIME_DATE is not set), so certificate dates are never judged
// by Mbed TLS itself there, and must not be here.
#undef MBEDTLS_HAVE_TIME_DATE
