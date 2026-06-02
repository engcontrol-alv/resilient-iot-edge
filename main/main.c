#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_system.h"
#include "storage_driver.h"      // [DEV1] Write-First storage layer

// --- HELTEC V3 OFFICIAL PINOUT ---
#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21
#define OLED_VEXT 36

// --- I/O MAPPING ---
#define TEST_GPIO_A 4        // INPUT (Controller Signal)
#define TEST_GPIO_B 2        // DIRECT OUTPUT (Follows GPIO 4)

#define I2C_PORT I2C_NUM_0
#define OLED_ADDR 0x3C
static const char *TAG = "SYSTEM_MAIN";

// --- FINITE STATE MACHINE (FSM) ---
typedef enum {
    SYS_MODE_CONFIG,
    SYS_MODE_OPERATION,
    SYS_MODE_EMERGENCY,
    SYS_MODE_RESYNC
} system_mode_t;

// MVP initializes in OPERATION mode
volatile system_mode_t current_mode = SYS_MODE_OPERATION;

// --- 5x7 MINI FONT ---
static const uint8_t font5x7[59][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x2f, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7f, 0x14, 0x7f, 0x14},
    {0x24, 0x2a, 0x7f, 0x2a, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1c, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1c, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x00, 0x50, 0x30, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x59, 0x51, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}
};

// --- HELTEC POWER MANAGEMENT ---
void oled_power_setup(void) {
    gpio_reset_pin(OLED_VEXT);
    gpio_set_direction(OLED_VEXT, GPIO_MODE_OUTPUT);
    gpio_set_level(OLED_VEXT, 0); 
    vTaskDelay(pdMS_TO_TICKS(100)); 

    gpio_reset_pin(OLED_RST);
    gpio_set_direction(OLED_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(OLED_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(OLED_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); 
}

// --- I2C COMMUNICATION ---
void i2c_master_init(void) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = OLED_SDA, .scl_io_num = OLED_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE, .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    i2c_param_config(I2C_PORT, &conf);
    i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);
}

void oled_send_cmd(uint8_t cmd) {
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_write_byte(cmd_handle, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd_handle, 0x00, true);
    i2c_master_write_byte(cmd_handle, cmd, true);
    i2c_master_stop(cmd_handle);
    i2c_master_cmd_begin(I2C_PORT, cmd_handle, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd_handle);
}

