# System Architecture Design
## Resilient IoT Edge — ESP32-S3FN8

---

## Document Information

| Field            | Value                                      |
|------------------|--------------------------------------------|
| Project          | Resilient IoT Edge                         |
| Hardware         | ESP32-S3FN8 — Heltec WiFi LoRa 32 V3      |
| Toolchain        | ESP-IDF 6.0.1 + FreeRTOS (native)         |
| Standard         | IEEE Std 830-1998                          |
| Version          | 2.0                                        |
| Previous version | 1.0 — incomplete, contained factual errors |

### Revision History

| Rev | Date       | Description                                                   |
|-----|------------|---------------------------------------------------------------|
| 1.0 | 2026-05-25 | Initial draft — high-level AMP overview                       |
| 2.0 | 2026-06-02 | Full rewrite — aligned with DEV1 implementation; corrected LittleFS terminology; added hardware constraints, Write-First contract, thread safety model, Captive Portal flow |

---

## 1. Hardware Platform

### 1.1 Target Device

| Parameter         | Value                              |
|-------------------|------------------------------------|
| SoC               | ESP32-S3FN8                        |
| CPU               | Dual-core Xtensa LX7 @ 240MHz      |
| Flash             | 8MB embedded (GD25Q64)             |
| SRAM              | 512KB internal + 8MB PSRAM (N/A)   |
| Board             | Heltec WiFi LoRa 32 V3             |
| USB-UART bridge   | CP2102 / CH340 (board-dependent)   |

### 1.2 Pin Mapping

| Function       | GPIO | Direction | Notes                                    |
|----------------|------|-----------|------------------------------------------|
| OLED SDA       | 17   | Output    | I2C data — SSD1306 display               |
| OLED SCL       | 18   | Output    | I2C clock — SSD1306 display              |
| OLED RST       | 21   | Output    | Active-low reset pulse on boot           |
| OLED VEXT      | 36   | Output    | Active-low power enable for OLED supply  |
| Sensor Input   | 4    | Input     | Pull-down; ADC1 channel — safe with Wi-Fi |
| Control Output | 2    | Output    | Mirrors GPIO 4 state; initially LOW      |

### 1.3 Critical Hardware Constraint — ADC1 vs ADC2

The ESP32-S3 silicon has a documented conflict: **ADC2 shares internal circuitry
with the Wi-Fi RF front-end**. When Wi-Fi is active, ADC2 readings are unreliable
and can cause driver crashes.

**Architectural decision:** All analog acquisition uses ADC1 exclusively.
GPIO 4 (ADC1_CH3) was selected for this reason. ADC2 channels are permanently
excluded from the pin mapping.

This constraint is documented in the ESP32-S3 Technical Reference Manual,
Section 5.3 (ADC) and confirmed in the ESP-IDF API reference.

---

## 2. Asymmetric Multiprocessing (AMP)

### 2.1 Design Rationale

A single-core sequential firmware cannot simultaneously guarantee hard real-time
sensor sampling and handle the blocking, unpredictable latency of TCP/IP and MQTT.
The ESP32-S3 dual-core architecture solves this by strict plane separation.

### 2.2 Core Assignment

```
┌─────────────────────────────┐   ┌─────────────────────────────┐
│        Core 0 (PRO_CPU)     │   │        Core 1 (APP_CPU)     │
│         DATA PLANE          │   │        NETWORK PLANE         │
├─────────────────────────────┤   ├─────────────────────────────┤
│ • FSM tick (50ms)           │   │ • Wi-Fi stack               │
│ • GPIO sampling (ADC1)      │   │ • MQTT client (QoS 1)       │
│ • OLED display (I2C)        │   │ • Store-and-Forward drain    │
│ • LittleFS append           │   │ • Exponential backoff        │
│ • storage_append()          │   │ • storage_read_next()        │
│                             │   │ • storage_pop_synced()       │
└─────────────────────────────┘   └─────────────────────────────┘
           ▲                                    ▲
           │        Shared Resource             │
           └──────── FreeRTOS Mutex ────────────┘
                   (storage_driver.c)
```

### 2.3 Current State (DEV1)

The AMP split is architecturally defined but not yet task-pinned. In DEV1,
all code runs in `app_main()`, which ESP-IDF schedules on Core 1 by default.
Core pinning is implemented in DEV3 when Wi-Fi and MQTT FreeRTOS tasks are
created with `xTaskCreatePinnedToCore()`.

### 2.4 Wi-Fi Core Configuration (DEV3)

By default, the ESP-IDF Wi-Fi stack runs on Core 0. To enforce the AMP contract,
DEV3 will set:

```
CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1=y
```

This moves the Wi-Fi driver to Core 1, freeing Core 0 exclusively for the
data plane.

