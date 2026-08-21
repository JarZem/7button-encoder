#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_CHECK_COMPLETION_MAX_LEN 16

/*
 * Validate C|version|code|random|MAC using the provisioning session context
 * persisted by ota_secure_session. On success, derive the one-time Bearer
 * token, persist the active grant random and return a legacy C|token request
 * for ota_service so the existing tested HTTPS downloader can be reused.
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
