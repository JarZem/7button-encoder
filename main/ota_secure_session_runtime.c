/*
 * Runtime extension of ota_secure_session.c.
 *
 * The secure-session implementation deliberately keeps protocol state private
 * to one translation unit. Include it here (and compile this file instead of
 * ota_secure_session.c directly) so the explicit user-requested reprovisioning
 * transition can reset only the protocol state while retaining prov_v2.
 */
#include "ota_secure_session.c"

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
