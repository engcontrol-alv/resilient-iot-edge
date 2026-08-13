#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "storage_driver.h"      // [DEV1] Write-First storage layer
#include "wifi_manager.h"        // [DEV2] Wi-Fi AP/STA management
#include "captive_portal.h"      // [DEV2] HTTP + DNS provisioning portal
#include "oled_driver.h"         // [REFACTORED] OLED Display Driver

// --- I/O MAPPING ---
#define TEST_GPIO_A 4        // INPUT (Controller Signal)
#define TEST_GPIO_B 2        // DIRECT OUTPUT (Follows GPIO 4)

// Conservative estimate of bytes/record (current NDJSON format).
// Better to underestimate capacity than overestimate promised duration.
#define ESTIMATED_RECORD_BYTES     70U
#define TARGET_BUFFER_SECONDS      (CONFIG_RIE_EMERGENCY_MIN_BUFFER_HOURS * 3600U)
#define MAX_RECORDS_IN_BUFFER      (STORAGE_MAX_FILE_SIZE / ESTIMATED_RECORD_BYTES)

// Ceiling division — guarantees duration >= configured limit,
// never less (truncating down would make the interval too short 
// and actual duration would fall below the promised one).
#define EMERGENCY_WRITE_INTERVAL_TICKS \
    (((TARGET_BUFFER_SECONDS * 1000U) + (MAX_RECORDS_IN_BUFFER * 50U) - 1U) \
     / (MAX_RECORDS_IN_BUFFER * 50U))

static const char *TAG = "SYSTEM_MAIN";

// --- FINITE STATE MACHINE (FSM) ---
typedef enum {
    SYS_MODE_CONFIG,
    SYS_MODE_CONNECTING,     // [NEW] Non-blocking connection state
    SYS_MODE_OPERATION,
    SYS_MODE_EMERGENCY,
    SYS_MODE_RESYNC
} system_mode_t;

volatile system_mode_t current_mode = SYS_MODE_CONFIG;
static uint64_t connection_start_time = 0; // For Wi-Fi connection timeout

// [DEV2] EMERGENCY state — exponential backoff state
static uint32_t s_backoff_ms      = 1000U;
static uint32_t s_backoff_elapsed = 0U;