void oled_send_data(const uint8_t *data, size_t len) {
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_write_byte(cmd_handle, (OLED_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd_handle, 0x40, true);
    i2c_master_write(cmd_handle, data, len, true);
    i2c_master_stop(cmd_handle);
    i2c_master_cmd_begin(I2C_PORT, cmd_handle, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd_handle);
}

void oled_software_setup(void) {
    uint8_t init_cmds[] = {
        0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40, 0x81, 0xFF,
        0xA1, 0xA6, 0xA8, 0x3F, 0xA4, 0xD3, 0x00, 0xD5, 0x80, 0xD9,
        0xF1, 0xDA, 0x12, 0xDB, 0x40, 0x8D, 0x14, 0xAF
    };
    for(size_t i = 0; i < sizeof(init_cmds); i++) oled_send_cmd(init_cmds[i]);
}

void oled_clear(void) {
    uint8_t zero[128];
    memset(zero, 0, 128);
    for (int i = 0; i < 8; i++) {
        oled_send_cmd(0xB0 + i); oled_send_cmd(0x00); oled_send_cmd(0x10);
        oled_send_data(zero, 128);
    }
}

void oled_draw_char(uint8_t x, uint8_t page, char c) {
    if (c < 32 || c > 90) c = 32;
    uint8_t idx = c - 32;
    oled_send_cmd(0xB0 + page);
    oled_send_cmd(0x00 + (x & 0x0F));
    oled_send_cmd(0x10 + ((x >> 4) & 0x0F));
    oled_send_data((const uint8_t*)font5x7[idx], 5);
    uint8_t space = 0x00;
    oled_send_data(&space, 1);
}

void oled_print(uint8_t x, uint8_t page, const char *str) {
    while (*str) {
        oled_draw_char(x, page, *str);
        x += 6;
        str++;
    }
}

// --- MAIN ENTRY POINT ---
void app_main(void)
{
    ESP_LOGI(TAG, "Configuring basic hardware interface...");

    // Configure GPIO 4 as INPUT with internal pull-down
    gpio_reset_pin(TEST_GPIO_A);
    gpio_set_direction(TEST_GPIO_A, GPIO_MODE_INPUT);
    gpio_set_pull_mode(TEST_GPIO_A, GPIO_PULLDOWN_ONLY);

    // Configure GPIO 2 as OUTPUT (Initially OFF)
    gpio_reset_pin(TEST_GPIO_B);
    gpio_set_direction(TEST_GPIO_B, GPIO_MODE_OUTPUT);
    gpio_set_level(TEST_GPIO_B, 0);

    ESP_LOGI(TAG, "Initializing OLED display...");
    oled_power_setup();      
    i2c_master_init();       
    oled_software_setup();   
    oled_clear();            

    oled_print(0, 0, "RESILIENT IOT EDGE"); 

    // --- NEW PHASE 3: LITTLEFS INITIALIZATION (FAIL-FAST) ---
    ESP_LOGI(TAG, "Initializing LittleFS Storage...");
    if (storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "FS MOUNT ERROR! Rebooting...");
        oled_print(0, 7, "FS MOUNT ERROR!     ");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart(); // [DEV1] Fail-fast: restarts the board if memory mount fails
    }
    
    // Read saved data
    uint32_t pending_records = storage_get_pending_count();
    ESP_LOGI(TAG, "[RECOVERY] %lu record(s) pending from previous session", (unsigned long)pending_records);

    // Visual effect on OLED: If data was recovered, flash the RECOVERY message
    if (pending_records > 0) {
        oled_print(0, 3, "RECOVERY ACTIVE   ");
        vTaskDelay(pdMS_TO_TICKS(1500)); // Wait 1.5s for the user to read the message
    }

    // Print the initial state IMMEDIATELY (fixes the "blank line" UX bug)
    char init_info[21];
    snprintf(init_info, sizeof(init_info), "STORE: %05lu PEND  ", (unsigned long)pending_records);
    oled_print(0, 3, init_info);

    uint32_t s_tick = 0U;  // [DEV1] FSM tick counter for periodic storage writes
    int last_state = -1;   // Stores the previous state to prevent redundant I2C writes

    while (1) {
        // Core system logic routes based on FSM state
        switch (current_mode) {
            
            case SYS_MODE_CONFIG:
                oled_print(0, 2, "FSM: CONFIG MODE    ");
                break;

            case SYS_MODE_OPERATION: {
                oled_print(0, 2, "FSM: OPERATION      ");
                
                /* ── GPIO Sampling (anti-flicker guard) ────────────────────────── */
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

                /* ── [DEV1] Periodic Storage Write (every 5 s = 100 × 50 ms) ──── */
                if ((s_tick % 100U) == 0U && s_tick > 0U) {
                    char record[STORAGE_MAX_LINE_LEN];
                    snprintf(record, sizeof(record),
                             "{\"uptime_ms\":%lu,\"gpio\":%d,\"state\":\"OPERATION\",\"seq\":%lu}",
                             (unsigned long)(s_tick * 50UL),
                             gpio_level,
                             (unsigned long)(s_tick / 100UL));

                    if (storage_append(record) == ESP_OK) {
                        char info[21];
                        snprintf(info, sizeof(info), "STORE: %05lu PEND  ",
                                 (unsigned long)storage_get_pending_count());
                        oled_print(0, 3, info);
                        ESP_LOGI(TAG, "[DEV1] Record stored. Pending: %lu",
                                 (unsigned long)storage_get_pending_count());
                    }
                }
                break;
            }

            case SYS_MODE_EMERGENCY:
                oled_print(0, 2, "FSM: EMERGENCY      ");
                break;

            case SYS_MODE_RESYNC:
                oled_print(0, 2, "FSM: RESYNC         ");
                break;
                
            default:
                ESP_LOGE(TAG, "FSM: INVALID STATE %d — forcing EMERGENCY", current_mode);
                current_mode = SYS_MODE_EMERGENCY;
                break;
        }

        s_tick++; // [DEV1] Advance FSM tick counter
        // Deterministic sampling interval (50ms)
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}