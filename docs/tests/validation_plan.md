# Software Verification & Validation Plan (SVVP)
## Resilient IoT Edge — ESP32-S3FN8

---

## Document Information

| Field              | Value                                         |
|--------------------|-----------------------------------------------|
| Project            | Resilient IoT Edge                            |
| Hardware           | ESP32-S3FN8 — Heltec WiFi LoRa 32 V3         |
| Toolchain          | ESP-IDF 6.0.1 + FreeRTOS (native)            |
| Standard           | IEEE Std 830-1998                             |
| Version            | 2.1                                           |
| Previous version   | 2.0 — Captive Portal incorrectly out of scope |

### Revision History

| Rev | Date       | Description                                               |
|-----|------------|-----------------------------------------------------------|
| 1.0 | 2026-05-04 | Initial plan — theoretical TCs, pre-implementation        |
| 2.0 | 2026-06-02 | Full rewrite — aligned with DEV1 implementation reality    |
| 2.1 | 2026-06-02 | Captive Portal restored to scope; TC-23 added; DEV3 scope updated |

---

## Status Legend

| Symbol          | Meaning                                                      |
|-----------------|--------------------------------------------------------------|
| ✅ PASSED       | All acceptance criteria fully met and observed on hardware   |
| ⚠️ PARTIAL      | Partially validated — full validation deferred to DEV8       |
| ⏳ PENDING      | Not yet executable — implementing DEV not complete           |
| 🔴 BLOCKED      | Cannot execute — architectural dependency not implemented    |
| ❌ OUT OF SCOPE  | Removed from v1.0 — documented for future versions          |

---

## Phase 2 — Hardware & RTOS Baseline

> **Implementing DEV:** Baseline (Fase 2 — pre-DEV1)
> **Phase status:** ✅ Complete

### TC-01: Build and Flash

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify the project compiles and flashes without errors on ESP-IDF 6.x |
| **Acceptance**      | `idf.py build` exits code 0; binary flashes successfully via UART |
| **Status**          | ✅ PASSED |
| **Observed result** | Binary: 0x31050 bytes (10% of 2MB factory partition). GCC 15.2, joltwallet/littlefs 1.21.1 resolved automatically. Bootloader: 0x5260 bytes (36% of reserved area). |

---

### TC-02: OLED Display Initialization

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify bare-metal I2C SSD1306 driver initializes and renders correctly |
| **Acceptance**      | "RESILIENT IOT EDGE" renders on page 0 within 500ms of boot |
| **Status**          | ✅ PASSED |
| **Observed result** | Display renders correctly. VEXT power sequence confirmed stable: 100ms LOW → 50ms RST LOW → 100ms RST HIGH. Anti-flicker guard (last_state comparison) prevents redundant I2C writes. |

---

### TC-03: FSM Deterministic Loop

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify the FSM tick runs at 50ms on the RTOS scheduler with no starvation |
| **Acceptance**      | s_tick increments at 20Hz (±5ms jitter); no RTOS deadlock observed |
| **Status**          | ✅ PASSED |
| **Observed result** | FSM loop stable at 50ms via `vTaskDelay(pdMS_TO_TICKS(50))`. `CONFIG_FREERTOS_HZ=1000` provides 1ms tick resolution. GPIO sampling and OLED updates execute within the same tick window. |

---

### TC-04: GPIO Digital I/O

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify GPIO 4 (input, pull-down) mirrors state to GPIO 2 (output) within one FSM tick |
| **Acceptance**      | GPIO 2 changes state within 50ms of GPIO 4; OLED updates accordingly |
| **Status**          | ✅ PASSED |
| **Observed result** | Output mirrors input with max 50ms latency. OLED shows "GPIO 4 = HIGH (SIGN)" / "GPIO 2 = HIGH (ON)" on transition. Anti-flicker guard confirmed operational. |

---

## Phase 3 — Local Storage / Write-First Policy

> **Implementing DEV:** DEV1 (LittleFS Storage Driver)
> **Phase status:** ⚠️ Functionally complete — stress test deferred to DEV8

### TC-05: LittleFS Mount and Initialization

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify the LittleFS partition mounts correctly on first boot |
| **Acceptance**      | `storage_init()` returns `ESP_OK`; log shows partition >= 2048 KB |
| **Status**          | ✅ PASSED |
| **Observed result** | Serial log: "LittleFS: 2048 KB total | 4 KB used | 2044 KB free". Partition label `lfs` matches `partitions.csv`. `format_if_mount_failed=true` confirmed on first flash after `erase-flash`. |

---

### TC-06: Fail-Fast on Mount Error

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify the system reboots with OLED feedback if LittleFS fails to mount |
| **Acceptance**      | OLED shows "FS MOUNT ERROR!"; device reboots after 5s delay |
| **Status**          | ⚠️ PARTIAL |
| **Observed result** | Code path implemented, reviewed, and statically validated. Fault-injection test (deliberate partition corruption) deferred to DEV8 (TC-21). |

