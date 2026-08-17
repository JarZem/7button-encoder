#include "ota_wifi.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_coexist.h"
#include "esp_ieee802154.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_phy_init.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"

#define OTA_WIFI_CONNECTED_BIT BIT0
#define OTA_WIFI_FAIL_BIT      BIT1
#define OTA_WIFI_SCAN_START_BIT BIT2
#define OTA_WIFI_MAX_RETRIES   8
#define OTA_WIFI_TIMEOUT_MS    45000
#define OTA_WIFI_SCAN_START_TIMEOUT_MS 5000
#define OTA_WIFI_TARGET_BSSID0 0x12
#define OTA_WIFI_TARGET_BSSID1 0x5a
#define OTA_WIFI_TARGET_BSSID2 0x95
#define OTA_WIFI_TARGET_BSSID3 0x22
#define OTA_WIFI_TARGET_BSSID4 0xd4
#define OTA_WIFI_TARGET_BSSID5 0x36
#define OTA_WIFI_MAX_SCAN_RECORDS 32

static const char *TAG = "ota_wifi";

static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_wifi_netif;
static esp_event_handler_instance_t s_wifi_any_id_handler;
static esp_event_handler_instance_t s_ip_got_ip_handler;
static int s_retry_count;
static bool s_started;
static bool s_disconnect_requested;
static bool s_coex_prepared;
static bool s_i154_quieted;
static bool s_i154_disabled_for_wifi;
static wifi_ap_record_t s_scan_records[OTA_WIFI_MAX_SCAN_RECORDS];

static const uint8_t s_target_bssid[6] = {
    OTA_WIFI_TARGET_BSSID0, OTA_WIFI_TARGET_BSSID1, OTA_WIFI_TARGET_BSSID2,
    OTA_WIFI_TARGET_BSSID3, OTA_WIFI_TARGET_BSSID4, OTA_WIFI_TARGET_BSSID5,
};

static const char *i154_state_name(esp_ieee802154_state_t state)
{
    switch (state) {
    case ESP_IEEE802154_RADIO_DISABLE:
        return "DISABLE";
    case ESP_IEEE802154_RADIO_IDLE:
        return "IDLE";
    case ESP_IEEE802154_RADIO_SLEEP:
        return "SLEEP";
    case ESP_IEEE802154_RADIO_RECEIVE:
        return "RECEIVE";
    case ESP_IEEE802154_RADIO_TRANSMIT:
        return "TRANSMIT";
    default:
        return "UNKNOWN";
    }
}

static void log_i154_state(const char *prefix)
{
    const esp_ieee802154_state_t state = esp_ieee802154_get_state();
    const uint8_t channel = esp_ieee802154_get_channel();
    const bool rx_when_idle = esp_ieee802154_get_rx_when_idle();
    ESP_LOGI(TAG, "%s IEEE802.15.4 state=%s(%d) channel=%u rx_when_idle=%d",
             prefix, i154_state_name(state), state, channel, rx_when_idle);
}

static void ota_wifi_prepare_radio_for_wifi(const char *context)
{
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
    const esp_ieee802154_state_t state = esp_ieee802154_get_state();
    s_i154_quieted = false;
    s_i154_disabled_for_wifi = false;
    ESP_LOGW(TAG, "DIAG: %s: preparing shared ESP32-C6 RF for WiFi while Zigbee is running",
             context);
    log_i154_state("DIAG: before WiFi:");

    if (state == ESP_IEEE802154_RADIO_DISABLE) {
        ESP_LOGW(TAG, "DIAG: IEEE802.15.4 radio is disabled; leaving coex/154 untouched for WiFi-only test");
        return;
    }

    esp_err_t err = esp_coex_wifi_i154_enable();
    ESP_LOGI(TAG, "DIAG: esp_coex_wifi_i154_enable() -> %s",
             esp_err_to_name(err));

    esp_coex_ieee802154_txrx_pti_set(IEEE802154_LOW);
    esp_coex_ieee802154_ack_pti_set(IEEE802154_LOW);
    ESP_LOGI(TAG, "DIAG: IEEE802.15.4 coex PTI set to LOW for WiFi OTA window");

    err = esp_ieee802154_set_rx_when_idle(false);
    ESP_LOGI(TAG, "DIAG: esp_ieee802154_set_rx_when_idle(false) -> %s",
             esp_err_to_name(err));

    err = esp_ieee802154_sleep();
    ESP_LOGI(TAG, "DIAG: esp_ieee802154_sleep() -> %s",
             esp_err_to_name(err));

    vTaskDelay(pdMS_TO_TICKS(50));
    esp_ieee802154_state_t quiet_state = esp_ieee802154_get_state();
    if (quiet_state != ESP_IEEE802154_RADIO_DISABLE) {
        ESP_LOGW(TAG, "DIAG: IEEE802.15.4 is %s after sleep; disabling 802.15.4 subsystem for OTA WiFi",
                 i154_state_name(quiet_state));
        err = esp_ieee802154_disable();
        ESP_LOGW(TAG, "DIAG: esp_ieee802154_disable() -> %s",
                 esp_err_to_name(err));
        s_i154_disabled_for_wifi = (err == ESP_OK);
    }

    esp_coex_ieee802154_status_disable();
    ESP_LOGI(TAG, "DIAG: esp_coex_ieee802154_status_disable()");
    s_i154_quieted = true;
    log_i154_state("DIAG: after 154 quiet:");
#else
    ESP_LOGW(TAG, "DIAG: %s: WiFi/IEEE802.15.4 coexistence API is not enabled in sdkconfig",
             context);
#endif
}

