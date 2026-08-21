#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_CHECK_COMPLETION_MAX_LEN 16

/* Copy the last successfully persisted provisioning counter+random into the
 * durable OTA CHECK context. Safe to call after a successful P frame. */
esp_err_t ota_check_auth_snapshot_provisioning_context(void);

/*
 * Validate C|version|code|random|MAC using the durable provisioning context.
 * On success derive the one-time Bearer token, persist the active grant random
 * and return C|token for the already tested ota_service HTTPS downloader.
 */
esp_err_t ota_check_auth_prepare_request(const char *payload, size_t payload_len,
                                         char *out_request, size_t out_size,
                                         size_t *out_len);

/* Build F|random for a completed, fully downloaded/verified firmware image. */
esp_err_t ota_check_auth_build_completion(char out[OTA_CHECK_COMPLETION_MAX_LEN]);

/* Forget the active grant after its F completion was queued successfully. */
void ota_check_auth_clear_active_grant(void);

#ifdef __cplusplus
}
#endif
