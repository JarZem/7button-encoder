/*
 * Runtime extension of ota_secure_session.c.
 *
 * The secure-session implementation deliberately keeps protocol state private
 * to one translation unit. Compile it through this wrapper so an explicit
 * user-requested reprovisioning run can reset only protocol state while
 * retaining the last valid prov_v2 data.
 */
#define ota_secure_session_is_provisioned ota_secure_session_is_provisioned_internal
#include "ota_secure_session.c"
#undef ota_secure_session_is_provisioned

#include "zigbee_ota_control.h"

void ota_secure_session_begin_reprovisioning(void)
{
    ESP_LOGW(TAG, "user reprovision requested state=%s -> IDLE; stored prov_v2 retained",
             ota_secure_session_state_name());
    s_state = OTA_SEC_STATE_IDLE;
    s_counter = 0;
    memset(s_random, 0, sizeof(s_random));
    memset(s_session_key, 0, sizeof(s_session_key));
    (void)persist_state();
}

bool ota_secure_session_is_provisioned(void)
{
    if (s_state == OTA_SEC_STATE_PROVISIONED && zigbee_ota_control_is_enabled()) {
        ota_secure_session_begin_reprovisioning();
        return false;
    }
    return s_state == OTA_SEC_STATE_PROVISIONED;
}
