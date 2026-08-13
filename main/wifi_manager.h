/**
 * @file    wifi_manager.h
 * @brief   Wi-Fi Manager — SoftAP / STA modes + NVS credential storage
 * @version DEV2
 *
 * @details Manages the full Wi-Fi lifecycle:
 *          - SoftAP mode for Captive Portal provisioning (CONFIG state)
 *          - STA mode for normal operation (OPERATION/EMERGENCY/RESYNC)
 *          - NVS credential persistence across reboots
 *          - FreeRTOS EventGroup for FSM state communication
 *
 * @author  Eng. Alvaro
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// =============================================================================
// Configuration
// =============================================================================

#define WIFI_AP_SSID          "resilient-iot-cfg"
#define WIFI_AP_CHANNEL       1
#define WIFI_AP_MAX_CONN      4

#define WIFI_NVS_NAMESPACE    "rie_cfg"
#define WIFI_NVS_KEY_SSID     "ssid"
#define WIFI_NVS_KEY_PASS     "pass"

#define WIFI_SSID_MAX_LEN     33    // 32 chars + null
#define WIFI_PASS_MAX_LEN     65    // 64 chars + null

// =============================================================================
// Event Group (extern — read by FSM in main.c)
// =============================================================================

extern EventGroupHandle_t g_wifi_event_group;

#define WIFI_EVT_CONNECTED        BIT0  // STA got IP
#define WIFI_EVT_DISCONNECTED     BIT1  // STA lost connection
#define WIFI_EVT_CREDENTIALS_SET  BIT2  // Captive portal received credentials

// =============================================================================
// State
// =============================================================================

typedef enum {
    WIFI_MGR_IDLE,
    WIFI_MGR_AP_ACTIVE,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_DISCONNECTED,
} wifi_manager_state_t;

// =============================================================================
// Public API
// =============================================================================

/**
 * @brief  Initializes NVS, TCP/IP stack, event loop, and Wi-Fi driver.
 *         Must be called once before any other wifi_manager function.
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief  Starts Wi-Fi in SoftAP mode (CONFIG state).
 *         Creates open network "resilient-iot-cfg" on 192.168.4.1.
 */
esp_err_t wifi_manager_start_ap(void);

/**
 * @brief  Stops AP, starts STA, and connects with given credentials.
 * @param[in] ssid      Wi-Fi network name (null-terminated).
 * @param[in] password  Wi-Fi password. Empty string for open networks.
 */
esp_err_t wifi_manager_start_sta(const char *ssid, const char *password);

/**
 * @brief  Attempts reconnection using the current STA configuration.
 *         Called during EMERGENCY exponential backoff.
 */
esp_err_t wifi_manager_reconnect(void);

/** @brief Returns true if valid SSID credentials exist in NVS. */
bool wifi_manager_has_credentials(void);

/**
 * @brief  Loads credentials from NVS into caller-provided buffers.
 * @return ESP_OK on success, ESP_FAIL if not found.
 */
esp_err_t wifi_manager_load_credentials(char *ssid, size_t ssid_len,
                                         char *pass, size_t pass_len);

/**
 * @brief  Saves credentials to NVS. Called by captive portal on form submit.
 */
esp_err_t wifi_manager_save_credentials(const char *ssid, const char *pass);

/** @brief Returns the current connection state. */
wifi_manager_state_t wifi_manager_get_state(void);

#endif // WIFI_MANAGER_H