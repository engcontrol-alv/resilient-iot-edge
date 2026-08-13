/**
 * @file  captive_portal.h
 * @brief Captive Portal — HTTP server + DNS responder for Wi-Fi provisioning
 * @version DEV2
 *
 * @details Serves an HTML form at 192.168.4.1 when the device is in SoftAP
 *          mode. A minimal DNS server redirects all queries to 192.168.4.1,
 *          triggering the captive portal detection on iOS and Android.
 *
 *          On form submission: saves credentials via wifi_manager, sets
 *          WIFI_EVT_CREDENTIALS_SET bit in g_wifi_event_group.
 *          The FSM in main.c detects this bit and transitions to STA mode.
 *
 * @author  Eng. Alvaro
 */

#ifndef CAPTIVE_PORTAL_H
#define CAPTIVE_PORTAL_H

#include "esp_err.h"

/**
 * @brief  Starts the HTTP server and DNS responder task.
 *         Call after wifi_manager_start_ap().
 */
esp_err_t captive_portal_start(void);

/**
 * @brief  Stops the HTTP server and DNS responder task.
 *         Call before wifi_manager_start_sta().
 */
esp_err_t captive_portal_stop(void);

#endif // CAPTIVE_PORTAL_H