static void ota_wifi_restore_radio_after_wifi(const char *context)
{
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
    if (!s_i154_quieted) {
        ESP_LOGI(TAG, "DIAG: %s: IEEE802.15.4 was not quieted; no coex restore needed",
                 context);
        return;
    }

    ESP_LOGW(TAG, "DIAG: %s: restoring IEEE802.15.4 coexistence after WiFi OTA window",
             context);

    esp_coex_ieee802154_status_enable();
    esp_coex_ieee802154_txrx_pti_set(IEEE802154_LOW);
    esp_coex_ieee802154_ack_pti_set(IEEE802154_HIGH);

    if (s_i154_disabled_for_wifi) {
        esp_err_t enable_err = esp_ieee802154_enable();
        ESP_LOGW(TAG, "DIAG: esp_ieee802154_enable() after OTA WiFi -> %s",
                 esp_err_to_name(enable_err));
        s_i154_disabled_for_wifi = false;
    }

    esp_err_t err = esp_ieee802154_set_rx_when_idle(true);
    ESP_LOGI(TAG, "DIAG: esp_ieee802154_set_rx_when_idle(true) -> %s",
             esp_err_to_name(err));
    log_i154_state("DIAG: after WiFi:");
    s_i154_quieted = false;
#else
    (void)context;
#endif
}

static void format_bssid(const uint8_t bssid[6], char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    if (bssid == NULL) {
        strlcpy(buffer, "null", buffer_len);
        return;
    }

    snprintf(buffer, buffer_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

static const char *authmode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa/wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2-enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2/wpa3";
    case WIFI_AUTH_WAPI_PSK:
        return "wapi";
    case WIFI_AUTH_OWE:
        return "owe";
    case WIFI_AUTH_WPA3_ENT_192:
        return "wpa3-enterprise-192";
    default:
        return "unknown";
    }
}

static const char *sae_pwe_name(wifi_sae_pwe_method_t method)
{
    switch (method) {
    case WPA3_SAE_PWE_UNSPECIFIED:
        return "WPA3_SAE_PWE_UNSPECIFIED";
    case WPA3_SAE_PWE_HUNT_AND_PECK:
        return "WPA3_SAE_PWE_HUNT_AND_PECK";
    case WPA3_SAE_PWE_HASH_TO_ELEMENT:
        return "WPA3_SAE_PWE_HASH_TO_ELEMENT";
    case WPA3_SAE_PWE_BOTH:
        return "WPA3_SAE_PWE_BOTH";
    default:
        return "unknown";
    }
}

static const char *country_policy_name(wifi_country_policy_t policy)
{
    switch (policy) {
    case WIFI_COUNTRY_POLICY_AUTO:
        return "AUTO";
    case WIFI_COUNTRY_POLICY_MANUAL:
        return "MANUAL";
    default:
        return "unknown";
    }
}

