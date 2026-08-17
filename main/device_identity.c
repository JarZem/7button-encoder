#include "device_identity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/ecp.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/private_access.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nwk/esp_zigbee_nwk.h"

#define OTA_SEC_NAMESPACE "ota_sec"
#define DEVICE_PRIVATE_KEY_NVS_KEY "dev_priv_v1"
#define DEVICE_KEY_ID_NVS_KEY "dev_key_id"
#define DEVICE_ENROLLMENT_COUNTER_NVS_KEY "enroll_counter"
#define DEVICE_PRIVATE_KEY_LEN 32
#define DEVICE_KEY_ID_V1 1

static const char *TAG = "device_identity";

static uint8_t s_public_key[DEVICE_ENC_PUBLIC_KEY_LEN];
static uint8_t s_private_key[DEVICE_PRIVATE_KEY_LEN];
static uint16_t s_key_id = DEVICE_KEY_ID_V1;
static bool s_initialized;

static int esp_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static esp_err_t open_sec_nvs(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(OTA_SEC_NAMESPACE, mode, handle);
}

static esp_err_t save_private_key(const uint8_t private_key[DEVICE_PRIVATE_KEY_LEN])
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_sec_nvs(NVS_READWRITE, &handle), TAG, "open secure NVS failed");
    esp_err_t err = nvs_set_blob(handle, DEVICE_PRIVATE_KEY_NVS_KEY, private_key, DEVICE_PRIVATE_KEY_LEN);
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, DEVICE_KEY_ID_NVS_KEY, DEVICE_KEY_ID_V1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t load_private_key(uint8_t private_key[DEVICE_PRIVATE_KEY_LEN], uint16_t *key_id)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_sec_nvs(NVS_READONLY, &handle), TAG, "open secure NVS failed");
    size_t len = DEVICE_PRIVATE_KEY_LEN;
    esp_err_t err = nvs_get_blob(handle, DEVICE_PRIVATE_KEY_NVS_KEY, private_key, &len);
    if (err == ESP_OK && len != DEVICE_PRIVATE_KEY_LEN) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        uint16_t stored_key_id = DEVICE_KEY_ID_V1;
        esp_err_t key_err = nvs_get_u16(handle, DEVICE_KEY_ID_NVS_KEY, &stored_key_id);
        if (key_err == ESP_OK || key_err == ESP_ERR_NVS_NOT_FOUND) {
            *key_id = stored_key_id;
        } else {
            err = key_err;
        }
    }
    nvs_close(handle);
    return err;
}

static esp_err_t derive_public_key_from_private(const uint8_t private_key[DEVICE_PRIVATE_KEY_LEN],
                                                uint8_t public_key[DEVICE_ENC_PUBLIC_KEY_LEN])
{
    esp_err_t err = ESP_OK;
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);

    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
        mbedtls_mpi_read_binary(&d, private_key, DEVICE_PRIVATE_KEY_LEN) != 0 ||
        mbedtls_ecp_mul(&grp, &q, &d, &grp.G, esp_rng, NULL) != 0 ||
        mbedtls_ecp_check_pubkey(&grp, &q) != 0 ||
        mbedtls_mpi_write_binary(&q.MBEDTLS_PRIVATE(X), public_key + 1, 32) != 0 ||
        mbedtls_mpi_write_binary(&q.MBEDTLS_PRIVATE(Y), public_key + 33, 32) != 0) {
        err = ESP_FAIL;
    } else {
        public_key[0] = 0x04;
    }

    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return err;
}

static esp_err_t generate_keypair(void)
{
    esp_err_t err = ESP_OK;
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;

    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);

    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
        mbedtls_ecp_gen_keypair(&grp, &d, &q, esp_rng, NULL) != 0 ||
        mbedtls_mpi_write_binary(&d, s_private_key, sizeof(s_private_key)) != 0 ||
        mbedtls_mpi_write_binary(&q.MBEDTLS_PRIVATE(X), s_public_key + 1, 32) != 0 ||
        mbedtls_mpi_write_binary(&q.MBEDTLS_PRIVATE(Y), s_public_key + 33, 32) != 0) {
        err = ESP_FAIL;
    } else {
        s_public_key[0] = 0x04;
        err = save_private_key(s_private_key);
    }

    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return err;
}

esp_err_t device_identity_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = load_private_key(s_private_key, &s_key_id);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "DEVELOPMENT SECURITY STATE: Flash/NVS encryption is not enabled; generating device P-256 key in NVS");
        err = generate_keypair();
    } else if (err == ESP_OK) {
        err = derive_public_key_from_private(s_private_key, s_public_key);
    }

    if (err != ESP_OK) {
        mbedtls_platform_zeroize(s_private_key, sizeof(s_private_key));
        ESP_LOGE(TAG, "device identity initialization failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "device encryption public key ready, key_id=%u", (unsigned)s_key_id);
    return ESP_OK;
}

esp_err_t device_identity_get_device_id(char device_id[DEVICE_ID_MAX_LEN])
{
    ESP_RETURN_ON_FALSE(device_id != NULL, ESP_ERR_INVALID_ARG, TAG, "missing device_id buffer");
    esp_zb_ieee_addr_t ieee;
    esp_zb_get_long_address(ieee);
    snprintf(device_id, DEVICE_ID_MAX_LEN, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             ieee[7], ieee[6], ieee[5], ieee[4], ieee[3], ieee[2], ieee[1], ieee[0]);
    return ESP_OK;
}

esp_err_t device_identity_get_public_key(uint8_t public_key[DEVICE_ENC_PUBLIC_KEY_LEN])
{
    ESP_RETURN_ON_ERROR(device_identity_init(), TAG, "identity not available");
    memcpy(public_key, s_public_key, DEVICE_ENC_PUBLIC_KEY_LEN);
    return ESP_OK;
}

esp_err_t device_identity_get_public_key_fingerprint(uint8_t fingerprint[DEVICE_PUBLIC_KEY_FINGERPRINT_LEN])
{
    ESP_RETURN_ON_FALSE(fingerprint != NULL, ESP_ERR_INVALID_ARG, TAG, "missing fingerprint buffer");
    ESP_RETURN_ON_ERROR(device_identity_init(), TAG, "identity not available");
    if (mbedtls_sha256(s_public_key, DEVICE_ENC_PUBLIC_KEY_LEN, fingerprint, 0) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

uint16_t device_identity_get_key_id(void)
{
    return s_key_id;
}

esp_err_t device_identity_next_enrollment_counter(uint64_t *counter)
{
    ESP_RETURN_ON_FALSE(counter != NULL, ESP_ERR_INVALID_ARG, TAG, "missing enrollment counter");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(open_sec_nvs(NVS_READWRITE, &handle), TAG, "open secure NVS failed");
    uint64_t value = 0;
    esp_err_t err = nvs_get_u64(handle, DEVICE_ENROLLMENT_COUNTER_NVS_KEY, &value);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        value++;
        err = nvs_set_u64(handle, DEVICE_ENROLLMENT_COUNTER_NVS_KEY, value);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        *counter = value;
    }
    return err;
}
