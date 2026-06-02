# Resilient IoT Edge

> **Reference firmware for resilient IoT edge nodes with local telemetry persistence
> and asynchronous cloud synchronization via MQTT.**
> Designed for dual-core hardware (ESP32-S3), engineered to guarantee zero data loss
> during severe and prolonged connectivity failures in remote installations.

![Status](https://img.shields.io/badge/Status-Active%20Development-success)
![Hardware](https://img.shields.io/badge/Hardware-ESP32--S3-orange)
![Framework](https://img.shields.io/badge/Framework-ESP--IDF%206.x-blue)
![RTOS](https://img.shields.io/badge/OS-FreeRTOS-green)
![Build](https://img.shields.io/badge/Build-Passing-brightgreen)

---

## Overview

This project implements an Industrial Edge Computing architecture focused on
uninterrupted telemetry acquisition. The core design principle is the
**Write-First policy**: every sensor reading is committed to non-volatile
storage *before* any network transmission attempt. If connectivity fails,
no data is lost — it is queued locally and synchronized automatically when
the connection is restored.

The firmware is structured around a **Finite State Machine (FSM)** running on
a strict **Asymmetric Multiprocessing (AMP)** model:

| Core | Plane | Responsibilities |
|------|-------|-----------------|
| Core 0 (PRO_CPU) | Data Plane | GPIO sampling, OLED display, LittleFS writes |
| Core 1 (APP_CPU) | Network Plane | Wi-Fi stack, Captive Portal, MQTT, Store-and-Forward |

---

## Operational States (FSM)

The firmware transitions seamlessly between four states based on connectivity
and storage events:

### `SYS_MODE_CONFIG` — First Boot / Provisioning
Active when no Wi-Fi credentials exist in NVS. Core 1 starts a SoftAP
(`resilient-iot-cfg`) and serves an HTTP Captive Portal at `192.168.4.1`.
The field technician connects, submits SSID + password, and the device
switches to STA mode and transitions to OPERATION automatically.

### `SYS_MODE_OPERATION` — Normal Online State
Core 0 samples inputs deterministically at 50ms intervals. Core 1 streams
telemetry to the cloud via MQTT (QoS 1). Write-First policy is active:
every record is written to LittleFS before publishing.

### `SYS_MODE_EMERGENCY` — Network Blackout
Triggered on Wi-Fi disconnect. Core 0 continues sampling and appends every
record to the LittleFS FIFO queue with no data loss. Core 1 runs an
exponential backoff reconnection loop (1s → 2s → 4s → ... → 60s cap).

### `SYS_MODE_RESYNC` — Synchronization Recovery
Triggered on reconnection. Core 1 drains the LittleFS backlog via
Store-and-Forward, publishing each historical record via MQTT QoS 1 and
advancing the sync cursor only after receiving PUBACK. Core 0 continues
real-time acquisition with zero jitter throughout.

---

## Technology Stack

| Layer | Technology |
|-------|-----------|
| Microcontroller | Heltec WiFi LoRa 32 V3 (ESP32-S3FN8, Dual-Core Xtensa LX7 @ 240MHz) |
| Framework | C / ESP-IDF 6.x (native, no Arduino abstraction) |
| RTOS | FreeRTOS — 1000Hz tick, 8KB main task stack |
| Storage | LittleFS (`joltwallet/littlefs` v1.21.1) — power-loss safe CoW journaling |
| Connectivity | Wi-Fi 802.11 b/g/n + MQTT (QoS 1) |
| Provisioning | Captive Portal — SoftAP + HTTP server + NVS credential storage |
| Display | SSD1306 OLED — bare-metal I2C driver (no external library) |

---

## Hardware Notes (Heltec WiFi LoRa 32 V3)

### Pinout

| Function | GPIO | Notes |
|----------|------|-------|
| OLED SDA | 17 | I2C data |
| OLED SCL | 18 | I2C clock |
| OLED RST | 21 | Active-low reset |
| OLED VEXT | 36 | Active-low power enable |
| Sensor Input | 4 | Pull-down; ADC1 channel |
| Control Output | 2 | Mirrors sensor input state |

### ADC Constraint

The ESP32-S3 has a documented silicon conflict: **ADC2 is incompatible with
the Wi-Fi RF front-end**. All analog acquisition uses **ADC1 exclusively**.
This constraint is enforced at the pin mapping level and documented in
`docs/architecture/system_design.md`.

### Serial Console

This firmware uses **UART0** (GPIO 43/44) as the serial console — the default
ESP-IDF configuration for the Heltec V3 board. Do not set
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` on this hardware; the board uses a
USB-UART bridge chip and the native USB peripheral is not connected to the
USB port.

---

## Repository Structure

```text
resilient-iot-edge/
├── docs/
│   ├── architecture/
│   │   └── system_design.md      # AMP, FSM, Write-First, hardware constraints
│   └── tests/
│       └── validation_plan.md    # SVVP — TC-01 to TC-23 (IEEE Std 830-1998)
├── main/
│   ├── main.c                    # FSM loop, OLED driver, hardware init
│   ├── storage_driver.c          # LittleFS Write-First FIFO queue
│   ├── storage_driver.h          # Storage API and constants
│   ├── CMakeLists.txt            # Component build config
│   └── idf_component.yml         # IDF Component Manager manifest
├── partitions.csv                # Flash layout: 2MB factory + 2MB LittleFS
├── sdkconfig.defaults            # RTOS, flash, stack, watchdog config
├── dependencies.lock             # Pinned component versions
└── CMakeLists.txt                # Root project config
```

---

## Development Status

- [x] **Phase 1 — Setup & Planning:** Repository structure, architecture definition,
      SVVP test plan, IEEE 830-1998 documentation baseline.
- [x] **Phase 2 — Hardware & RTOS Base:** Heltec V3 pinout validation, OLED bare-metal
      driver, FSM skeleton at 50ms deterministic tick.
- [x] **Phase 3 — Local Storage:** LittleFS driver, Write-First policy, FIFO queue,
      power-loss recovery validated (TC-05, TC-07, TC-08 partial).
- [ ] **Phase 4 — Connectivity:** Wi-Fi STA/AP, Captive Portal provisioning,
      MQTT client with QoS 1 and Will message.
- [ ] **Phase 5 — Synchronization:** FSM complete 4-state transitions,
      Store-and-Forward drain with PUBACK confirmation.
- [ ] **Phase 6 — Final Validation:** BDD fault-injection tests, 72h stability
      run, TC completion (TC-09 to TC-23).

---

## Getting Started

### Prerequisites

- [ESP-IDF 6.x](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)
- Python 3.x (included with ESP-IDF installer)
- Heltec WiFi LoRa 32 V3 board
- USB cable connected to the board's USB port

### Build and Flash

```bash
# First build — downloads joltwallet/littlefs automatically
idf.py build

# Flash and monitor (adjust port as needed)
idf.py -p COM7 flash monitor
```

### First Boot

On first boot with an empty NVS, the device enters CONFIG state and creates
a Wi-Fi access point named `resilient-iot-cfg`. Connect a phone or laptop
to that network and navigate to `192.168.4.1` to provision Wi-Fi credentials.

### Expected Serial Output (after provisioning)

```
I (xxx) SYSTEM_MAIN: Configuring basic hardware interface...
I (xxx) SYSTEM_MAIN: Initializing OLED display...
I (xxx) SYSTEM_MAIN: Initializing LittleFS Storage...
I (xxx) STORAGE: LittleFS: 2048 KB total | 4 KB used | 2044 KB free
I (xxx) SYSTEM_MAIN: Storage ready. Pending records: 0
I (xxx) SYSTEM_MAIN: [DEV1] Record stored. Pending: 1
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [`docs/architecture/system_design.md`](docs/architecture/system_design.md) | Full architecture: AMP, FSM states, Write-First policy, thread safety, hardware constraints |
| [`docs/tests/validation_plan.md`](docs/tests/validation_plan.md) | SVVP: 23 test cases mapped to implementation phases (IEEE Std 830-1998) |

---

## License

This project is licensed under the MIT License — see [LICENSE](LICENSE) for details.