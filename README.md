# Resilient IoT Edge

> **Reference firmware for resilient IoT systems with local persistence in LittleFS and asynchronous synchronization via MQTT.**
> Designed to operate on dual-core hardware (ESP32-S3), aiming to mitigate data loss during severe connectivity failures.

![Status](https://img.shields.io/badge/Status-Active%20Development-success)
![Hardware](https://img.shields.io/badge/Hardware-ESP32--S3-orange)
![RTOS](https://img.shields.io/badge/OS-FreeRTOS-green)
![CI/CD](https://img.shields.io/badge/Build-Passing-brightgreen)

## Overview
This project implements an Industrial Edge Computing architecture focused on uninterrupted telemetry using a Finite State Machine (FSM) optimized for asymmetric dual-core execution:

* **Core 0 (Data Plane):** Dedicated to high-priority, deterministic tasks: real-time signal acquisition (GPIO telemetry) and local persistence writing.

* **Core 1 (Control & Network Plane):** Dedicated to asynchronous, heavy network operations: Wi-Fi stack management, Captive Portal hosting, and MQTT synchronization.

## Operational Modes (System FSM)
The firmware monitors connectivity events and storage metrics to transition seamlessly between 4 operational states:

* 1. **Configuration Mode (SYS_MODE_CONFIG):** Active upon first boot or connection failure timeout. Core 1 initiates an Access Point (AP) and hosts a Captive Portal for network provisioning.

* 2. **Operation Mode (SYS_MODE_OPERATION):** The standard "online" state. Core 0 samples inputs in real-time, while Core 1 streams packets immediately to the cloud via MQTT 5.0.

* 3. **Emergency / Offline Mode (SYS_MODE_EMERGENCY):** Triggered autonomously upon detecting network/broker blackout. Core 0 intercepts the data stream and spools telemetry into a non-volatile circular buffer inside LittleFS, avoiding data loss.

* 4. **Resynchronization Mode (SYS_MODE_RESYNC):** Triggered as soon as connection is re-established. Core 1 runs a background Store-and-Forward algorithm to drain historical logs from LittleFS, while Core 0 continues acquiring real-time data with zero jitter.

## Technology Stack & Hardware
* **Microcontroller:** Heltec WiFi LoRa 32 V3 (ESP32-S3FN8 Dual-Core Xtensa LX7)
* **Framework:** C / ESP-IDF (Espressif IoT Development Framework) running on FreeRTOS
* **Protocols:** Wi-Fi 802.11 b/g/n, Captive Portal (DNS Redirect + HTTP), MQTT 5.0 (QoS 1)
* **Storage:** LittleFS for embedded Flash with Wear Leveling

### Hardware Notes (Heltec V3)
The following pinout configurations were used for this board:
* **OLED Display Pinout:** `SDA: 17`, `SCL: 18`, `RST: 21`, `Vext (Power): 36` (Active LOW).
* **Serial Monitor (USB CDC):** Configured natively via ESP-IDF menuconfig (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) to prevent boot crashes.

## Repository Structure
```text
resilient-edge-iot/
├── .github/workflows/     # CI/CD: ESP-IDF Docker build automation
├── docs/
│   ├── architecture/      # State diagrams and multithreading design
│   └── tests/             # QA: Test Cases and Validation Plan (IEEE 829)
├── main/                  # Main FSM logic, RTOS tasks, and internal components
├── scripts/               # Auxiliary tools for log collection and simulation
├── CMakeLists.txt         # Root CMake configuration
└── sdkconfig.defaults     # Core hardware and RTOS optimizations
```

## Development Status
- [x] Phase 1 (Setup & Planning): Repository structuring, architecture definition, and test plan creation.

- [x] Phase 2 (Hardware & RTOS Base): Pinout validation (Heltec V3), USB Serial stabilization, and Multi-Core skeleton implementation (FreeRTOS).

- [ ] Phase 3 (Local Storage): LittleFS configuration and circular buffer implementation (Core 0).

- [ ] Phase 4 (Connectivity): Wi-Fi/MQTT integration and network failure detection routine (Core 1).

- [ ] Phase 5 (Synchronization): Store-and-Forward logic development for sending backlogged data post-failure.

- [ ] Phase 6 (Final Validation): Practical tests simulating network blackouts and integration with orchestrator (Node-RED).