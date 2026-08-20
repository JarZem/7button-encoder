#include "ota_secure_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_credentials.h"
#include "device_identity.h"
#include "esp_check.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "nvs.h"

#define OTA_SEC_NAMESPACE "ota_sec"
#define OTA_SEC_CHALLENGE_KEY "chal_v1"
#define OTA_SEC_PROVISION_KEY "prov_v1"
#define OTA_SEC_CHALLENGE_MAGIC 0x43483131u /* CH11 */
#define OTA_SEC_PROVISION_MAGIC 0x50563131u /* PV11 */

#define OTA_SEC_RANDOM_B64URL_LEN 22
#define OTA_SEC_HMAC_B64URL_LEN 43
#define OTA_SEC_CRC_HEX_LEN 8
#define OTA_SEC_SESSION_KEY_LEN 32
#define OTA_SEC_GCM_TAG_LEN 16
#define OTA_SEC_GCM_NONCE_LEN 12
#define OTA_SEC_MAX_BINARY_WIRE 96
#define OTA_SEC_MAX_PLAINTEXT 96

#define OTA_SEC_CHALLENGE_PREFIX "A1|"
#define OTA_SEC_RESPONSE_PREFIX "R1|"
#define OTA_SEC_PROVISION_PREFIX "P1|"

static const char *TAG = "ota_secure_session";

static const char SESSION_DOMAIN[] = "JaroslavZemanESP-SESSION-v1";
static const char CHALLENGE_DOMAIN[] = "JaroslavZemanESP-CHALLENGE-v1";
static const char RESPONSE_DOMAIN[] = "JaroslavZemanESP-CHALLENGE-OK-v1";
static const char PROVISION_KEY_DOMAIN[] = "JaroslavZemanESP-PROVISION-KEY-v1";
static const char PROVISION_NONCE_DOMAIN[] = "JaroslavZemanESP-PROVISION-NONCE-v1";
static const char PROVISION_AAD_DOMAIN[] = "JaroslavZemanESP-PROVISION-AAD-v1";

typedef struct {
    uint32_t magic;
    uint64_t counter;
    uint8_t random[OTA_SECURE_RANDOM_LEN];
    uint32_t crc32;
} challenge_nvs_t;

typedef struct {
    uint32_t magic;
    ota_secure_provisioning_t config;
} provision_nvs_t;

static bool s_challenge_verified;
static bool s_provisioned;
static uint64_t s_counter;
static uint8_t s_random[OTA_SECURE_RANDOM_LEN];
static uint32_t s_crc32;
static uint8_t s_session_key[OTA_SEC_SESSION_KEY_LEN];

static void put_u64_be(uint8_t out[8], uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) out[7 - i] = (uint8_t)(value >> (i * 8));
}

