#include "device_credentials.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

static const char *TAG = "device_credentials";

extern const char device_cert_pem_start[] asm("_binary_device_cert_pem_start");
extern const char device_cert_pem_end[] asm("_binary_device_cert_pem_end");
extern const char device_private_pem_start[] asm("_binary_device_private_pem_start");
extern const char device_private_pem_end[] asm("_binary_device_private_pem_end");

static mbedtls_x509_crt s_cert;
static mbedtls_pk_context s_private_key;
static bool s_initialized;

static int esp_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

esp_err_t device_credentials_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    mbedtls_x509_crt_init(&s_cert);
    mbedtls_pk_init(&s_private_key);

    const size_t cert_len = (size_t)(device_cert_pem_end - device_cert_pem_start);
    const size_t key_len = (size_t)(device_private_pem_end - device_private_pem_start);

    int ret = mbedtls_x509_crt_parse(&s_cert,
                                     (const unsigned char *)device_cert_pem_start,
                                     cert_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "device certificate parse failed: -0x%04x", (unsigned)-ret);
        return ESP_FAIL;
    }

    ret = mbedtls_pk_parse_key(&s_private_key,
                               (const unsigned char *)device_private_pem_start,
                               key_len,
                               NULL,
                               0,
                               esp_rng,
                               NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "device private key parse failed: -0x%04x", (unsigned)-ret);
        return ESP_FAIL;
    }

    ret = mbedtls_pk_check_pair(&s_cert.pk, &s_private_key);
    if (ret != 0) {
        ESP_LOGE(TAG, "device certificate/private key mismatch: -0x%04x", (unsigned)-ret);
        return ESP_FAIL;
    }

    if (!mbedtls_pk_can_do(&s_private_key, MBEDTLS_PK_ECKEY)) {
        ESP_LOGE(TAG, "device private key is not an EC key");
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "device certificate/private key ready cert_der_len=%u",
             (unsigned)s_cert.raw.len);
    return ESP_OK;
}

esp_err_t device_credentials_get_certificate_der(const uint8_t **der, size_t *der_len)
{
    ESP_RETURN_ON_FALSE(der != NULL && der_len != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid certificate output");
    ESP_RETURN_ON_ERROR(device_credentials_init(), TAG, "credentials unavailable");
    *der = s_cert.raw.p;
    *der_len = s_cert.raw.len;
    return ESP_OK;
}

esp_err_t device_credentials_get_public_key_der(uint8_t *out, size_t out_size, size_t *written)
{
    ESP_RETURN_ON_FALSE(out != NULL && written != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid public key output");
    ESP_RETURN_ON_ERROR(device_credentials_init(), TAG, "credentials unavailable");

    unsigned char temp[DEVICE_CREDENTIAL_PUBLIC_KEY_MAX_DER];
    int len = mbedtls_pk_write_pubkey_der(&s_cert.pk, temp, sizeof(temp));
    if (len <= 0 || (size_t)len > out_size) {
        ESP_LOGE(TAG, "public key DER export failed len=%d", len);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, temp + sizeof(temp) - len, (size_t)len);
    *written = (size_t)len;
    return ESP_OK;
}

esp_err_t device_credentials_sign(const uint8_t *data, size_t data_len,
                                  uint8_t *signature_der, size_t signature_size,
                                  size_t *signature_len)
{
    ESP_RETURN_ON_FALSE(data != NULL && data_len > 0 && signature_der != NULL && signature_len != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid sign arguments");
    ESP_RETURN_ON_ERROR(device_credentials_init(), TAG, "credentials unavailable");

    uint8_t hash[32];
    if (mbedtls_sha256(data, data_len, hash, 0) != 0) {
        return ESP_FAIL;
    }

    size_t out_len = 0;
    int ret = mbedtls_pk_sign(&s_private_key,
                              MBEDTLS_MD_SHA256,
                              hash,
                              sizeof(hash),
                              signature_der,
                              signature_size,
                              &out_len,
                              esp_rng,
                              NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "HELLO signature failed: -0x%04x", (unsigned)-ret);
        return ESP_FAIL;
    }

    *signature_len = out_len;
    return ESP_OK;
}
