/*
 * Runtime extension of ota_secure_session.c.
 *
 * The secure-session implementation deliberately keeps protocol state private
 * to one translation unit. Compile it through this wrapper so the public
 * is_provisioned() helper can trigger explicit user-requested reprovisioning
 * while keeping the last valid provisioning data intact.
 */
#define ota_secure_session_is_provisioned ota_secure_session_is_provisioned_internal
#include "ota_secure_session.c"
#undef ota_secure_session_is_provisioned

#include "zigbee_ota_control.h"

bool ota_secure_session_is_provisioned(void)
{
    if (s_state == OTA_SEC_STATE_PROVISIONED && zigbee_ota_control_is_enabled()) {
        ota_secure_session_begin_reprovisioning();
        return false;
    }
    return s_state == OTA_SEC_STATE_PROVISIONED;
}
