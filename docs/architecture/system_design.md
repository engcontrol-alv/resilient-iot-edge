# System Architecture Design

## Core Strategy: Asymmetric Multiprocessing (AMP)
The firmware utilizes the ESP32-S3 dual-core architecture to strictly separate the **Data Plane** from the **Control/Network Plane**. This ensures that heavy Wi-Fi or MQTT operations never block critical sensor readings.

### Core 0 (PRO_CPU) - Data Plane
* **Responsibility:** Hard real-time operations.
* **Tasks:**
  * High-frequency GPIO polling (Sensor data).
  * OLED Display updates (I2C).
  * Spooling data to the LittleFS circular buffer during network blackouts.

### Core 1 (APP_CPU) - Network Plane
* **Responsibility:** Asynchronous and blocking operations.
* **Tasks:**
  * Wi-Fi Stack & Captive Portal.
  * MQTT Client (Publishing/Subscribing).
  * Store-and-Forward synchronization logic.

---

## Finite State Machine (FSM)
The system routes logic through four primary states:
1. `SYS_MODE_CONFIG`: Access Point active, waiting for user credentials.
2. `SYS_MODE_OPERATION`: Normal telemetry streaming.
3. `SYS_MODE_EMERGENCY`: Network lost. Intercepting data to LittleFS.
4. `SYS_MODE_RESYNC`: Connection restored. Draining LittleFS backlog to MQTT.