---

### TC-07: Write-First Policy (Periodic Append)

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify telemetry records are written to LittleFS at every scheduled interval |
| **Acceptance**      | OLED counter increments every 5s; NDJSON records confirmed in `/lfs/telemetry.log` |
| **Status**          | ✅ PASSED |
| **Observed result** | OLED shows "STORE: 00001 PEND" incrementing every 5s (100x50ms ticks). Record format confirmed: `{"uptime_ms":<u32>,"gpio":<0|1>,"state":"OPERATION","seq":<u32>}`. Mutex-guarded `storage_append()` executed without timeout. |

---

### TC-08: Power-Loss Recovery

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify LittleFS data integrity survives complete power loss during active operation |
| **Acceptance**      | After each reset, `pending_count` matches pre-reset OLED value; no file corruption; OLED shows recovery state |
| **Status**          | ⚠️ PARTIAL — 1/10 cycles executed |
| **Observed result** | Cycle 1: Serial log showed "[RECOVERY] X record(s) pending from previous session" matching pre-reset OLED count exactly. OLED displayed "RECOVERY ACTIVE" on boot, followed by correct "STORE: XXXXX PEND". LittleFS Copy-on-Write (CoW) mechanism confirmed operational. Full 10-cycle stress test scheduled as TC-21 in DEV8. |

---

### TC-09: Storage Near-Full Protection

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify `storage_append()` rejects records when log file exceeds 1.8MB |
| **Acceptance**      | `ESP_ERR_NO_MEM` returned; "STORAGE FULL" logged; no filesystem corruption |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV8 — requires fault-injection harness to fill 1.8MB partition |

---

## Phase 4 — Network Plane (Wi-Fi + Captive Portal + MQTT)

> **Implementing DEV:** DEV3 (Wi-Fi + Captive Portal) + DEV4 (MQTT)
> **Phase status:** ⏳ Pending

### TC-10: Wi-Fi Initial Connection (STA Mode)

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify Wi-Fi connects to configured SSID on Core 1 without blocking Core 0 |
| **Acceptance**      | IP assigned within 10s; FSM remains in OPERATION; s_tick jitter <= 5ms during handshake |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV3 |

---

### TC-11: Wi-Fi Reconnection with Exponential Backoff

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify FSM transitions to EMERGENCY on disconnect and reconnects with backoff |
| **Acceptance**      | EMERGENCY state within one FSM tick of disconnect; reconnection within 60s; backoff intervals logged (1s -> 2s -> 4s -> ... -> 60s cap) |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV3 |

---

### TC-12: MQTT QoS 1 Publish with ACK

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify MQTT client publishes real-time telemetry with QoS 1 and waits for PUBACK |
| **Acceptance**      | PUBACK received for every publish; no message loss on clean broker disconnect |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV4 |

---

### TC-13: MQTT Will Message on Ungraceful Disconnect

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify broker receives Last Will message when device loses power ungracefully |
| **Acceptance**      | Will topic receives payload within broker keep-alive timeout window |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV4 |

---

### TC-23: Captive Portal Initial Configuration

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify the device provisions Wi-Fi credentials via SoftAP Captive Portal when NVS is empty |
| **Acceptance**      | (1) Device boots in CONFIG state; SoftAP "resilient-iot-cfg" visible to nearby devices. (2) Technician connects and accesses portal at 192.168.4.1 within 30s. (3) After submitting valid SSID + password: device disables AP, connects as STA, saves credentials to NVS, transitions to OPERATION — all within 3 minutes. (4) On subsequent reboot, device skips CONFIG and connects directly using saved NVS credentials. |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV3 |

---

## Phase 5 — FSM Complete (4-State Transitions)

> **Implementing DEV:** DEV5
> **Phase status:** ⏳ Pending

### TC-14: CONFIG State — No-Credentials Boot

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify FSM boots in CONFIG when NVS credentials are absent |
| **Acceptance**      | OLED shows "FSM: CONFIG MODE"; SoftAP "resilient-iot-cfg" visible; no STA connection attempted |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV5 |

---

### TC-15: OPERATION → EMERGENCY Transition

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify FSM transitions to EMERGENCY within one tick on Wi-Fi loss |
| **Acceptance**      | State change within 50ms; OLED shows "FSM: EMERGENCY"; `storage_append()` activates for every subsequent record |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV5 |

---

### TC-16: EMERGENCY → RESYNC Transition

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify FSM transitions to RESYNC within 5s of Wi-Fi restoration |
| **Acceptance**      | RESYNC state active on reconnect; Store-and-Forward drain begins on Core 1 |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV5 |

---

## Phase 6 — Store-and-Forward Resync

> **Implementing DEV:** DEV6
> **Phase status:** ⏳ Pending

### TC-17: Historical Record Drain

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify all pending LittleFS records are published via MQTT in RESYNC mode |
| **Acceptance**      | `pending_count` reaches 0 after RESYNC; records published in chronological order (ascending `seq`) |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV6 |

