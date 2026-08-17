# 3. Use 64-Bit Hardware Timers for Absolute Uptime Tracking

* Status: Accepted
* Date: 2026-08-13

## Context
Embedded systems relying on standard 32-bit unsigned millisecond counters
suffer from integer overflow after approximately 49.7 days of continuous
operation. While this wrap-around is well-defined behavior in C and does
not cause a system crash or hardware panic, it leads to silent data
corruption: telemetry timestamps wrap back to a small value, destroying
chronological integrity in the LittleFS storage.

## Decision
We utilize the ESP32 hardware 64-bit timer API (`esp_timer_get_time()`) to
compute absolute uptime for telemetry timestamps, bypassing the local
32-bit tick counter for this purpose.

## Consequences
* **Positive:** Eliminates silent timestamp corruption after 49 days,
  allowing uninterrupted multi-year node operation.
* **Positive:** Provides microsecond-resolution timestamps tied to actual
  elapsed time, rather than a value quantized to the 50ms loop tick — the
  old method (`s_tick * 50UL`) drifted from real elapsed time under any
  loop execution jitter; `esp_timer_get_time()` does not.
* **Negative:** None identified — this is a strict correctness fix with no
  functional trade-off; the `seq` field (derived from `s_tick` directly)
  was not affected by the same overflow at this timescale and required no
  change.