static const char *disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
        return "WIFI_REASON_UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE:
        return "WIFI_REASON_AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:
        return "WIFI_REASON_AUTH_LEAVE";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "WIFI_REASON_ASSOC_TOOMANY";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA:
        return "WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA";
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA:
        return "WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA";
    case WIFI_REASON_ASSOC_LEAVE:
        return "WIFI_REASON_ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "WIFI_REASON_ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
        return "WIFI_REASON_DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
        return "WIFI_REASON_DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_BSS_TRANSITION_DISASSOC:
        return "WIFI_REASON_BSS_TRANSITION_DISASSOC";
    case WIFI_REASON_IE_INVALID:
        return "WIFI_REASON_IE_INVALID";
    case WIFI_REASON_MIC_FAILURE:
        return "WIFI_REASON_MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "WIFI_REASON_IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "WIFI_REASON_GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return "WIFI_REASON_PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID:
        return "WIFI_REASON_AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return "WIFI_REASON_UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
        return "WIFI_REASON_INVALID_RSN_IE_CAP";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "WIFI_REASON_802_1X_AUTH_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return "WIFI_REASON_CIPHER_SUITE_REJECTED";
    case WIFI_REASON_TDLS_PEER_UNREACHABLE:
        return "WIFI_REASON_TDLS_PEER_UNREACHABLE";
    case WIFI_REASON_TDLS_UNSPECIFIED:
        return "WIFI_REASON_TDLS_UNSPECIFIED";
    case WIFI_REASON_SSP_REQUESTED_DISASSOC:
        return "WIFI_REASON_SSP_REQUESTED_DISASSOC";
    case WIFI_REASON_NO_SSP_ROAMING_AGREEMENT:
        return "WIFI_REASON_NO_SSP_ROAMING_AGREEMENT";
    case WIFI_REASON_BAD_CIPHER_OR_AKM:
        return "WIFI_REASON_BAD_CIPHER_OR_AKM";
    case WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION:
        return "WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION";
    case WIFI_REASON_SERVICE_CHANGE_PERCLUDES_TS:
        return "WIFI_REASON_SERVICE_CHANGE_PERCLUDES_TS";
    case WIFI_REASON_UNSPECIFIED_QOS:
        return "WIFI_REASON_UNSPECIFIED_QOS";
    case WIFI_REASON_NOT_ENOUGH_BANDWIDTH:
        return "WIFI_REASON_NOT_ENOUGH_BANDWIDTH";
    case WIFI_REASON_MISSING_ACKS:
        return "WIFI_REASON_MISSING_ACKS";
    case WIFI_REASON_EXCEEDED_TXOP:
        return "WIFI_REASON_EXCEEDED_TXOP";
    case WIFI_REASON_STA_LEAVING:
        return "WIFI_REASON_STA_LEAVING";
    case WIFI_REASON_END_BA:
        return "WIFI_REASON_END_BA";
    case WIFI_REASON_UNKNOWN_BA:
        return "WIFI_REASON_UNKNOWN_BA";
    case WIFI_REASON_TIMEOUT:
        return "WIFI_REASON_TIMEOUT";
    case WIFI_REASON_PEER_INITIATED:
        return "WIFI_REASON_PEER_INITIATED";
    case WIFI_REASON_AP_INITIATED:
        return "WIFI_REASON_AP_INITIATED";
    case WIFI_REASON_INVALID_FT_ACTION_FRAME_COUNT:
        return "WIFI_REASON_INVALID_FT_ACTION_FRAME_COUNT";
    case WIFI_REASON_INVALID_PMKID:
        return "WIFI_REASON_INVALID_PMKID";
    case WIFI_REASON_INVALID_MDE:
        return "WIFI_REASON_INVALID_MDE";
    case WIFI_REASON_INVALID_FTE:
        return "WIFI_REASON_INVALID_FTE";
    case WIFI_REASON_TRANSMISSION_LINK_ESTABLISH_FAILED:
        return "WIFI_REASON_TRANSMISSION_LINK_ESTABLISH_FAILED";
    case WIFI_REASON_ALTERATIVE_CHANNEL_OCCUPIED:
        return "WIFI_REASON_ALTERATIVE_CHANNEL_OCCUPIED";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "WIFI_REASON_BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "WIFI_REASON_NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "WIFI_REASON_AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "WIFI_REASON_ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "WIFI_REASON_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "WIFI_REASON_CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET:
        return "WIFI_REASON_AP_TSF_RESET";
    case WIFI_REASON_ROAMING:
        return "WIFI_REASON_ROAMING";
    case WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG:
        return "WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG";
    case WIFI_REASON_SA_QUERY_TIMEOUT:
        return "WIFI_REASON_SA_QUERY_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:
        return "WIFI_REASON_UNKNOWN";
    }
}

static esp_err_t reset_persistent_wifi_rf_state(void)
{
    nvs_handle_t net80211 = 0;
    esp_err_t err = nvs_open("nvs.net80211", NVS_READWRITE, &net80211);
    if (err == ESP_OK) {
        err = nvs_erase_all(net80211);
        ESP_LOGW(TAG, "OTA WiFi: nvs.net80211 erase_all -> %s", esp_err_to_name(err));
        if (err == ESP_OK) {
            err = nvs_commit(net80211);
            ESP_LOGW(TAG, "OTA WiFi: nvs.net80211 commit -> %s", esp_err_to_name(err));
        }
        nvs_close(net80211);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "OTA WiFi: nvs.net80211 namespace not present");
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "OTA WiFi: nvs.net80211 open -> %s", esp_err_to_name(err));
    }

    esp_err_t phy_err = esp_phy_erase_cal_data_in_nvs();
    if (phy_err == ESP_ERR_NVS_NOT_FOUND) {
        phy_err = ESP_OK;
    }
    ESP_LOGW(TAG, "OTA WiFi: esp_phy_erase_cal_data_in_nvs() -> %s",
             esp_err_to_name(phy_err));

    return err != ESP_OK ? err : phy_err;
}

static esp_err_t set_cz_country(void)
{
    wifi_country_t country_config = {
        .cc = "CZ",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 78,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };

    esp_err_t err = esp_wifi_set_country(&country_config);
    ESP_LOGI(TAG, "DIAG: esp_wifi_set_country(CZ manual 1..13) -> %s",
             esp_err_to_name(err));
    if (err != ESP_OK) {
        return err;
    }

    wifi_country_t country = {0};
    err = esp_wifi_get_country(&country);
    ESP_LOGI(TAG, "DIAG: esp_wifi_get_country() -> %s", esp_err_to_name(err));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "DIAG: country cc=%c%c%c schan=%u nchan=%u policy=%s(%d)",
                 country.cc[0], country.cc[1], country.cc[2],
                 country.schan,
                 country.nchan,
                 country_policy_name(country.policy),
                 country.policy);
        if (country.schan <= 13 && country.schan + country.nchan - 1 >= 13) {
            ESP_LOGI(TAG, "DIAG: channel 13 is allowed by current country config");
        } else {
            ESP_LOGE(TAG, "DIAG: channel 13 is NOT allowed by current country config");
        }
    }

    return err;
}