static void put_u32_be(uint8_t out[4], uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint16_t get_u16_be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

static bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static esp_err_t hmac_sha256(const uint8_t *key, size_t key_len,
                             const uint8_t *data, size_t data_len,
                             uint8_t out[32])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md == NULL) return ESP_ERR_NOT_SUPPORTED;
    const int ret = mbedtls_md_hmac(md, key, key_len, data, data_len, out);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t base64url_decode_exact(const char *input, size_t expected_input_len,
                                        uint8_t *out, size_t out_len)
{
    if (input == NULL || out == NULL || strlen(input) != expected_input_len) return ESP_ERR_INVALID_ARG;

    char padded[160];
    if (expected_input_len + 4 >= sizeof(padded)) return ESP_ERR_INVALID_SIZE;
    memcpy(padded, input, expected_input_len);
    size_t padded_len = expected_input_len;
    for (size_t i = 0; i < padded_len; ++i) {
        if (padded[i] == '-') padded[i] = '+';
        else if (padded[i] == '_') padded[i] = '/';
        else if (!((padded[i] >= 'A' && padded[i] <= 'Z') ||
                   (padded[i] >= 'a' && padded[i] <= 'z') ||
                   (padded[i] >= '0' && padded[i] <= '9'))) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    while ((padded_len % 4) != 0) padded[padded_len++] = '=';
    padded[padded_len] = '\0';

    size_t written = 0;
    const int ret = mbedtls_base64_decode(out, out_len, &written,
                                          (const unsigned char *)padded, padded_len);
    memset(padded, 0, sizeof(padded));
    if (ret != 0 || written != out_len) {
        memset(out, 0, out_len);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t base64url_decode_variable(const char *input,
                                           uint8_t *out, size_t out_size,
                                           size_t *written)
{
    if (input == NULL || out == NULL || written == NULL) return ESP_ERR_INVALID_ARG;
    const size_t input_len = strlen(input);
    if (input_len == 0 || input_len + 4 >= 192) return ESP_ERR_INVALID_SIZE;

    char padded[192];
    memcpy(padded, input, input_len);
    size_t padded_len = input_len;
    for (size_t i = 0; i < padded_len; ++i) {
        if (padded[i] == '-') padded[i] = '+';
        else if (padded[i] == '_') padded[i] = '/';
        else if (!((padded[i] >= 'A' && padded[i] <= 'Z') ||
                   (padded[i] >= 'a' && padded[i] <= 'z') ||
                   (padded[i] >= '0' && padded[i] <= '9'))) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    while ((padded_len % 4) != 0) padded[padded_len++] = '=';
    padded[padded_len] = '\0';

    size_t out_len = 0;
    const int ret = mbedtls_base64_decode(out, out_size, &out_len,
                                          (const unsigned char *)padded, padded_len);
    memset(padded, 0, sizeof(padded));
    if (ret != 0) return ESP_ERR_INVALID_SIZE;
    *written = out_len;
    return ESP_OK;
}

static esp_err_t base64url_encode(const uint8_t *input, size_t input_len,
                                  char *out, size_t out_size)
{
    if (input == NULL || out == NULL || out_size < 2) return ESP_ERR_INVALID_ARG;
    size_t written = 0;
    int ret = mbedtls_base64_encode((unsigned char *)out, out_size, &written, input, input_len);
    if (ret != 0 || written >= out_size) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < written; ++i) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }
    while (written > 0 && out[written - 1] == '=') --written;
    out[written] = '\0';
    return ESP_OK;
}

static esp_err_t parse_hex_u32(const char *value, uint32_t *out)
{
    if (value == NULL || out == NULL || strlen(value) != OTA_SEC_CRC_HEX_LEN) return ESP_ERR_INVALID_ARG;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 16);
    if (end == value || *end != '\0' || parsed > 0xfffffffful) return ESP_ERR_INVALID_ARG;
    *out = (uint32_t)parsed;
    return ESP_OK;
}

static esp_err_t get_device_id(char out[DEVICE_ID_MAX_LEN])
{
    memset(out, 0, DEVICE_ID_MAX_LEN);
    return device_identity_get_device_id(out);
}

static esp_err_t derive_session_key(uint64_t counter,
                                    const uint8_t random[OTA_SECURE_RANDOM_LEN],
                                    uint8_t out[OTA_SEC_SESSION_KEY_LEN])
{
    ESP_RETURN_ON_ERROR(device_credentials_verify_ota_server_certificate(), TAG, "OTA server CA verification failed");

    uint8_t shared[DEVICE_CREDENTIAL_ECDH_SECRET_LEN];
    ESP_RETURN_ON_ERROR(device_credentials_derive_ota_ecdh_secret(shared), TAG, "OTA ECDH failed");

    char device_id[DEVICE_ID_MAX_LEN];
    ESP_RETURN_ON_ERROR(get_device_id(device_id), TAG, "device id unavailable");

    uint8_t counter_be[8];
    put_u64_be(counter_be, counter);
    uint8_t material[160];
    size_t pos = 0;
    const size_t domain_len = strlen(SESSION_DOMAIN);
    const size_t device_len = strlen(device_id);
    memcpy(material + pos, SESSION_DOMAIN, domain_len); pos += domain_len;
    memcpy(material + pos, device_id, device_len); pos += device_len;
    memcpy(material + pos, counter_be, sizeof(counter_be)); pos += sizeof(counter_be);
    memcpy(material + pos, random, OTA_SECURE_RANDOM_LEN); pos += OTA_SECURE_RANDOM_LEN;

    const esp_err_t err = hmac_sha256(shared, sizeof(shared), material, pos, out);
    memset(shared, 0, sizeof(shared));
    memset(material, 0, sizeof(material));
    return err;
}

static esp_err_t build_auth_material(const char *domain,
                                     uint64_t counter,
                                     const uint8_t random[OTA_SECURE_RANDOM_LEN],
                                     uint32_t crc,
                                     uint8_t *out, size_t out_size, size_t *written)
{
    if (domain == NULL || random == NULL || out == NULL || written == NULL) return ESP_ERR_INVALID_ARG;
    char device_id[DEVICE_ID_MAX_LEN];
    ESP_RETURN_ON_ERROR(get_device_id(device_id), TAG, "device id unavailable");

    const size_t domain_len = strlen(domain);
    const size_t device_len = strlen(device_id);
    const size_t needed = domain_len + device_len + 8 + OTA_SECURE_RANDOM_LEN + 4;
    if (needed > out_size) return ESP_ERR_INVALID_SIZE;

    uint8_t counter_be[8];
    uint8_t crc_be[4];
    put_u64_be(counter_be, counter);
    put_u32_be(crc_be, crc);

    size_t pos = 0;
    memcpy(out + pos, domain, domain_len); pos += domain_len;
    memcpy(out + pos, device_id, device_len); pos += device_len;
    memcpy(out + pos, counter_be, sizeof(counter_be)); pos += sizeof(counter_be);
    memcpy(out + pos, random, OTA_SECURE_RANDOM_LEN); pos += OTA_SECURE_RANDOM_LEN;
    memcpy(out + pos, crc_be, sizeof(crc_be)); pos += sizeof(crc_be);
    *written = pos;
    return ESP_OK;
}

static esp_err_t persist_challenge(uint64_t counter,
                                   const uint8_t random[OTA_SECURE_RANDOM_LEN],
                                   uint32_t crc)
{
    challenge_nvs_t record = {
        .magic = OTA_SEC_CHALLENGE_MAGIC,
        .counter = counter,
        .crc32 = crc,
    };
    memcpy(record.random, random, OTA_SECURE_RANDOM_LEN);

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(OTA_SEC_NAMESPACE, NVS_READWRITE, &handle), TAG, "open NVS challenge failed");
    esp_err_t err = nvs_set_blob(handle, OTA_SEC_CHALLENGE_KEY, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    memset(&record, 0, sizeof(record));
    return err;
}

static esp_err_t persist_provisioning(const ota_secure_provisioning_t *config)
{
    provision_nvs_t record = {
        .magic = OTA_SEC_PROVISION_MAGIC,
        .config = *config,
    };
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(OTA_SEC_NAMESPACE, NVS_READWRITE, &handle), TAG, "open NVS provisioning failed");
    esp_err_t err = nvs_set_blob(handle, OTA_SEC_PROVISION_KEY, &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    memset(&record, 0, sizeof(record));
    return err;
}

esp_err_t ota_secure_session_init(void)
{
    s_challenge_verified = false;
    memset(s_session_key, 0, sizeof(s_session_key));
    memset(s_random, 0, sizeof(s_random));
    s_counter = 0;
    s_crc32 = 0;

    ota_secure_provisioning_t config;
    const esp_err_t err = ota_secure_session_load_provisioning(&config);
    s_provisioned = err == ESP_OK;
    memset(&config, 0, sizeof(config));
    return ESP_OK;
}

esp_err_t ota_secure_session_accept_challenge(const char *payload,
                                              char ack_out[OTA_SECURE_ACK_MAX_LEN])
{
    if (payload == NULL || ack_out == NULL) return ESP_ERR_INVALID_ARG;
    ack_out[0] = '\0';
    if (strncmp(payload, OTA_SEC_CHALLENGE_PREFIX, strlen(OTA_SEC_CHALLENGE_PREFIX)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char work[OTA_SECURE_PROVISION_MAX_WIRE_LEN + 1];
    const size_t payload_len = strlen(payload);
    if (payload_len > OTA_SECURE_PROVISION_MAX_WIRE_LEN) return ESP_ERR_INVALID_SIZE;
    memcpy(work, payload, payload_len + 1);

    char *save = NULL;
    char *prefix = strtok_r(work, "|", &save);
    char *counter_text = strtok_r(NULL, "|", &save);
    char *random_text = strtok_r(NULL, "|", &save);
    char *crc_text = strtok_r(NULL, "|", &save);
    char *mac_text = strtok_r(NULL, "|", &save);
    if (prefix == NULL || strcmp(prefix, "A1") != 0 || counter_text == NULL || random_text == NULL ||
        crc_text == NULL || mac_text == NULL || strtok_r(NULL, "|", &save) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *counter_end = NULL;
    unsigned long long parsed_counter = strtoull(counter_text, &counter_end, 10);
    if (counter_end == counter_text || *counter_end != '\0') return ESP_ERR_INVALID_ARG;
    const uint64_t counter = (uint64_t)parsed_counter;

    uint64_t hello_counter = 0;
    ESP_RETURN_ON_ERROR(device_identity_get_enrollment_counter(&hello_counter), TAG, "cannot read HELLO counter");
    if (counter == 0 || counter != hello_counter) {
        ESP_LOGE(TAG, "challenge counter rejected received=%llu expected_current_hello=%llu",
                 (unsigned long long)counter, (unsigned long long)hello_counter);
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t random[OTA_SECURE_RANDOM_LEN];
    ESP_RETURN_ON_ERROR(base64url_decode_exact(random_text, OTA_SEC_RANDOM_B64URL_LEN,
                                               random, sizeof(random)), TAG, "challenge random invalid");

    uint8_t crc_input[8 + OTA_SECURE_RANDOM_LEN];
    put_u64_be(crc_input, counter);
    memcpy(crc_input + 8, random, sizeof(random));
    const uint32_t calculated_crc = crc32_ieee(crc_input, sizeof(crc_input));
    uint32_t received_crc = 0;
    ESP_RETURN_ON_ERROR(parse_hex_u32(crc_text, &received_crc), TAG, "challenge CRC invalid");
    if (received_crc != calculated_crc) {
        ESP_LOGE(TAG, "challenge CRC mismatch received=%08lx calculated=%08lx",
                 (unsigned long)received_crc, (unsigned long)calculated_crc);
        memset(random, 0, sizeof(random));
        return ESP_ERR_INVALID_CRC;
    }

    uint8_t session_key[OTA_SEC_SESSION_KEY_LEN];
    ESP_RETURN_ON_ERROR(derive_session_key(counter, random, session_key), TAG, "challenge session key derivation failed");

    uint8_t auth_material[192];
    size_t auth_len = 0;
    ESP_RETURN_ON_ERROR(build_auth_material(CHALLENGE_DOMAIN, counter, random, received_crc,
                                            auth_material, sizeof(auth_material), &auth_len),
                        TAG, "challenge canonicalization failed");
    uint8_t expected_mac[OTA_SECURE_AUTH_TAG_LEN];
    ESP_RETURN_ON_ERROR(hmac_sha256(session_key, sizeof(session_key), auth_material, auth_len, expected_mac),
                        TAG, "challenge HMAC calculation failed");
    uint8_t received_mac[OTA_SECURE_AUTH_TAG_LEN];
    ESP_RETURN_ON_ERROR(base64url_decode_exact(mac_text, OTA_SEC_HMAC_B64URL_LEN,
                                               received_mac, sizeof(received_mac)),
                        TAG, "challenge HMAC encoding invalid");
    if (!constant_time_equal(expected_mac, received_mac, sizeof(expected_mac))) {
        ESP_LOGE(TAG, "challenge authentication failed: OTA sender does not own CA-certified server key");
        memset(session_key, 0, sizeof(session_key));
        memset(expected_mac, 0, sizeof(expected_mac));
        memset(received_mac, 0, sizeof(received_mac));
        memset(random, 0, sizeof(random));
        memset(auth_material, 0, sizeof(auth_material));
        return ESP_ERR_INVALID_CRC;
    }

    ESP_RETURN_ON_ERROR(persist_challenge(counter, random, received_crc), TAG, "challenge NVS persistence failed");

    s_counter = counter;
    s_crc32 = received_crc;
    memcpy(s_random, random, sizeof(s_random));
    memcpy(s_session_key, session_key, sizeof(s_session_key));
    s_challenge_verified = true;

    size_t response_material_len = 0;
    ESP_RETURN_ON_ERROR(build_auth_material(RESPONSE_DOMAIN, counter, random, received_crc,
                                            auth_material, sizeof(auth_material), &response_material_len),
                        TAG, "response canonicalization failed");
    uint8_t response_mac[OTA_SECURE_AUTH_TAG_LEN];
    ESP_RETURN_ON_ERROR(hmac_sha256(session_key, sizeof(session_key), auth_material,
                                    response_material_len, response_mac),
                        TAG, "response HMAC failed");
    char response_b64[OTA_SEC_HMAC_B64URL_LEN + 1];
    ESP_RETURN_ON_ERROR(base64url_encode(response_mac, sizeof(response_mac),
                                         response_b64, sizeof(response_b64)),
                        TAG, "response HMAC encoding failed");

    const int n = snprintf(ack_out, OTA_SECURE_ACK_MAX_LEN, "R1|%llu|%s",
                           (unsigned long long)counter, response_b64);
    if (n <= 0 || n >= OTA_SECURE_ACK_MAX_LEN) return ESP_ERR_INVALID_SIZE;

    ESP_LOGI(TAG, "challenge VERIFIED: CA=OK ECDH=OK counter=%llu CRC=OK HMAC=OK NVS=OK",
             (unsigned long long)counter);

    memset(session_key, 0, sizeof(session_key));
    memset(expected_mac, 0, sizeof(expected_mac));
    memset(received_mac, 0, sizeof(received_mac));
    memset(response_mac, 0, sizeof(response_mac));
    memset(random, 0, sizeof(random));
    memset(auth_material, 0, sizeof(auth_material));
    return ESP_OK;
}

static esp_err_t derive_provision_key_nonce_aad(uint8_t key[32], uint8_t nonce[OTA_SEC_GCM_NONCE_LEN],
                                                uint8_t *aad, size_t aad_size, size_t *aad_len)
{
    if (!s_challenge_verified) return ESP_ERR_INVALID_STATE;

    ESP_RETURN_ON_ERROR(hmac_sha256(s_session_key, sizeof(s_session_key),
                                    (const uint8_t *)PROVISION_KEY_DOMAIN, strlen(PROVISION_KEY_DOMAIN), key),
                        TAG, "provision key derivation failed");

    uint8_t nonce_material[128];
    size_t pos = 0;
    const size_t nonce_domain_len = strlen(PROVISION_NONCE_DOMAIN);
    memcpy(nonce_material + pos, PROVISION_NONCE_DOMAIN, nonce_domain_len); pos += nonce_domain_len;
    put_u64_be(nonce_material + pos, s_counter); pos += 8;
    memcpy(nonce_material + pos, s_random, sizeof(s_random)); pos += sizeof(s_random);
    uint8_t nonce_hash[32];
    ESP_RETURN_ON_ERROR(hmac_sha256(s_session_key, sizeof(s_session_key), nonce_material, pos, nonce_hash),
                        TAG, "provision nonce derivation failed");
    memcpy(nonce, nonce_hash, OTA_SEC_GCM_NONCE_LEN);

    char device_id[DEVICE_ID_MAX_LEN];
    ESP_RETURN_ON_ERROR(get_device_id(device_id), TAG, "device id unavailable");
    const size_t aad_domain_len = strlen(PROVISION_AAD_DOMAIN);
    const size_t device_len = strlen(device_id);
    const size_t needed = aad_domain_len + device_len + 8 + sizeof(s_random) + 4;
    if (needed > aad_size) return ESP_ERR_INVALID_SIZE;
    pos = 0;
    memcpy(aad + pos, PROVISION_AAD_DOMAIN, aad_domain_len); pos += aad_domain_len;
    memcpy(aad + pos, device_id, device_len); pos += device_len;
    put_u64_be(aad + pos, s_counter); pos += 8;
    memcpy(aad + pos, s_random, sizeof(s_random)); pos += sizeof(s_random);
    put_u32_be(aad + pos, s_crc32); pos += 4;
    *aad_len = pos;

    memset(nonce_material, 0, sizeof(nonce_material));
    memset(nonce_hash, 0, sizeof(nonce_hash));
    return ESP_OK;
}

static esp_err_t parse_provision_plaintext(const uint8_t *plain, size_t len,
                                           ota_secure_provisioning_t *config)
{
    if (plain == NULL || config == NULL || len < 9) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));

    size_t pos = 0;
    const uint8_t version = plain[pos++];
    if (version != 1) return ESP_ERR_NOT_SUPPORTED;
    config->wifi_security = plain[pos++];
    config->wifi_channel = plain[pos++];
    const uint8_t ssid_len = plain[pos++];
    const uint8_t password_len = plain[pos++];
    const uint8_t host_type = plain[pos++];
    const uint8_t host_len = plain[pos++];

    if (ssid_len == 0 || ssid_len > OTA_SECURE_SSID_MAX_LEN ||
        password_len > OTA_SECURE_PASSWORD_MAX_LEN ||
        host_len == 0 || host_len > OTA_SECURE_HOST_MAX_LEN) return ESP_ERR_INVALID_SIZE;
    if (config->wifi_channel != 0 && (config->wifi_channel < 1 || config->wifi_channel > 14)) return ESP_ERR_INVALID_ARG;
    if (pos + ssid_len + password_len + host_len + 2 != len) return ESP_ERR_INVALID_SIZE;

    memcpy(config->ssid, plain + pos, ssid_len); pos += ssid_len;
    memcpy(config->password, plain + pos, password_len); pos += password_len;

    if (host_type == 1) {
        if (host_len != 4) return ESP_ERR_INVALID_SIZE;
        snprintf(config->ota_host, sizeof(config->ota_host), "%u.%u.%u.%u",
                 plain[pos], plain[pos + 1], plain[pos + 2], plain[pos + 3]);
        pos += 4;
    } else if (host_type == 0) {
        memcpy(config->ota_host, plain + pos, host_len);
        pos += host_len;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    config->ota_port = get_u16_be(plain + pos);
    if (config->ota_port == 0) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

esp_err_t ota_secure_session_accept_provisioning(const char *payload)
{
    if (payload == NULL || strncmp(payload, OTA_SEC_PROVISION_PREFIX, strlen(OTA_SEC_PROVISION_PREFIX)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_challenge_verified) {
        ESP_LOGE(TAG, "provisioning rejected: no verified challenge session");
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(payload) > OTA_SECURE_PROVISION_MAX_WIRE_LEN) return ESP_ERR_INVALID_SIZE;

    uint8_t binary[OTA_SEC_MAX_BINARY_WIRE];
    size_t binary_len = 0;
    ESP_RETURN_ON_ERROR(base64url_decode_variable(payload + 3, binary, sizeof(binary), &binary_len),
                        TAG, "provisioning base64url invalid");
    if (binary_len <= OTA_SEC_GCM_TAG_LEN) return ESP_ERR_INVALID_SIZE;

    const size_t ciphertext_len = binary_len - OTA_SEC_GCM_TAG_LEN;
    if (ciphertext_len > OTA_SEC_MAX_PLAINTEXT) return ESP_ERR_INVALID_SIZE;

    uint8_t key[32];
    uint8_t nonce[OTA_SEC_GCM_NONCE_LEN];
    uint8_t aad[160];
    size_t aad_len = 0;
    ESP_RETURN_ON_ERROR(derive_provision_key_nonce_aad(key, nonce, aad, sizeof(aad), &aad_len),
                        TAG, "provisioning crypto context failed");

    uint8_t plain[OTA_SEC_MAX_PLAINTEXT];
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret == 0) {
        ret = mbedtls_gcm_auth_decrypt(&gcm,
                                       ciphertext_len,
                                       nonce, sizeof(nonce),
                                       aad, aad_len,
                                       binary + ciphertext_len, OTA_SEC_GCM_TAG_LEN,
                                       binary,
                                       plain);
    }
    mbedtls_gcm_free(&gcm);
    if (ret != 0) {
        ESP_LOGE(TAG, "provisioning rejected: AES-GCM authentication failed");
        memset(binary, 0, sizeof(binary));
        memset(plain, 0, sizeof(plain));
        memset(key, 0, sizeof(key));
        return ESP_ERR_INVALID_CRC;
    }

    ota_secure_provisioning_t config;
    esp_err_t err = parse_provision_plaintext(plain, ciphertext_len, &config);
    if (err == ESP_OK) err = persist_provisioning(&config);
    if (err == ESP_OK) {
        s_provisioned = true;
        ESP_LOGI(TAG,
                 "provisioning VERIFIED+STORED: ssid=%s ota=%s:%u security=%u channel=%u password_len=%u",
                 config.ssid, config.ota_host, (unsigned)config.ota_port,
                 (unsigned)config.wifi_security, (unsigned)config.wifi_channel,
                 (unsigned)strlen(config.password));
        ota_secure_session_clear_pending();
    }

    memset(&config, 0, sizeof(config));
    memset(binary, 0, sizeof(binary));
    memset(plain, 0, sizeof(plain));
    memset(key, 0, sizeof(key));
    memset(nonce, 0, sizeof(nonce));
    memset(aad, 0, sizeof(aad));
    return err;
}

bool ota_secure_session_has_verified_challenge(void)
{
    return s_challenge_verified;
}

bool ota_secure_session_is_provisioned(void)
{
    return s_provisioned;
}

esp_err_t ota_secure_session_load_provisioning(ota_secure_provisioning_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_SEC_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    provision_nvs_t record;
    size_t size = sizeof(record);
    err = nvs_get_blob(handle, OTA_SEC_PROVISION_KEY, &record, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(record) || record.magic != OTA_SEC_PROVISION_MAGIC) {
        memset(&record, 0, sizeof(record));
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    *out = record.config;
    memset(&record, 0, sizeof(record));
    return ESP_OK;
}

void ota_secure_session_clear_pending(void)
{
    s_challenge_verified = false;
    s_counter = 0;
    s_crc32 = 0;
    memset(s_random, 0, sizeof(s_random));
    memset(s_session_key, 0, sizeof(s_session_key));
}