// --- MAIN ENTRY POINT ---
void app_main(void)
{
    ESP_LOGI(TAG, "Configuring basic hardware interface...");

    gpio_reset_pin(TEST_GPIO_A);
    gpio_set_direction(TEST_GPIO_A, GPIO_MODE_INPUT);
    gpio_set_pull_mode(TEST_GPIO_A, GPIO_PULLDOWN_ONLY);

    gpio_reset_pin(TEST_GPIO_B);
    gpio_set_direction(TEST_GPIO_B, GPIO_MODE_OUTPUT);
    gpio_set_level(TEST_GPIO_B, 0);

    ESP_LOGI(TAG, "Initializing OLED display...");
    oled_power_setup();      
    i2c_master_init();       
    oled_software_setup();   
    oled_clear();            

    oled_print(0, 0, "RESILIENT IOT EDGE"); 

    ESP_LOGI(TAG, "Initializing LittleFS Storage...");
    if (storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "FS MOUNT ERROR! Rebooting...");
        oled_print(0, 7, "FS MOUNT ERROR!     ");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart(); // Fail-fast: restarts the board if memory mount fails
    }
    
    ESP_LOGI(TAG, "Initializing Wi-Fi manager...");
    ESP_ERROR_CHECK(wifi_manager_init());

    if (wifi_manager_has_credentials()) {
        char ssid[WIFI_SSID_MAX_LEN] = {0};
        char pass[WIFI_PASS_MAX_LEN] = {0};
        wifi_manager_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
        ESP_LOGI(TAG, "NVS credentials found — connecting to '%s'", ssid);
        
        wifi_manager_start_sta(ssid, pass);
        
        // Instead of blocking the code while waiting, change state
        current_mode = SYS_MODE_CONNECTING;
        connection_start_time = esp_timer_get_time();
    } else {
        ESP_LOGI(TAG, "No credentials found — starting Captive Portal");
        wifi_manager_start_ap();
        captive_portal_start();
        current_mode = SYS_MODE_CONFIG;
    }

    uint32_t pending_records = storage_get_pending_count();
    ESP_LOGI(TAG, "[RECOVERY] %lu record(s) pending from previous session", (unsigned long)pending_records);

    if (pending_records > 0) {
        oled_print(0, 3, "RECOVERY ACTIVE     ");
        vTaskDelay(pdMS_TO_TICKS(1500)); 
    }

    char init_info[21];
    snprintf(init_info, sizeof(init_info), "STORE: %05lu PEND  ", (unsigned long)pending_records);
    oled_print(0, 3, init_info);

    uint32_t s_tick = 0U;
    int last_state = -1;

    // Configuration for vTaskDelayUntil (Absolute Timing)
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(50);

    while (1) {
        switch (current_mode) {

            case SYS_MODE_CONFIG: {
                oled_print(0, 2, "FSM: CONFIG MODE    ");
                oled_print(0, 4, "SSID:RES-IOT-CFG    ");
                oled_print(0, 5, "IP: 192.168.4.1     ");

                EventBits_t bits = xEventGroupGetBits(g_wifi_event_group);
                if (bits & WIFI_EVT_CREDENTIALS_SET) {
                    xEventGroupClearBits(g_wifi_event_group, WIFI_EVT_CREDENTIALS_SET);
                    captive_portal_stop();

                    char ssid[WIFI_SSID_MAX_LEN] = {0};
                    char pass[WIFI_PASS_MAX_LEN] = {0};
                    wifi_manager_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
                    
                    xEventGroupClearBits(g_wifi_event_group, WIFI_EVT_CONNECTED | WIFI_EVT_DISCONNECTED);
                    wifi_manager_start_sta(ssid, pass);
                    
                    oled_print(0, 4, "CONNECTING...       ");
                    oled_print(0, 5, "                    ");

                    connection_start_time = esp_timer_get_time();
                    current_mode = SYS_MODE_CONNECTING;
                }
                break;
            }

            case SYS_MODE_CONNECTING: {
                oled_print(0, 2, "FSM: CONNECTING     ");
                
                EventBits_t bits = xEventGroupGetBits(g_wifi_event_group);
                
                if (bits & WIFI_EVT_CONNECTED) {
                    current_mode = SYS_MODE_OPERATION;
                    oled_print(0, 2, "FSM: OPERATION      ");
                    oled_print(0, 4, "                    ");
                    ESP_LOGI(TAG, "Wi-Fi connected — entering OPERATION");
                } 
                // 15-second timeout (15,000,000 microseconds)
                else if ((bits & WIFI_EVT_DISCONNECTED) || 
                         ((esp_timer_get_time() - connection_start_time) > 15000000ULL)) {
                    
                    ESP_LOGW(TAG, "Wi-Fi connect failed/timeout — back to CONFIG");
                    oled_print(0, 4, "WIFI FAILED         ");
                    wifi_manager_start_ap();
                    captive_portal_start();
                    current_mode = SYS_MODE_CONFIG;
                }
                break;
            }

            case SYS_MODE_OPERATION: {
                oled_print(0, 2, "FSM: OPERATION      ");

                EventBits_t bits = xEventGroupGetBits(g_wifi_event_group);
                if (bits & WIFI_EVT_DISCONNECTED) {
                    xEventGroupClearBits(g_wifi_event_group, WIFI_EVT_DISCONNECTED);
                    s_backoff_ms      = 1000U;
                    s_backoff_elapsed = 0U;
                    oled_print(0, 4, "                    ");
                    oled_print(0, 6, "                    ");
                    current_mode = SYS_MODE_EMERGENCY;
                    break;
                }

                int gpio_level = gpio_get_level(TEST_GPIO_A);
                if (gpio_level != last_state) {
                    last_state = gpio_level;
                    if (gpio_level == 1) {
                        gpio_set_level(TEST_GPIO_B, 1);
                        oled_print(0, 4, "GPIO 4 = HIGH (SIGN)");
                        oled_print(0, 6, "GPIO 2 = HIGH (ON)  ");
                        ESP_LOGI(TAG, "[OPERATION] GPIO 4: HIGH -> GPIO 2: HIGH");
                    } else {
                        gpio_set_level(TEST_GPIO_B, 0);
                        oled_print(0, 4, "GPIO 4 = LOW (WAIT) ");
                        oled_print(0, 6, "GPIO 2 = LOW (OFF)  ");
                        ESP_LOGI(TAG, "[OPERATION] GPIO 4: LOW -> GPIO 2: LOW");
                    }
                }

                if ((s_tick % 100U) == 0U && s_tick > 0U) {
                    char record[STORAGE_MAX_LINE_LEN];
                    snprintf(record, sizeof(record),
                             "{\"uptime_ms\":%llu,\"gpio\":%d,"
                             "\"state\":\"OPERATION\",\"seq\":%lu}",
                             (unsigned long long)(esp_timer_get_time() / 1000ULL),
                             gpio_level,
                             (unsigned long)(s_tick / 100UL));
                    if (storage_append(record) == ESP_OK) {
                        char info[21];
                        snprintf(info, sizeof(info), "STORE: %05lu PEND  ",
                                 (unsigned long)storage_get_pending_count());
                        oled_print(0, 3, info);
                    }
                }
                break;
            }

            case SYS_MODE_EMERGENCY: {
                oled_print(0, 2, "FSM: EMERGENCY      ");

                int gpio_level = gpio_get_level(TEST_GPIO_A);
                gpio_set_level(TEST_GPIO_B, 0); 

                if ((s_tick % EMERGENCY_WRITE_INTERVAL_TICKS) == 0U) {
                    char record[STORAGE_MAX_LINE_LEN];
                    snprintf(record, sizeof(record),
                             "{\"uptime_ms\":%llu,\"gpio\":%d,"
                             "\"state\":\"EMERGENCY\",\"seq\":%lu}",
                             (unsigned long long)(esp_timer_get_time() / 1000ULL),
                             gpio_level,
                             (unsigned long)s_tick);
                    storage_append(record);
                }

                char info[21];
                snprintf(info, sizeof(info), "STORE: %05lu PEND  ",
                         (unsigned long)storage_get_pending_count());
                oled_print(0, 3, info);

                s_backoff_elapsed += 50U;
                if (s_backoff_elapsed >= s_backoff_ms) {
                    s_backoff_elapsed = 0U;
                    uint32_t next_backoff_ms = s_backoff_ms * 2U;
                    s_backoff_ms = (next_backoff_ms < 60000U) ? next_backoff_ms : 60000U;
                    char backoff_str[21];
                    snprintf(backoff_str, sizeof(backoff_str),
                             "RETRY: %5lus       ",
                             (unsigned long)(s_backoff_ms / 1000U));
                    oled_print(0, 4, backoff_str);
                    ESP_LOGI(TAG, "[EMERGENCY] Reconnect. Next: %lu ms",
                             (unsigned long)s_backoff_ms);
                    wifi_manager_reconnect();
                }

                EventBits_t bits = xEventGroupGetBits(g_wifi_event_group);
                if (bits & WIFI_EVT_CONNECTED) {
                    xEventGroupClearBits(g_wifi_event_group, WIFI_EVT_CONNECTED);
                    s_backoff_ms      = 1000U;
                    s_backoff_elapsed = 0U;
                    ESP_LOGI(TAG, "Wi-Fi reconnected — %lu record(s) pending for DEV6",
                             (unsigned long)storage_get_pending_count());
                    current_mode = SYS_MODE_OPERATION;
                    oled_print(0, 2, "FSM: OPERATION      ");
                    last_state = -1; 
                }
                break;
            }

            case SYS_MODE_RESYNC:
                // Reserved for DEV6 — currently unreachable (see EMERGENCY above).
                oled_print(0, 2, "FSM: RESYNC         ");
                if (storage_get_pending_count() == 0U) {
                    current_mode = SYS_MODE_OPERATION;
                }
                break;

            default:
                ESP_LOGE(TAG, "FSM: INVALID STATE %d — forcing EMERGENCY", current_mode);
                current_mode = SYS_MODE_EMERGENCY;
                break;
        }

        s_tick++; 
        
        // Absolute delay: guarantees the loop takes exactly 50ms, 
        // accounting for the execution time of the code itself
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}