static void log_sta_ap_info(const char *prefix)
{
    wifi_ap_record_t ap = {0};
    const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    ESP_LOGI(TAG, "%s esp_wifi_sta_get_ap_info() -> %s",
             prefix, esp_err_to_name(err));
    if (err == ESP_OK) {
        char bssid[18];
        format_bssid(ap.bssid, bssid, sizeof(bssid));
        ESP_LOGI(TAG, "%s AP_INFO ssid='%s' bssid=%s channel=%u rssi=%d auth=%s(%d)",
                 prefix,
                 ap.ssid,
                 bssid,
                 ap.primary,
                 ap.rssi,
                 authmode_name(ap.authmode),
                 ap.authmode);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "DIAG: WIFI_EVENT_STA_START");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *event = (const wifi_event_sta_connected_t *)event_data;
        char bssid[18];
        format_bssid(event != NULL ? event->bssid : NULL, bssid, sizeof(bssid));
        ESP_LOGI(TAG, "DIAG: WIFI_EVENT_STA_CONNECTED ssid='%.*s' bssid=%s channel=%u auth=%s(%d) aid=%u",
                 event != NULL ? event->ssid_len : 0,
                 event != NULL ? (const char *)event->ssid : "",
                 bssid,
                 event != NULL ? event->channel : 0,
                 event != NULL ? authmode_name(event->authmode) : "unknown",
                 event != NULL ? event->authmode : -1,
                 event != NULL ? event->aid : 0);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        const uint8_t reason = event != NULL ? event->reason : 0;
        char bssid[18];
        format_bssid(event != NULL ? event->bssid : NULL, bssid, sizeof(bssid));
        ESP_LOGW(TAG, "DIAG: WIFI_EVENT_STA_DISCONNECTED ssid='%.*s' bssid=%s rssi=%d reason=%u %s",
                 event != NULL ? event->ssid_len : 0,
                 event != NULL ? (const char *)event->ssid : "",
                 bssid,
                 event != NULL ? event->rssi : 0,
                 reason,
                 disconnect_reason_name(reason));
        if (s_disconnect_requested) {
            ESP_LOGI(TAG, "DIAG: disconnect was requested by OTA diagnostic cleanup; not retrying");
            return;
        }
        if (s_retry_count < OTA_WIFI_MAX_RETRIES) {
            ++s_retry_count;
            ESP_LOGW(TAG, "DIAG: retrying WiFi connect %d/%d after reason=%u %s",
                     s_retry_count, OTA_WIFI_MAX_RETRIES,
                     reason, disconnect_reason_name(reason));
            const esp_err_t err = esp_wifi_connect();
            ESP_LOGI(TAG, "DIAG: esp_wifi_connect() retry -> %s",
                     esp_err_to_name(err));
            if (err != ESP_OK && s_wifi_events != NULL) {
                xEventGroupSetBits(s_wifi_events, OTA_WIFI_FAIL_BIT);
            }
        } else if (s_wifi_events != NULL) {
            ESP_LOGE(TAG, "DIAG: no retries left after reason=%u %s",
                     reason, disconnect_reason_name(reason));
            xEventGroupSetBits(s_wifi_events, OTA_WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "DIAG: IP_EVENT_STA_GOT_IP ip=" IPSTR " netmask=" IPSTR " gw=" IPSTR,
                 IP2STR(&event->ip_info.ip),
                 IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));
        log_sta_ap_info("DIAG:");
        s_retry_count = 0;
        if (s_wifi_events != NULL) {
            xEventGroupSetBits(s_wifi_events, OTA_WIFI_CONNECTED_BIT);
        }
    }
}

static void wifi_scan_event_handler(void *arg, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    (void)event_data;

    EventGroupHandle_t events = (EventGroupHandle_t)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "DIAG: WIFI_EVENT_STA_START received for scan-only test");
        if (events != NULL) {
            xEventGroupSetBits(events, OTA_WIFI_SCAN_START_BIT);
        }
    }
}

static void log_wifi_mode(const char *prefix)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    const esp_err_t err = esp_wifi_get_mode(&mode);
    ESP_LOGI(TAG, "%s esp_wifi_get_mode() -> %s mode=%d",
             prefix, esp_err_to_name(err), mode);
}

static void log_sta_mac(const char *prefix)
{
    uint8_t mac[6] = {0};
    const esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_text[18];
    format_bssid(mac, mac_text, sizeof(mac_text));
    ESP_LOGI(TAG, "%s esp_wifi_get_mac(WIFI_IF_STA) -> %s mac=%s",
             prefix, esp_err_to_name(err), mac_text);
}

static esp_err_t configure_wifi_rf_like_reference(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    ESP_RETURN_ON_ERROR(set_cz_country(), TAG, "esp_wifi_set_country failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_protocol(WIFI_IF_STA,
                                              WIFI_PROTOCOL_11B |
                                              WIFI_PROTOCOL_11G |
                                              WIFI_PROTOCOL_11N),
                        TAG, "esp_wifi_set_protocol failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20),
                        TAG, "esp_wifi_set_bandwidth failed");
    return ESP_OK;
}

