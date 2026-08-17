#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define DEVICE_ENROLLMENT_PROTOCOL_VERSION 1
#define DEVICE_ENROLLMENT_CANONICAL_DOMAIN "JaroslavZemanESP-DEVICE-ENROLL-v1"
#define DEVICE_AUTH_SECRET_DOMAIN "JaroslavZemanESP-DEVICE-AUTH-KEY-v1"
#define DEVICE_ENC_PUBLIC_KEY_LEN 65
#define DEVICE_AUTH_HMAC_LEN 32
#define DEVICE_AUTH_CHALLENGE_LEN 32

typedef struct {
    uint8_t protocol_version;
    const char *message_id;
    const uint8_t *challenge;
    size_t challenge_len;
    uint64_t enrollment_counter;
    const char *device_id;
    const char *zigbee_ieee;
    const char *ota_ecosystem;
    const char *device_model;
    const char *product_role;
    const char *hardware_revision;
    const char *chip_family;
    const char *flash_size;
    const char *firmware_version;
    const char *firmware_channel;
    uint16_t device_enc_key_id;
    const uint8_t *device_enc_public_key;
    size_t device_enc_public_key_len;
} device_enrollment_fields_t;

esp_err_t device_enrollment_encode_canonical(const device_enrollment_fields_t *fields,
                                             uint8_t *out,
                                             size_t out_len,
                                             size_t *written);