---

## 3. Flash Memory Layout

```
ESP32-S3FN8 — 8MB Flash
┌──────────────────────────────────────────────┐
│ 0x000000  Bootloader            (~21 KB)      │
│ 0x008000  Partition Table       (4 KB)        │
│ 0x009000  nvs          data/nvs (16 KB)       │
│ 0x00D000  phy_init     data/phy (4 KB)        │
│ 0x010000  factory      app      (2 MB)  ◄──── │ idf.py flash writes here
│ 0x210000  lfs          data     (2 MB)  ◄──── │ LittleFS — NEVER touched by flash
└──────────────────────────────────────────────┘
  Total used: ~4.1MB of 8MB
  Available:  ~3.9MB (reserved for future OTA partitions)
```

**Key property:** `idf.py flash` writes only bootloader, partition table, and
factory app. The `lfs` partition at `0x210000` is never overwritten during a
firmware update, guaranteeing telemetry data survives field maintenance.

**OTA note:** The partition layout intentionally leaves 3.9MB free. A future v2.0
OTA implementation requires replacing `factory` with `ota_0` + `ota_1` + `otadata`
partitions. This is a planned breaking change, documented here to avoid
architectural surprise.

---

## 4. Finite State Machine (FSM)

### 4.1 State Diagram

```
                    ┌─────────┐
          Boot      │         │  Credentials
        (no NVS) ──►│ CONFIG  │──────────────────────────────┐
                    │         │                               │
                    └─────────┘                               ▼
                                                       ┌─────────────┐
                    ┌──────────────────────────────────►│             │
                    │        Wi-Fi restored             │  OPERATION  │◄── Normal
                    │                                   │             │    telemetry
                    │                                   └──────┬──────┘
                    │                                          │
                    │                                    Wi-Fi lost
                    │                                          │
                    │                                          ▼
                    │                                   ┌─────────────┐
                    │                                   │             │
                    └───────────────────────────────────│  EMERGENCY  │
                             RESYNC complete            │             │
                                                        └──────┬──────┘
                                                               │
                                                         Wi-Fi restored
                                                               │
                                                               ▼
                                                        ┌─────────────┐
                                                        │             │
                                                        │   RESYNC    │
                                                        │             │
                                                        └─────────────┘
```

### 4.2 State Descriptions

#### `SYS_MODE_CONFIG`
- **Entry condition:** NVS does not contain valid Wi-Fi credentials on boot.
- **Core 1 behavior:** Wi-Fi starts in SoftAP mode. SSID: `"resilient-iot-cfg"`.
  HTTP server on `192.168.4.1` serves the credential provisioning portal.
- **Core 0 behavior:** OLED shows "FSM: CONFIG MODE". Sensor sampling suspended
  until transition to OPERATION.
- **Exit condition:** User submits valid SSID + password via Captive Portal.
  Device: (1) validates credentials by attempting STA connection, (2) saves to NVS
  on success, (3) disables AP mode, (4) transitions to OPERATION.

#### `SYS_MODE_OPERATION`
- **Entry condition:** Valid NVS credentials present on boot, or successful
  Captive Portal provisioning.
- **Core 0 behavior:** GPIO sampling every 50ms. OLED updates on state change.
  Periodic telemetry record appended to LittleFS every 5s (DEV1 baseline;
  interval will be configurable in DEV5).
- **Core 1 behavior:** Wi-Fi connected in STA mode. MQTT publishing real-time
  telemetry with QoS 1.
- **Exit condition:** Wi-Fi disconnect event → EMERGENCY.

#### `SYS_MODE_EMERGENCY`
- **Entry condition:** Wi-Fi disconnect detected on Core 1.
- **Core 0 behavior:** Write-First policy activates at full rate. Every telemetry
  sample is appended to `/lfs/telemetry.log` via `storage_append()`. OLED shows
  "FSM: EMERGENCY".
- **Core 1 behavior:** Exponential backoff reconnection loop
  (1s → 2s → 4s → ... → 60s cap). No MQTT publish attempted.
- **Exit condition:** Successful Wi-Fi reconnection → RESYNC.

#### `SYS_MODE_RESYNC`
- **Entry condition:** Wi-Fi reconnected after EMERGENCY.
- **Core 0 behavior:** Continues sensor sampling and appending new records.
  Does NOT modify the existing pending queue — only appends new records after
  the current `sync_cursor` position.
- **Core 1 behavior:** Drains `/lfs/telemetry.log` via `storage_read_next()` +
  `storage_pop_synced()` in a loop. Each record is published via MQTT QoS 1
  and popped only after PUBACK is received.
- **Exit condition:** `storage_get_pending_count() == 0` → OPERATION.