static bool choose_target_from_scan(const char *ssid, uint16_t count,
                                    wifi_ap_record_t *selected)
{
    if (count == 0) {
        return false;
    }

    memset(s_scan_records, 0, sizeof(s_scan_records));
    uint16_t returned = count > OTA_WIFI_MAX_SCAN_RECORDS ? OTA_WIFI_MAX_SCAN_RECORDS : count;
    esp_err_t err = esp_wifi_scan_get_ap_records(&returned, s_scan_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA WiFi: esp_wifi_scan_get_ap_records -> %s", esp_err_to_name(err));
        return false;
    }

    bool found = false;
    for (uint16_t i = 0; i < returned; ++i) {
        char bssid[18];
        format_bssid(s_scan_records[i].bssid, bssid, sizeof(bssid));
        const bool ssid_match = strcmp((const char *)s_scan_records[i].ssid, ssid) == 0;
        const bool reference_bssid = memcmp(s_scan_records[i].bssid,
                                            s_target_bssid,
                                            sizeof(s_target_bssid)) == 0;

        ESP_LOGI(TAG, "OTA WiFi: AP[%u]%s%s ssid='%s' bssid=%s channel=%u rssi=%d auth=%s(%d)",
                 (unsigned)i,
                 ssid_match ? " SSID_MATCH" : "",
                 reference_bssid ? " REFERENCE_BSSID" : "",
                 s_scan_records[i].ssid,
                 bssid,
                 s_scan_records[i].primary,
                 s_scan_records[i].rssi,
                 authmode_name(s_scan_records[i].authmode),
                 s_scan_records[i].authmode);

        if (!ssid_match) {
            continue;
        }
        if (!found || reference_bssid || s_scan_records[i].rssi > selected->rssi) {
            *selected = s_scan_records[i];
            found = true;
            if (reference_bssid) {
                break;
            }
        }
    }

    return found;
}

static esp_err_t run_channel13_fallback_scan(const char *reason, uint16_t *count)
{
    wifi_scan_config_t channel13_scan = {
        .ssid = NULL,
        .bssid = (uint8_t *)s_target_bssid,
        .channel = 13,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time.passive = 5000,
    };

    ESP_LOGW(TAG, "OTA WiFi: %s, retrying passive channel 13 for reference BSSID",
             reason);
    esp_err_t err = esp_wifi_scan_start(&channel13_scan, true);
    ESP_LOGI(TAG, "OTA WiFi: esp_wifi_scan_start(channel 13 passive) -> %s",
             esp_err_to_name(err));
    if (err != ESP_OK) {
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(count), TAG,
                        "esp_wifi_scan_get_ap_num ch13 failed");
    ESP_LOGW(TAG, "OTA WiFi: channel 13 fallback scan found %u APs",
             (unsigned)*count);
    return ESP_OK;
}

static esp_err_t select_target_ap(const char *ssid, wifi_ap_record_t *selected)
{
    if (selected == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(selected, 0, sizeof(*selected));

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 1500,
    };

    ESP_LOGW(TAG, "OTA WiFi: reference scan before connect for SSID '%s'", ssid);
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    ESP_LOGI(TAG, "OTA WiFi: esp_wifi_scan_start(all channels) -> %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        return err;
    }

    uint16_t count = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_num(&count), TAG, "esp_wifi_scan_get_ap_num failed");
    ESP_LOGW(TAG, "OTA WiFi: scan found %u APs", (unsigned)count);

    bool found = choose_target_from_scan(ssid, count, selected);
    if (!found) {
        err = run_channel13_fallback_scan(count == 0 ? "all-channel scan empty" : "target not found in all-channel scan",
                                          &count);
        if (err != ESP_OK) {
            return err;
        }
        found = choose_target_from_scan(ssid, count, selected);
    }

    if (!found) {
        ESP_LOGE(TAG, "OTA WiFi: target SSID '%s' not found in scans", ssid);
        return ESP_ERR_NOT_FOUND;
    }

    char selected_bssid[18];
    format_bssid(selected->bssid, selected_bssid, sizeof(selected_bssid));
    ESP_LOGW(TAG, "OTA WiFi: selected AP ssid='%s' bssid=%s channel=%u rssi=%d auth=%s(%d)",
             selected->ssid,
             selected_bssid,
             selected->primary,
             selected->rssi,
             authmode_name(selected->authmode),
             selected->authmode);
    return ESP_OK;
}

