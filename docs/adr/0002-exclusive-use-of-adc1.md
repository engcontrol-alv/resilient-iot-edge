# 2. Restrict Analog Acquisition to ADC1 Only

* Status: Accepted
* Date: 2026-06-02

## Context
The ESP32-S3 has a documented silicon constraint: ADC2 shares internal
circuitry with the Wi-Fi RF front-end. When Wi-Fi is active, ADC2 readings
become unreliable and can cause driver faults. This is documented in the
ESP32-S3 Technical Reference Manual, Section 5.3 (ADC), and confirmed in
the ESP-IDF API reference. The device depends on Wi-Fi continuously during
`OPERATION`, so this constraint is live for essentially the entire runtime.

## Decision
All analog acquisition in this project uses ADC1 exclusively. ADC2 channels
are permanently excluded from the pin map. GPIO 4 (ADC1_CH3) was selected
for this reason.

## Consequences
* **Positive:** Eliminates the risk of corrupted readings or driver faults
  while Wi-Fi is active.
* **Negative:** Permanently constrains pin choice for any future analog
  sensor to ADC1 channels only, regardless of routing convenience.
* **Note on current implementation status:** the live reading on GPIO 4 is
  digital (`gpio_get_level`), not an analog ADC1 read. This ADR fixes which
  converter to use once analog acquisition is implemented — the ADC1 driver
  itself is planned for v2.0 (see Known Limitations in `system_design.md`;
  ADC1 validation remains blocked until then).