#include "device_enrollment.h"

#include <string.h>

#include "esp_check.h"

static const char *TAG = "device_enrollment";

typedef struct {
    uint8_t *out;
    size_t out_len;
    size_t pos;
} encoder_t;

static esp_err_t put_bytes(encoder_t *enc, const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(enc != NULL && data != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid canonical input");
    ESP_RETURN_ON_FALSE(enc->pos + len <= enc->out_len, ESP_ERR_INVALID_SIZE, TAG, "canonical buffer too small");
    memcpy(enc->out + enc->pos, data, len);
    enc->pos += len;
    return ESP_OK;
}

static esp_err_t put_u8(encoder_t *enc, uint8_t value)
{
    return put_bytes(enc, &value, sizeof(value));
}

static esp_err_t put_u16_be(encoder_t *enc, uint16_t value)
{
    const uint8_t data[2] = {
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xff),
    };
    return put_bytes(enc, data, sizeof(data));
}

static esp_err_t put_u64_be(encoder_t *enc, uint64_t value)
{
    uint8_t data[8];
    for (size_t i = 0; i < sizeof(data); ++i) {
        data[sizeof(data) - 1 - i] = (uint8_t)((value >> (i * 8)) & 0xff);
    }
    return put_bytes(enc, data, sizeof(data));
}

static esp_err_t put_len_bytes(encoder_t *enc, const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(len <= UINT16_MAX, ESP_ERR_INVALID_SIZE, TAG, "canonical field too large");
    ESP_RETURN_ON_ERROR(put_u16_be(enc, (uint16_t)len), TAG, "canonical length failed");
    if (len == 0) {
        return ESP_OK;
    }
    return put_bytes(enc, data, len);
}

static esp_err_t put_string(encoder_t *enc, const char *value)
{
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "missing canonical string");
    return put_len_bytes(enc, value, strlen(value));
}

esp_err_t device_enrollment_encode_canonical(const device_enrollment_fields_t *fields,
                                             uint8_t *out,
                                             size_t out_len,
                                             size_t *written)
{
    ESP_RETURN_ON_FALSE(fields != NULL && out != NULL && written != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid canonical arguments");
    ESP_RETURN_ON_FALSE(fields->challenge != NULL &&
                            fields->challenge_len == DEVICE_AUTH_CHALLENGE_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "invalid challenge");
    ESP_RETURN_ON_FALSE(fields->device_enc_public_key != NULL &&
                            fields->device_enc_public_key_len == DEVICE_ENC_PUBLIC_KEY_LEN,
                        ESP_ERR_INVALID_ARG, TAG, "invalid public key");
    ESP_RETURN_ON_FALSE(fields->device_enc_public_key[0] == 0x04,
                        ESP_ERR_INVALID_ARG, TAG, "public key must be uncompressed P-256");

    encoder_t enc = {
        .out = out,
        .out_len = out_len,
        .pos = 0,
    };

    ESP_RETURN_ON_ERROR(put_string(&enc, DEVICE_ENROLLMENT_CANONICAL_DOMAIN), TAG, "canonical domain failed");
    ESP_RETURN_ON_ERROR(put_u8(&enc, fields->protocol_version), TAG, "canonical protocol failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->message_id), TAG, "canonical message_id failed");
    ESP_RETURN_ON_ERROR(put_len_bytes(&enc, fields->challenge, fields->challenge_len), TAG, "canonical challenge failed");
    ESP_RETURN_ON_ERROR(put_u64_be(&enc, fields->enrollment_counter), TAG, "canonical counter failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->device_id), TAG, "canonical device_id failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->zigbee_ieee), TAG, "canonical zigbee_ieee failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->ota_ecosystem), TAG, "canonical ecosystem failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->device_model), TAG, "canonical model failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->product_role), TAG, "canonical role failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->hardware_revision), TAG, "canonical hw revision failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->chip_family), TAG, "canonical chip family failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->flash_size), TAG, "canonical flash size failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->firmware_version), TAG, "canonical fw version failed");
    ESP_RETURN_ON_ERROR(put_string(&enc, fields->firmware_channel), TAG, "canonical fw channel failed");
    ESP_RETURN_ON_ERROR(put_u16_be(&enc, fields->device_enc_key_id), TAG, "canonical key id failed");
    ESP_RETURN_ON_ERROR(put_len_bytes(&enc,
                                      fields->device_enc_public_key,
                                      fields->device_enc_public_key_len),
                        TAG, "canonical public key failed");

    *written = enc.pos;
    return ESP_OK;
}

