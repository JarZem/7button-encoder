#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_SECURE_RANDOM_LEN 8
#define OTA_SECURE_ACK_MAX_LEN 96
#define OTA_SECURE_PROVISION_MAX_WIRE_LEN 100
#define OTA_SECURE_SSID_MAX_LEN 32
#define OTA_SECURE_PASSWORD_MAX_LEN 64
#define OTA_SECURE_HOST_MAX_LEN 64

typedef enum {
    OTA_SEC_STATE_IDLE = 0,
    OTA_SEC_STATE_WAIT_CHALLENGE = 1,
    OTA_SEC_STATE_WAIT_PROVISIONING = 2,
    OTA_SEC_STATE_PROVISIONED = 3,
} ota_secure_state_t;

typedef struct {
    char ssid[OTA_SECURE_SSID_MAX_LEN + 1];
    char password[OTA_SECURE_PASSWORD_MAX_LEN + 1];
    char ota_host[OTA_SECURE_HOST_MAX_LEN + 1];
    uint16_t ota_port;
    uint8_t wifi_security;
    uint8_t wifi_channel;
} ota_secure_provisioning_t;

/*
 * Strict protocol, endpoint 10 / cluster 0xFC00:
 *   ESP -> OTA  H|counter|signatureESP
 *   OTA -> ESP  A|random8-base64url|signatureOTA
 *   ESP -> OTA  R|signatureESP
 *   OTA -> ESP  P|base64url(AES-256-GCM(ciphertext||tag16))
 *
 * No counter/device-id is repeated after H. Both sides retain them as session state.
 * A is signed over device_id + counter + random.
 * R is signed over device_id + counter + random + OK.
 * The AES-GCM session key is derived from P-256 ECDH plus device_id/counter/random.
 * Any message received outside its required state is dropped as replay/out-of-order.
 */

esp_err_t ota_secure_session_init(void);
esp_err_t ota_secure_session_begin_hello(uint64_t counter);
esp_err_t ota_secure_session_accept_challenge(const char *payload,
                                              char response_out[OTA_SECURE_ACK_MAX_LEN]);
esp_err_t ota_secure_session_accept_provisioning(const char *payload);
void ota_secure_session_reset_for_retry(void);
/* Explicit user-requested reprovisioning: reset the protocol state to IDLE
 * while retaining the last valid provisioning data in prov_v2. */
void ota_secure_session_begin_reprovisioning(void);

ota_secure_state_t ota_secure_session_state(void);
const char *ota_secure_session_state_name(void);
bool ota_secure_session_is_provisioned(void);
esp_err_t ota_secure_session_load_provisioning(ota_secure_provisioning_t *out);

#ifdef __cplusplus
}
#endif