esp_err_t ota_wifi_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        ESP_LOGE(TAG, "OTA Wi-Fi SSID is not configured");
        return ESP_ERR_INVALID_STATE;
    }

    ota_wifi_prepare_radio_for_wifi("connect");
    s_coex_prepared = true;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == NULL) {
        vEventGroupDelete(s_wifi_events);
        s_wifi_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_log_level_set("wifi", ESP_LOG_VERBOSE);
    esp_log_level_set("wifi_init", ESP_LOG_VERBOSE);
    esp_log_level_set("phy_init", ESP_LOG_VERBOSE);
    ESP_RETURN_ON_ERROR(reset_persistent_wifi_rf_state(), TAG, "reset persistent WiFi/RF state failed");
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(configure_wifi_rf_like_reference(), TAG, "configure WiFi RF failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            wifi_event_handler, NULL,
                                                            &s_wifi_any_id_handler),
                        TAG, "register WIFI_EVENT failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            wifi_event_handler, NULL,
                                                            &s_ip_got_ip_handler),
                        TAG, "register IP_EVENT failed");

    s_retry_count = 0;
    s_disconnect_requested = false;
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "DIAG: esp_wifi_set_ps(WIFI_PS_NONE) -> %s", esp_err_to_name(err));
    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_set_ps failed");
    err = esp_wifi_set_max_tx_power(78);
    ESP_LOGI(TAG, "DIAG: esp_wifi_set_max_tx_power(78) -> %s", esp_err_to_name(err));
    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_set_max_tx_power failed");
    s_started = true;

    wifi_ap_record_t selected_ap = {0};
    err = select_target_ap(ssid, &selected_ap);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (password != NULL) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }
    memcpy(wifi_config.sta.bssid, selected_ap.bssid, sizeof(selected_ap.bssid));
    wifi_config.sta.bssid_set = true;
    wifi_config.sta.channel = selected_ap.primary;
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;
    wifi_config.sta.failure_retry_cnt = 1;

    ESP_LOGI(TAG, "DIAG: wifi_config.sta ssid='%s' scan_method=%d threshold.authmode=%s(%d) sae_pwe_h2e=%s(%d) bssid_set=%d channel=%u failure_retry_cnt=%u",
             ssid,
             wifi_config.sta.scan_method,
             authmode_name(wifi_config.sta.threshold.authmode),
             wifi_config.sta.threshold.authmode,
             sae_pwe_name(wifi_config.sta.sae_pwe_h2e),
             wifi_config.sta.sae_pwe_h2e,
             wifi_config.sta.bssid_set,
             wifi_config.sta.channel,
             wifi_config.sta.failure_retry_cnt);
    ESP_LOGI(TAG, "DIAG: esp_wifi_set_storage(WIFI_STORAGE_RAM) active; WiFi config will not be persisted to NVS");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config failed");
    err = esp_wifi_connect();
    ESP_LOGI(TAG, "DIAG: esp_wifi_connect() after full config -> %s", esp_err_to_name(err));
    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_connect failed");

    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        OTA_WIFI_CONNECTED_BIT | OTA_WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(OTA_WIFI_TIMEOUT_MS)
    );

    if ((bits & OTA_WIFI_CONNECTED_BIT) != 0) {
        ESP_LOGI(TAG, "DIAG: WiFi connected");
        log_sta_ap_info("DIAG:");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "DIAG: WiFi connect did not complete, event_bits=0x%lx",
             (unsigned long)bits);
    return ESP_ERR_TIMEOUT;
}

