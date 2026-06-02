/**
 * @file    storage_driver.h
 * @brief   LittleFS Storage Driver — Write-First Policy
 * @version DEV1
 *
 * @details Provides a thread-safe FIFO queue over LittleFS for telemetry
 *          persistence. Enforces the Write-First contract: every telemetry
 *          record is committed to non-volatile storage BEFORE any MQTT
 *          publish attempt.
 *
 *          Record format (NDJSON — one JSON object per line):
 *          {"uptime_ms":<u32>,"gpio":<0|1>,"state":"<FSM>","seq":<u32>}\n
 *
 *          Thread safety:
 *          - storage_append()    → called from Core 0 (Data Plane)
 *          - storage_read_next() → called from Core 1 (Network Plane / RESYNC)
 *          - storage_pop_synced()→ called from Core 1 (Network Plane / RESYNC)
 *          All operations are guarded by an internal FreeRTOS mutex.
 *
 * @author  Eng. Alvaro
 * @see     docs/architecture/system_design.md
 */

#ifndef STORAGE_DRIVER_H
#define STORAGE_DRIVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// =============================================================================
// Configuration Constants
// =============================================================================

/** VFS mount point for the LittleFS partition. */
#define STORAGE_MOUNT_POINT      "/lfs"

/** Partition label — must match partitions.csv Name field exactly. */
#define STORAGE_PARTITION_LABEL  "lfs"

/** Absolute path to the telemetry log file. */
#define STORAGE_LOG_FILE         "/lfs/telemetry.log"

/** Maximum length of a single JSON record line (bytes). */
#define STORAGE_MAX_LINE_LEN     256

/**
 * @brief  Safety threshold: 1.8 MB out of 2 MB partition.
 * @note   When exceeded, new appends are rejected to prevent filesystem
 *         corruption. Triggers an EMERGENCY → RESYNC transition in DEV5.
 */
#define STORAGE_MAX_FILE_SIZE    (1800U * 1024U)

// =============================================================================
// Public API
// =============================================================================

/**
 * @brief  Mounts the LittleFS partition and initializes internal state.
 *
 * @note   Automatically counts and reports pre-existing records from a
 *         previous session (power-failure recovery).
 * @note   Formats the partition if mounting fails (first boot or corruption).
 *
 * @return ESP_OK           on success.
 * @return ESP_ERR_NO_MEM   if FreeRTOS mutex allocation fails.
 * @return ESP_FAIL         if LittleFS mounting fails after format attempt.
 */
esp_err_t storage_init(void);

/**
 * @brief  Appends a single JSON telemetry record to the log file.
 *
 * @details This is the hot path of the Write-First policy. It is designed
 *          to be called from Core 0 at every FSM tick during EMERGENCY mode,
 *          and periodically during OPERATION mode.
 *
 * @param[in] json_record  Null-terminated JSON string. Must not contain '\n'.
 *
 * @return ESP_OK                 on success.
 * @return ESP_ERR_INVALID_STATE  if storage is not mounted.
 * @return ESP_ERR_INVALID_ARG    if json_record is NULL.
 * @return ESP_ERR_NO_MEM         if storage_is_near_full() is true.
 * @return ESP_ERR_TIMEOUT        if mutex could not be acquired in 50 ms.
 * @return ESP_FAIL               if filesystem write fails.
 */
esp_err_t storage_append(const char *json_record);

/**
 * @brief  Reads the next un-synced record from the log file into a buffer.
 *
 * @details Called by Core 1 during RESYNC mode. The internal sync cursor
 *          advances only after storage_pop_synced() is called, allowing
 *          retry on MQTT publish failure.
 *
 * @param[out] buffer    Destination buffer for the JSON string.
 * @param[in]  buf_size  Size of the destination buffer (recommend >= STORAGE_MAX_LINE_LEN).
 *
 * @return ESP_OK              on success — buffer contains a valid JSON record.
 * @return ESP_ERR_NOT_FOUND   if no pending records remain.
 * @return ESP_ERR_INVALID_ARG if buffer is NULL or buf_size is 0.
 * @return ESP_ERR_TIMEOUT     if mutex could not be acquired in 100 ms.
 */
esp_err_t storage_read_next(char *buffer, size_t buf_size);

/**
 * @brief  Marks the current head record as successfully synced.
 *
 * @details Advances the internal sync cursor past the current record.
 *          When the last record is popped, the log file is truncated
 *          and the cursor is reset to 0.
 *
 * @note   Only call this AFTER receiving a confirmed MQTT QoS 1 ACK.
 *
 * @return ESP_OK              on success.
 * @return ESP_ERR_INVALID_STATE if storage is not mounted.
 * @return ESP_ERR_NOT_FOUND   if no pending records exist.
 * @return ESP_ERR_TIMEOUT     if mutex could not be acquired in 100 ms.
 */
esp_err_t storage_pop_synced(void);

/**
 * @brief  Returns the count of records not yet confirmed as synced.
 *
 * @note   This count persists across reboots (loaded from file at init).
 *         The sync cursor does NOT persist — on reboot, all file records
 *         are treated as pending (safe: MQTT QoS 1 handles deduplication).
 *
 * @return Number of pending records. 0 if not mounted.
 */
uint32_t storage_get_pending_count(void);

/**
 * @brief  Checks whether the log file is approaching the size safety limit.
 *
 * @return true  if log file size > STORAGE_MAX_FILE_SIZE.
 * @return false if file does not exist yet, or storage is not mounted.
 */
bool storage_is_near_full(void);

/**
 * @brief  Unmounts the LittleFS partition and releases resources.
 *
 * @note   Call before entering deep sleep or before a controlled reboot.
 */
void storage_deinit(void);

#endif // STORAGE_DRIVER_H