---

### TC-18: No Duplicate Records (QoS 1 Deduplication)

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify no duplicate records appear on the broker after a RESYNC cycle |
| **Acceptance**      | Broker receives exactly N unique records matching N pending records; `seq` values non-repeating |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV6 |

---

## Phase 7 — Stability & Full Validation

> **Implementing DEV:** DEV8 (BDD + Fault Injection)
> **Phase status:** ⏳ Pending

### TC-19: 72-Hour Uptime Stability

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify system stability over a continuous 72-hour operational run |
| **Acceptance**      | Zero Task Watchdog triggers; device remains in OPERATION or RESYNC throughout |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV8 |

---

### TC-20: Heap Memory Leak Audit

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify heap usage remains stable over the 72-hour run |
| **Acceptance**      | Heap leak <= 1 KB/hour measured via `esp_get_free_heap_size()`; no heap exhaustion event |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV8 |

---

### TC-21: Full Power-Loss Stress Test (10 Cycles)

| Field               | Value |
|---------------------|-------|
| **Objective**       | Complete TC-08 validation with 10 random power-loss cycles |
| **Acceptance**      | All 10 cycles: `pending_count` correct after recovery; `telemetry.log` not corrupted; OLED shows correct recovery count |
| **Status**          | ⏳ PENDING — 1/10 cycles validated (see TC-08) |
| **Blocking DEV**    | DEV8 |

---

### TC-22: AMP Core Isolation Under Load

| Field               | Value |
|---------------------|-------|
| **Objective**       | Verify Core 0 (data plane) maintains 50ms determinism while Core 1 (network plane) is under active MQTT publish load |
| **Acceptance**      | s_tick jitter <= 5ms during simultaneous MQTT publish; no I2C bus contention or LittleFS mutex starvation |
| **Status**          | ⏳ PENDING |
| **Blocking DEV**    | DEV8 |

---

## Out of Scope — v1.0

| Feature               | Reason                                                                 |
|-----------------------|------------------------------------------------------------------------|
| ADC1 analog sensor    | GPIO digital input used for baseline validation; ADC driver planned v2.0 |
| OTA firmware update   | Partition layout supports future OTA; excluded from v1.0 by design     |

---

## Traceability Matrix

| TC    | Description                           | Phase | DEV      | Status      |
|-------|---------------------------------------|-------|----------|-------------|
| TC-01 | Build and Flash                       | 2     | Baseline | ✅ PASSED   |
| TC-02 | OLED Display Initialization           | 2     | Baseline | ✅ PASSED   |
| TC-03 | FSM Deterministic Loop (50ms)         | 2     | Baseline | ✅ PASSED   |
| TC-04 | GPIO Digital I/O                      | 2     | Baseline | ✅ PASSED   |
| TC-05 | LittleFS Mount and Initialization     | 3     | DEV1     | ✅ PASSED   |
| TC-06 | Fail-Fast on Mount Error              | 3     | DEV1     | ⚠️ PARTIAL  |
| TC-07 | Write-First Policy (Periodic Append)  | 3     | DEV1     | ✅ PASSED   |
| TC-08 | Power-Loss Recovery (1/10 cycles)     | 3     | DEV1     | ⚠️ PARTIAL  |
| TC-09 | Storage Near-Full Protection          | 3     | DEV8     | ⏳ PENDING  |
| TC-10 | Wi-Fi Initial Connection (STA)        | 4     | DEV3     | ⏳ PENDING  |
| TC-11 | Wi-Fi Reconnection + Backoff          | 4     | DEV3     | ⏳ PENDING  |
| TC-12 | MQTT QoS 1 Publish with ACK           | 4     | DEV4     | ⏳ PENDING  |
| TC-13 | MQTT Will Message on Disconnect       | 4     | DEV4     | ⏳ PENDING  |
| TC-14 | CONFIG State — No Credentials         | 5     | DEV5     | ⏳ PENDING  |
| TC-15 | OPERATION -> EMERGENCY Transition     | 5     | DEV5     | ⏳ PENDING  |
| TC-16 | EMERGENCY -> RESYNC Transition        | 5     | DEV5     | ⏳ PENDING  |
| TC-17 | Historical Record Drain (RESYNC)      | 6     | DEV6     | ⏳ PENDING  |
| TC-18 | No Duplicate Records (QoS 1)          | 6     | DEV6     | ⏳ PENDING  |
| TC-19 | 72-Hour Uptime Stability              | 7     | DEV8     | ⏳ PENDING  |
| TC-20 | Heap Memory Leak Audit                | 7     | DEV8     | ⏳ PENDING  |
| TC-21 | Full Power-Loss Stress Test (10x)     | 7     | DEV8     | ⏳ PENDING  |
| TC-22 | AMP Core Isolation Under Load         | 7     | DEV8     | ⏳ PENDING  |
| TC-23 | Captive Portal Initial Configuration  | 4     | DEV3     | ⏳ PENDING  |