### 4.3 FSM Tick

The FSM runs at a fixed 50ms interval via `vTaskDelay(pdMS_TO_TICKS(50))`.
`CONFIG_FREERTOS_HZ=1000` provides 1ms scheduler resolution, ensuring the
50ms tick is deterministic within ±1ms jitter.

---

## 5. Write-First Policy

### 5.1 Contract

> Every telemetry record MUST be committed to non-volatile storage BEFORE
> any network transmission attempt. A record that exists only in RAM is
> considered lost.

This contract is enforced at the API level: `storage_append()` must return
`ESP_OK` before any call to the MQTT publish function.

### 5.2 Record Format (NDJSON)

Each telemetry record is one JSON object per line, terminated by `\n`.
This format (Newline-Delimited JSON) allows sequential reading with `fgets()`
and is human-readable for debugging.

```json
{"uptime_ms":5000,"gpio":1,"state":"OPERATION","seq":1}
{"uptime_ms":10000,"gpio":1,"state":"OPERATION","seq":2}
{"uptime_ms":15000,"gpio":0,"state":"EMERGENCY","seq":3}
```

File path: `/lfs/telemetry.log`
Maximum file size: 1.8MB (safety threshold before 2MB partition limit).

### 5.3 FIFO Queue Implementation

The storage driver implements a FIFO queue over the NDJSON log file using
an in-memory sync cursor:

```
/lfs/telemetry.log (on Flash)
┌────────────────────────────────────────────────────┐
│ record_001\n  ◄── sync_cursor = 0 (start)          │
│ record_002\n                                        │
│ record_003\n  ◄── sync_cursor advances here after  │
│ record_004\n      storage_pop_synced() × 2          │
│ record_005\n  ◄── new records appended by Core 0   │
└────────────────────────────────────────────────────┘

storage_read_next()  → reads from sync_cursor position
storage_pop_synced() → advances sync_cursor past current record
                       when pending_count reaches 0: file is truncated
```

### 5.4 Delivery Guarantee

The sync cursor is stored in RAM only — it is not persisted to NVS.
On reboot, `sync_cursor` resets to 0, making all file records pending again.

This implements **at-least-once delivery**. Duplicate records may be
published after a reboot during RESYNC. The MQTT broker deduplicates using:
- QoS 1 PUBACK protocol
- The `seq` field in the JSON payload (application-level deduplication)

### 5.5 Power-Loss Safety

LittleFS uses a Copy-on-Write (CoW) journaling mechanism. A power loss during
`fwrite()` will result in either:
- The complete record being committed, or
- The previous state of the file being preserved.

Partial writes that corrupt the file are structurally impossible with LittleFS.
This was validated in TC-08 (1 power-loss cycle observed).

---

## 6. Thread Safety Model

All storage driver operations are guarded by a single FreeRTOS mutex
(`SemaphoreHandle_t`). Timeout values are calibrated to the FSM tick:

| Function              | Caller  | Mutex timeout | Rationale                        |
|-----------------------|---------|---------------|----------------------------------|
| `storage_append()`    | Core 0  | 50ms          | Must not block FSM tick          |
| `storage_read_next()` | Core 1  | 100ms         | Network latency tolerance        |
| `storage_pop_synced()`| Core 1  | 100ms         | Network latency tolerance        |
| `storage_init()`      | Core 1  | N/A (startup) | Single-threaded at boot          |

If `storage_append()` times out (mutex held by Core 1 for > 50ms), the record
is dropped and `ESP_ERR_TIMEOUT` is returned. This is logged as a critical error.
In practice, Core 1 RESYNC operations complete in < 5ms per record.

---

## 7. Captive Portal Provisioning Flow

### 7.1 Sequence

```
Boot (NVS empty)
      │
      ▼
  CONFIG state
      │
      ├── Core 1: wifi_init_softap("resilient-iot-cfg")
      ├── Core 1: http_server_start() on 192.168.4.1
      └── Core 0: OLED "FSM: CONFIG MODE"

Field technician:
      │
      ├── Connects phone/tablet to "resilient-iot-cfg"
      ├── Browser opens 192.168.4.1 (DNS redirect)
      └── Submits SSID + password via HTML form

Device on form submission:
      │
      ├── Validates: attempt STA connection with submitted credentials
      ├── On success:
      │     ├── nvs_set_str("wifi_ssid", ssid)
      │     ├── nvs_set_str("wifi_pass", password)
      │     ├── esp_wifi_set_mode(WIFI_MODE_STA)  ← disables AP
      │     └── FSM: CONFIG → OPERATION
      └── On failure:
            └── HTTP response: "Invalid credentials. Try again."
```

### 7.2 Subsequent Boots

On every boot after successful provisioning:

