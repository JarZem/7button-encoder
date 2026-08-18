#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_CREDENTIAL_PUBLIC_KEY_MAX_DER 128
#define DEVICE_CREDENTIAL_PUBLIC_KEY_UNCOMPRESSED_LEN 65
#define DEVICE_CREDENTIAL_SIGNATURE_MAX_DER 80

esp_err_t device_credentials_init(void);
esp_err_t device_credentials_get_certificate_der(const uint8_t **der, size_t *der_len);
esp_err_t device_credentials_get_public_key_der(uint8_t *out, size_t out_size, size_t *written);
esp_err_t device_credentials_get_public_key_uncompressed(uint8_t out[DEVICE_CREDENTIAL_PUBLIC_KEY_UNCOMPRESSED_LEN]);
esp_err_t device_credentials_sign(const uint8_t *data, size_t data_len,
                                  uint8_t *signature_der, size_t signature_size,
                                  size_t *signature_len);

#ifdef __cplusplus
}
#endif
