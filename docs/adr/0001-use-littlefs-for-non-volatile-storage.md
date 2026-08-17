# 1. Use LittleFS for Write-First Storage

* Status: Accepted
* Date: 2026-06-02

## Context
The firmware requires safe local persistence against power loss during
prolonged connectivity outages, following the Write-First policy: every
telemetry record must be committed to non-volatile storage before any
network transmission attempt. The ESP-IDF native filesystem options were
SPIFFS (deprecated, no directory support, known wear-leveling issues) and
FAT (heavyweight for this footprint, no power-loss safety without
transaction support).

## Decision
We use LittleFS, via the `joltwallet/littlefs` component (v1.21.1), as the
filesystem for the dedicated `lfs` partition (2MB, isolated from the
`factory` app partition — a firmware update never overwrites telemetry
data).

## Consequences
* **Positive:** Copy-on-Write (CoW) journaling — a write interrupted by
  power loss results in either the complete record or the file's prior
  state, never partial corruption. Validated in TC-08 (1 of 10 planned
  cycles executed to date).
* **Positive:** Static RAM usage, no heap fragmentation.
* **Positive:** Standard POSIX VFS integration (`fopen`/`fwrite`/`fgets`),
  no proprietary API required elsewhere in the storage driver.
* **Negative:** Hard 1.8MB safety ceiling (of 2MB total) before
  `storage_append()` starts rejecting new writes — the write rate chosen
  for each FSM state directly determines how long the device can buffer
  during an outage before data loss begins.
* **Negative:** The sync cursor is kept in RAM only, not persisted to NVS —
  guarantees at-least-once delivery, not exactly-once; duplicate records
  are possible after a reboot mid-resync, deduplication is delegated to
  MQTT QoS 1 plus the `seq` field in the payload.