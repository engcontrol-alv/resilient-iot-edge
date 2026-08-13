/**
 * @file  wifi_manager.c
 * @brief Wi-Fi Manager — implementation
 */

#include "wifi_manager.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "esp_mac.h"

static const char *TAG = "WIFI_MGR";

// =============================================================================
// Internal State
// =============================================================================

EventGroupHandle_t    g_wifi_event_group = NULL;
static wifi_manager_state_t s_state      = WIFI_MGR_IDLE;
static esp_netif_t   *s_ap_netif         = NULL;
static esp_netif_t   *s_sta_netif        = NULL;

// =============================================================================
// Event Handler
// =============================================================================

static void prv_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "STA started — connecting...");
            s_state = WIFI_MGR_CONNECTING;
            esp_wifi_connect();

        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *e = data;
            ESP_LOGW(TAG, "STA disconnected (reason=%d)", e->reason);
            s_state = WIFI_MGR_DISCONNECTED;
            xEventGroupSetBits(g_wifi_event_group, WIFI_EVT_DISCONNECTED);
            xEventGroupClearBits(g_wifi_event_group, WIFI_EVT_CONNECTED);

        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "Client joined AP: %02x:%02x:%02x:%02x:%02x:%02x",
                e->mac[0], e->mac[1], e->mac[2],
                e->mac[3], e->mac[4], e->mac[5]);
}

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "IP obtained: " IPSTR, IP2STR(&e->ip_info.ip));
        s_state = WIFI_MGR_CONNECTED;
        xEventGroupClearBits(g_wifi_event_group, WIFI_EVT_DISCONNECTED);
        xEventGroupSetBits(g_wifi_event_group, WIFI_EVT_CONNECTED);
    }
}

// =============================================================================
// Public API
// =============================================================================

esp_err_t wifi_manager_init(void)
{
    // ── NVS ──────────────────────────────────────────────────────────────────
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased — reinitializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ── TCP/IP + Event Loop ──────────────────────────────────────────────────
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ── FreeRTOS Event Group ─────────────────────────────────────────────────
    g_wifi_event_group = xEventGroupCreate();
    if (!g_wifi_event_group) {
        ESP_LOGE(TAG, "Event group allocation failed");
        return ESP_ERR_NO_MEM;
    }

    // ── Wi-Fi Driver ─────────────────────────────────────────────────────────
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // ── Event Handlers ────────────────────────────────────────────────────────
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &prv_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &prv_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "Wi-Fi manager initialized");
    return ESP_OK;
}

// -----------------------------------------------------------------------------

esp_err_t wifi_manager_start_ap(void)
{
    ESP_LOGI(TAG, "Starting SoftAP: SSID='%s'", WIFI_AP_SSID);

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = (uint8_t)strlen(WIFI_AP_SSID),
            .channel        = WIFI_AP_CHANNEL,
            .password       = "",
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_state = WIFI_MGR_AP_ACTIVE;
    ESP_LOGI(TAG, "SoftAP active. Connect to '%s' → open 192.168.4.1",
             WIFI_AP_SSID);
    return ESP_OK;
}

// -----------------------------------------------------------------------------

esp_err_t wifi_manager_start_sta(const char *ssid, const char *password)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "Starting STA: SSID='%s'", ssid);

    // Stop AP cleanly before switching to STA
    esp_wifi_stop();

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid,     ssid,
            sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password,
            sizeof(sta_cfg.sta.password) - 1);

    // Accept WPA or WPA2 when password provided; open when empty
    sta_cfg.sta.threshold.authmode =
        (strlen(password) > 0) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start()); // → triggers STA_START → esp_wifi_connect()

    s_state = WIFI_MGR_CONNECTING;
    return ESP_OK;
}

// -----------------------------------------------------------------------------

esp_err_t wifi_manager_reconnect(void)
{
    ESP_LOGI(TAG, "Reconnect attempt...");
    esp_err_t ret = esp_wifi_connect();
    if (ret == ESP_OK) {
        s_state = WIFI_MGR_CONNECTING;
    }
    return ret;
}

// -----------------------------------------------------------------------------

bool wifi_manager_has_credentials(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool has = (nvs_get_str(h, WIFI_NVS_KEY_SSID, NULL, &len) == ESP_OK &&
                len > 1);
    nvs_close(h);
    return has;
}

// -----------------------------------------------------------------------------

esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len,
                                         char *pass, size_t pass_len)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h));
    esp_err_t r1 = nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid, &ssid_len);
    esp_err_t r2 = nvs_get_str(h, WIFI_NVS_KEY_PASS, pass, &pass_len);
    nvs_close(h);
    return (r1 == ESP_OK && r2 == ESP_OK) ? ESP_OK : ESP_FAIL;
}

// -----------------------------------------------------------------------------

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, WIFI_NVS_KEY_PASS, pass ? pass : ""));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "Credentials saved: SSID='%s'", ssid);
    return ESP_OK;
}

// -----------------------------------------------------------------------------

wifi_manager_state_t wifi_manager_get_state(void)
{
    return s_state;
}