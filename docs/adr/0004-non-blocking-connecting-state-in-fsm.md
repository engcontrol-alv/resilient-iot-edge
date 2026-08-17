# 4. Introduce a Non-Blocking SYS_MODE_CONNECTING State

* Status: Accepted
* Date: 2026-08-13

## Context
The original implementation waited for Wi-Fi connection confirmation
(`WIFI_EVT_CONNECTED`/`WIFI_EVT_DISCONNECTED` bits) via a blocking call
(`xEventGroupWaitBits` with a 15s timeout), both on boot with saved
credentials and after the Captive Portal submits new ones. During this
wait, the main task was fully blocked — no GPIO sampling, OLED updates, or
other tick-based logic ran.

## Decision
Introduce a dedicated, transitional, non-blocking `SYS_MODE_CONNECTING`
state:
1. `wifi_manager_start_sta()` is called and the FSM transitions to
   `SYS_MODE_CONNECTING` immediately, without waiting.
2. Every tick of the main loop (50ms), the `CONNECTING` state performs a
   non-blocking poll of the event bits via `xEventGroupGetBits()`.
3. A 15-second timeout is tracked via `esp_timer_get_time()`, compared
   against the attempt's start timestamp (`connection_start_time`).
4. Transitions: `WIFI_EVT_CONNECTED` → `OPERATION`; `WIFI_EVT_DISCONNECTED`
   or 15s timeout → back to `CONFIG` (reopens the Captive Portal).

## Consequences
* **Positive:** The main loop stays responsive through the entire
  connection attempt — the 50ms tick's determinism is preserved even on
  connection paths.
* **Negative:** Adds a state to the FSM (4 → 5 states), requiring any
  diagram or document that enumerates states (`README.md`,
  `system_design.md`) to be updated to match.
* **Negative:** `connection_start_time` (`uint64_t`) is new `app_main()`
  state with no persistence across reboots — acceptable, since every new
  connection attempt resets it, but worth noting for anyone reasoning about
  the state's lifetime.