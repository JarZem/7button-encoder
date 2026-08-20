#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_SECURE_RANDOM_LEN 16
#define OTA_SECURE_AUTH_TAG_LEN 32
#define OTA_SECURE_ACK_MAX_LEN 80
#define OTA_SECURE_PROVISION_MAX_WIRE_LEN 100
#define OTA_SECURE_SSID_MAX_LEN 32
#define OTA_SECURE_PASSWORD_MAX_LEN 64
#define OTA_SECURE_HOST_MAX_LEN 64

/*
 * Challenge wire format (ASCII, <= 100 bytes):
 *   A1|<hello_counter_dec>|<16-byte-random-base64url>|<crc32-hex>|<hmac32-base64url>
 *
 * The HMAC key is derived from P-256 ECDH(device_private, CA-verified OTA_server_public)
 * and the challenge counter/random. The OTA server certificate is verified against
 * the embedded root CA before the challenge is accepted.
 *
 * ESP success response (single response, no extra handshake):
 *   R1|<hello_counter_dec>|<hmac32-base64url>
 *
 * Provisioning wire format:
 *   P1|<base64url(AES-256-GCM(ciphertext || tag16))>
 *
 * AES-GCM key, nonce and AAD are derived from the accepted challenge session.
 */

typedef struct {
    char ssid[OTA_SECURE_SSID_MAX_LEN + 1];
    char password[OTA_SECURE_PASSWORD_MAX_LEN + 1];
    char ota_host[OTA_SECURE_HOST_MAX_LEN + 1];
    uint16_t ota_port;
    uint8_t wifi_security;
    uint8_t wifi_channel;
} ota_secure_provisioning_t;

esp_err_t ota_secure_session_init(void);

/* Returns ESP_OK only after CA/counter/CRC/HMAC validation and NVS persistence. */
esp_err_t ota_secure_session_accept_challenge(const char *payload,
                                              char ack_out[OTA_SECURE_ACK_MAX_LEN]);

/* Returns ESP_OK only after AES-GCM authentication, parsing and NVS persistence. */
esp_err_t ota_secure_session_accept_provisioning(const char *payload);

bool ota_secure_session_has_verified_challenge(void);
bool ota_secure_session_is_provisioned(void);
esp_err_t ota_secure_session_load_provisioning(ota_secure_provisioning_t *out);
void ota_secure_session_clear_pending(void);

#ifdef __cplusplus
}
#endif