```
Boot
  │
  ├── storage_init() reads NVS
  ├── NVS has valid credentials → skip CONFIG
  └── FSM starts directly in OPERATION
        └── Core 1: wifi_init_sta(nvs_ssid, nvs_pass)
```

---

## 8. Component Dependencies

### 8.1 IDF Component Manager (`main/idf_component.yml`)

| Component              | Version  | Purpose                              |
|------------------------|----------|--------------------------------------|
| `idf`                  | >=6.0    | ESP-IDF framework                    |
| `joltwallet/littlefs`  | 1.21.1   | LittleFS VFS driver for ESP-IDF      |

MQTT will be added in DEV4:

| Component              | Version  | Purpose                              |
|------------------------|----------|--------------------------------------|
| `espressif/esp-mqtt`   | >=2.0.0  | MQTT client (built-in in IDF 5.x, external in IDF 6.x) |

### 8.2 Why `joltwallet/littlefs`

The ESP-IDF built-in filesystem options are SPIFFS (deprecated, no directory
support, wear-leveling issues) and FAT (heavy, not power-loss safe without
transaction support). `joltwallet/littlefs` provides:

- Power-loss safe CoW journaling
- Static RAM usage (no heap fragmentation)
- POSIX VFS integration (standard `fopen`/`fwrite`/`fgets`)
- Active maintenance and ESP-IDF 6.x compatibility confirmed (v1.21.1)

---

## 9. Build Configuration (`sdkconfig.defaults`)

| Config key                          | Value  | Rationale                                           |
|-------------------------------------|--------|-----------------------------------------------------|
| `CONFIG_PARTITION_TABLE_CUSTOM`     | y      | Enables custom `partitions.csv`                     |
| `CONFIG_ESPTOOLPY_FLASHSIZE_8MB`    | y      | Matches ESP32-S3FN8 embedded flash                  |
| `CONFIG_FREERTOS_HZ`                | 1000   | 1ms tick resolution for 50ms FSM determinism        |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE`   | 8192   | LittleFS `fopen` peaks require > 3584 byte default  |
| `CONFIG_ESP_TASK_WDT_INIT`          | y      | Hardware watchdog enabled — mandatory for field use  |
| `CONFIG_ESP_TASK_WDT_TIMEOUT_S`     | 30     | Development value; reduce to 5s before DEV8         |
| `CONFIG_COMPILER_OPTIMIZATION_DEFAULT` | y   | `-Og` preserves debug symbols and stack frames      |
| `CONFIG_I2C_SUPPRESS_DEPRECATE_WARN`| y      | Legacy I2C driver (EOL in IDF 6.x); migration tracked below |

---

## 10. Known Limitations and Technical Debt

| Item | Impact | Resolution |
|------|--------|------------|
| Legacy I2C driver (`driver/i2c.h`) | Warning on every build; scheduled removal in IDF 7.0 | Migrate `oled_send_cmd/data` to `driver/i2c_master.h` before DEV8 |
| `sync_cursor` not persisted to NVS | At-least-once delivery; duplicates possible after reboot | Acceptable: QoS 1 + `seq` deduplication. Persist cursor in DEV6 if required |
| FSM not yet task-pinned to cores | AMP contract not enforced until DEV3 | `xTaskCreatePinnedToCore()` in DEV3; `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1=y` |
| MQTT component not built-in in IDF 6.x | DEV4 build will fail without `espressif/esp-mqtt` in `idf_component.yml` | Add dependency at start of DEV4 |
| Sensor is GPIO digital, not ADC | TC-01 (ADC1 validation) is blocked | ADC driver planned for v2.0 |

---

## 11. Development Roadmap

| DEV  | Module                  | Core(s) affected | Key deliverables                              |
|------|-------------------------|------------------|-----------------------------------------------|
| DEV1 | Storage Driver          | 0                | LittleFS, Write-First, FIFO queue ✅ COMPLETE |
| DEV2 | Store-and-Forward Queue | 0                | Metadata, sequence numbers, queue management  |
| DEV3 | Wi-Fi + Captive Portal  | 1                | STA/AP modes, backoff, NVS provisioning       |
| DEV4 | MQTT Client             | 1                | QoS 1, Will message, esp-mqtt component       |
| DEV5 | FSM Complete            | 0 + 1            | Full 4-state transitions, core pinning        |
| DEV6 | RESYNC                  | 1                | Store-and-Forward drain, pop on PUBACK        |
| DEV7 | Display                 | 0                | FSM state indicators, signal strength         |
| DEV8 | BDD Tests               | 0 + 1            | Fault injection, 72h stability, TC completion |
| DEV9 | Monograph               | —                | Academic text, IEEE 830-1998 final document   |