esp_err_t ota_wifi_scan_log(const char *target_ssid)
{
    ESP_LOGW(TAG, "DIAG: ===== PURE WIFI PHY/SCAN TEST START =====");
    ota_wifi_prepare_radio_for_wifi("scan");
    log_wifi_mode("DIAG: before init:");

    esp_err_t err = esp_netif_init();
    ESP_LOGI(TAG, "DIAG: esp_netif_init() -> %s", esp_err_to_name(err));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    ESP_LOGI(TAG, "DIAG: esp_event_loop_create_default() -> %s", esp_err_to_name(err));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    esp_netif_t *scan_netif = esp_netif_create_default_wifi_sta();
    ESP_LOGI(TAG, "DIAG: esp_netif_create_default_wifi_sta() -> %s ptr=%p",
             scan_netif != NULL ? "ESP_OK" : "ESP_ERR_NO_MEM",
             scan_netif);
    if (scan_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    EventGroupHandle_t scan_events = xEventGroupCreate();
    ESP_LOGI(TAG, "DIAG: xEventGroupCreate(scan) -> %s ptr=%p",
             scan_events != NULL ? "ESP_OK" : "ESP_ERR_NO_MEM",
             scan_events);
    if (scan_events == NULL) {
        esp_netif_destroy_default_wifi(scan_netif);
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_log_level_set("wifi", ESP_LOG_VERBOSE);
    esp_log_level_set("wifi_init", ESP_LOG_VERBOSE);
    esp_log_level_set("phy_init", ESP_LOG_VERBOSE);
    err = reset_persistent_wifi_rf_state();
    ESP_LOGI(TAG, "DIAG: reset_persistent_wifi_rf_state(scan) -> %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        vEventGroupDelete(scan_events);
        esp_netif_destroy_default_wifi(scan_netif);
        return err;
    }
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DIAG: esp_wifi_init(scan) -> %s", esp_err_to_name(err));
        vEventGroupDelete(scan_events);
        esp_netif_destroy_default_wifi(scan_netif);
        return err;
    }
    ESP_LOGI(TAG, "DIAG: esp_wifi_init(scan) -> %s", esp_err_to_name(err));
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_LOGI(TAG, "DIAG: esp_wifi_set_storage(WIFI_STORAGE_RAM scan) -> %s",
             esp_err_to_name(err));
    log_wifi_mode("DIAG: after init:");

    esp_event_handler_instance_t scan_start_handler = NULL;
    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              WIFI_EVENT_STA_START,
                                              wifi_scan_event_handler,
                                              scan_events,
                                              &scan_start_handler);
    ESP_LOGI(TAG, "DIAG: register WIFI_EVENT_STA_START(scan) -> %s",
             esp_err_to_name(err));

    if (err == ESP_OK) {
        err = configure_wifi_rf_like_reference();
        ESP_LOGI(TAG, "DIAG: configure_wifi_rf_like_reference(scan) -> %s", esp_err_to_name(err));
        log_wifi_mode("DIAG: after set_mode:");
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
        ESP_LOGI(TAG, "DIAG: esp_wifi_start(scan) -> %s", esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "DIAG: esp_wifi_set_ps(WIFI_PS_NONE scan) -> %s",
                 esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_max_tx_power(78);
        ESP_LOGI(TAG, "DIAG: esp_wifi_set_max_tx_power(78 scan) -> %s",
                 esp_err_to_name(err));
    }
    if (err == ESP_OK) {
        const EventBits_t bits = xEventGroupWaitBits(scan_events,
                                                     OTA_WIFI_SCAN_START_BIT,
                                                     pdFALSE,
                                                     pdFALSE,
                                                     pdMS_TO_TICKS(OTA_WIFI_SCAN_START_TIMEOUT_MS));
        ESP_LOGI(TAG, "DIAG: wait WIFI_EVENT_STA_START -> bits=0x%lx",
                 (unsigned long)bits);
        if ((bits & OTA_WIFI_SCAN_START_BIT) == 0) {
            ESP_LOGE(TAG, "DIAG: WIFI_EVENT_STA_START not received before scan");
            err = ESP_ERR_TIMEOUT;
        }
    }
    if (err == ESP_OK) {
        log_wifi_mode("DIAG: before scan:");
        log_sta_mac("DIAG: before scan:");
    }

    if (err == ESP_OK) {
        wifi_scan_config_t scan_config = {0};
        scan_config.ssid = NULL;
        scan_config.bssid = NULL;
        scan_config.channel = 0;
        scan_config.show_hidden = true;
        scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        scan_config.scan_time.active.min = 120;
        scan_config.scan_time.active.max = 1500;
        scan_config.scan_time.passive = 1500;
        ESP_LOGI(TAG, "DIAG: scan_config ssid=NULL bssid=NULL channel=0 show_hidden=true scan_type=%d",
                 scan_config.scan_type);
        ESP_LOGI(TAG, "DIAG: synchronous unfiltered WiFi scan for target SSID '%s'",
                 target_ssid != NULL ? target_ssid : "");
        err = esp_wifi_scan_start(&scan_config, true);
        ESP_LOGI(TAG, "DIAG: esp_wifi_scan_start() -> %s", esp_err_to_name(err));
    }

    if (err == ESP_OK) {
        uint16_t count = 0;
        err = esp_wifi_scan_get_ap_num(&count);
        ESP_LOGI(TAG, "DIAG: esp_wifi_scan_get_ap_num() -> %s count=%u",
                 esp_err_to_name(err), (unsigned)count);
        wifi_ap_record_t *records = NULL;
        if (err == ESP_OK && count > 0) {
            records = heap_caps_calloc(count, sizeof(*records), MALLOC_CAP_DEFAULT);
        }
        if (err == ESP_OK && count > 0 && records == NULL) {
            ESP_LOGE(TAG, "DIAG: cannot allocate %u WiFi scan records", (unsigned)count);
            err = ESP_ERR_NO_MEM;
        }

        uint16_t record_count = err == ESP_OK ? count : 0;
        if (err == ESP_OK && record_count > 0) {
            err = esp_wifi_scan_get_ap_records(&record_count, records);
            ESP_LOGI(TAG, "DIAG: esp_wifi_scan_get_ap_records() -> %s returned=%u",
                     esp_err_to_name(err), (unsigned)record_count);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "DIAG: scan found %u APs, logging all %u",
                     (unsigned)count, (unsigned)record_count);
        }

        bool target_ssid_found = false;
        bool target_bssid_found = false;
        bool target_bssid_channel13_found = false;
        for (uint16_t i = 0; err == ESP_OK && i < record_count; ++i) {
            const bool is_target_ssid = target_ssid != NULL
                && strcmp((const char *)records[i].ssid, target_ssid) == 0;
            const bool is_target_bssid = memcmp(records[i].bssid, s_target_bssid, sizeof(s_target_bssid)) == 0;
            target_ssid_found = target_ssid_found || is_target_ssid;
            target_bssid_found = target_bssid_found || is_target_bssid;
            target_bssid_channel13_found = target_bssid_channel13_found
                || (is_target_bssid && records[i].primary == 13);

            char bssid[18];
            format_bssid(records[i].bssid, bssid, sizeof(bssid));
            ESP_LOGI(TAG, "DIAG: AP[%u]%s%s ssid='%s' bssid=%s channel=%u rssi=%d auth=%s(%d)",
                     (unsigned)i,
                     is_target_ssid ? " SSID_MATCH" : "",
                     is_target_bssid ? " TARGET_BSSID" : "",
                     records[i].ssid,
                     bssid,
                     records[i].primary,
                     records[i].rssi,
                     authmode_name(records[i].authmode),
                     records[i].authmode);

            if (is_target_ssid) {
                ESP_LOGI(TAG, "DIAG: FOUND SSID '%s' BSSID=%s channel=%u rssi=%d auth=%s(%d)",
                         target_ssid,
                         bssid,
                         records[i].primary,
                         records[i].rssi,
                         authmode_name(records[i].authmode),
                         records[i].authmode);
            }
            if (is_target_bssid && records[i].primary == 13) {
                ESP_LOGW(TAG, "DIAG: *** FOUND TARGET BSSID 12:5a:95:22:d4:06 ON CHANNEL 13 ***");
            }
        }

        if (err == ESP_OK && target_ssid != NULL && target_ssid[0] != '\0') {
            ESP_LOGI(TAG, "DIAG: SUMMARY target_ssid='%s' found=%s",
                     target_ssid, target_ssid_found ? "YES" : "NO");
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "DIAG: SUMMARY target_bssid=12:5a:95:22:d4:06 found=%s found_on_channel13=%s",
                     target_bssid_found ? "YES" : "NO",
                     target_bssid_channel13_found ? "YES" : "NO");
        }

        free(records);

        if (err == ESP_OK && count == 0) {
            wifi_scan_config_t channel13_scan = {0};
            channel13_scan.ssid = NULL;
            channel13_scan.bssid = NULL;
            channel13_scan.channel = 13;
            channel13_scan.show_hidden = true;
            channel13_scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            channel13_scan.scan_time.active.min = 120;
            channel13_scan.scan_time.active.max = 3000;
            channel13_scan.scan_time.passive = 3000;
            ESP_LOGW(TAG, "DIAG: all-channel scan found 0 APs; retrying explicit long active scan on channel 13");
            err = esp_wifi_scan_start(&channel13_scan, true);
            ESP_LOGI(TAG, "DIAG: esp_wifi_scan_start(channel=13) -> %s", esp_err_to_name(err));
            if (err == ESP_OK) {
                uint16_t channel13_count = 0;
                err = esp_wifi_scan_get_ap_num(&channel13_count);
                ESP_LOGI(TAG, "DIAG: channel 13 scan AP count=%u err=%s",
                         (unsigned)channel13_count, esp_err_to_name(err));
                if (err == ESP_OK && channel13_count > 0) {
                    wifi_ap_record_t *channel13_records = heap_caps_calloc(channel13_count,
                                                                           sizeof(*channel13_records),
                                                                           MALLOC_CAP_DEFAULT);
                    if (channel13_records == NULL) {
                        err = ESP_ERR_NO_MEM;
                    } else {
                        uint16_t returned = channel13_count;
                        err = esp_wifi_scan_get_ap_records(&returned, channel13_records);
                        ESP_LOGI(TAG, "DIAG: channel 13 records returned=%u err=%s",
                                 (unsigned)returned, esp_err_to_name(err));
                        for (uint16_t i = 0; err == ESP_OK && i < returned; ++i) {
                            char bssid[18];
                            format_bssid(channel13_records[i].bssid, bssid, sizeof(bssid));
                            ESP_LOGI(TAG, "DIAG: CH13 AP[%u] ssid='%s' bssid=%s channel=%u rssi=%d auth=%s(%d)",
                                     (unsigned)i,
                                     channel13_records[i].ssid,
                                     bssid,
                                     channel13_records[i].primary,
                                     channel13_records[i].rssi,
                                     authmode_name(channel13_records[i].authmode),
                                     channel13_records[i].authmode);
                        }
                        free(channel13_records);
                    }
                }
            }
        }
    } else {
        ESP_LOGE(TAG, "DIAG: WiFi scan setup failed: %s", esp_err_to_name(err));
    }

    if (scan_start_handler != NULL) {
        const esp_err_t unregister_err = esp_event_handler_instance_unregister(WIFI_EVENT,
                                                                               WIFI_EVENT_STA_START,
                                                                               scan_start_handler);
        ESP_LOGI(TAG, "DIAG: unregister WIFI_EVENT_STA_START(scan) -> %s",
                 esp_err_to_name(unregister_err));
    }
    esp_err_t stop_err = esp_wifi_stop();
    ESP_LOGI(TAG, "DIAG: esp_wifi_stop(scan) -> %s", esp_err_to_name(stop_err));
    esp_err_t deinit_err = esp_wifi_deinit();
    ESP_LOGI(TAG, "DIAG: esp_wifi_deinit(scan) -> %s", esp_err_to_name(deinit_err));
    vEventGroupDelete(scan_events);
    esp_netif_destroy_default_wifi(scan_netif);
    ota_wifi_restore_radio_after_wifi("scan");
    ESP_LOGW(TAG, "DIAG: ===== PURE WIFI PHY/SCAN TEST END =====");
    return err;
}

void ota_wifi_disconnect(void)
{
    if (s_started) {
        s_disconnect_requested = true;
        esp_wifi_disconnect();
        esp_wifi_stop();
    }

    if (s_wifi_any_id_handler != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_any_id_handler);
        s_wifi_any_id_handler = NULL;
    }
    if (s_ip_got_ip_handler != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_got_ip_handler);
        s_ip_got_ip_handler = NULL;
    }

    if (s_started) {
        esp_wifi_deinit();
        s_started = false;
    }

    if (s_wifi_netif != NULL) {
        esp_netif_destroy_default_wifi(s_wifi_netif);
        s_wifi_netif = NULL;
    }

    if (s_wifi_events != NULL) {
        vEventGroupDelete(s_wifi_events);
        s_wifi_events = NULL;
    }

    if (s_coex_prepared) {
        ota_wifi_restore_radio_after_wifi("disconnect");
        s_coex_prepared = false;
    }

    s_disconnect_requested = false;
}
