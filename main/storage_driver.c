/**
 * @file    storage_driver.c
 * @brief   LittleFS Storage Driver — Write-First Policy Implementation
 * @version DEV1
 *
 * @details Architecture notes:
 *   - storage_append() runs on Core 0 (hot path, must be fast < 5 ms).
 *   - storage_read_next() / storage_pop_synced() run on Core 1 (RESYNC only).
 *   - A single FreeRTOS mutex serializes all filesystem access.
 *
 *   Sync cursor design:
 *   - sync_cursor is a byte offset stored in RAM only (not persisted to NVS).
 *   - On reboot, sync_cursor resets to 0 → all file records become "pending".
 *   - This guarantees at-least-once delivery. MQTT QoS 1 + timestamp in the
 *     JSON payload allows the broker/backend to deduplicate retransmissions.
 *
 * @author  Eng. Alvaro
 */

#include "storage_driver.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "STORAGE";

// =============================================================================
// Internal State
// =============================================================================

typedef struct {
    bool              mounted;
    uint32_t          pending_count; ///< Records not yet confirmed as synced
    long              sync_cursor;   ///< Byte offset of next record to read/pop
    SemaphoreHandle_t mutex;
} storage_ctx_t;

static storage_ctx_t s_ctx = {
    .mounted       = false,
    .pending_count = 0U,
    .sync_cursor   = 0L,
    .mutex         = NULL,
};

// =============================================================================
// Private Helpers
// =============================================================================

/**
 * @brief Counts newline-terminated records in the log file.
 *        Called once at init for power-failure recovery.
 */
static uint32_t prv_count_records(void)
{
    FILE *f = fopen(STORAGE_LOG_FILE, "r");
    if (!f) return 0U;

    uint32_t count = 0U;
    int ch;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') count++;
    }
    fclose(f);
    return count;
}

// =============================================================================
// Public API Implementation
// =============================================================================

esp_err_t storage_init(void)
{
    if (s_ctx.mounted) {
        ESP_LOGW(TAG, "Already mounted — skipping init");
        return ESP_OK;
    }

    // ── Step 1: Create synchronisation primitive ──────────────────────────────
    s_ctx.mutex = xSemaphoreCreateMutex();
    if (!s_ctx.mutex) {
        ESP_LOGE(TAG, "Mutex allocation failed");
        return ESP_ERR_NO_MEM;
    }

    // ── Step 2: Mount LittleFS ─────────────────────────────────────────────────
    const esp_vfs_littlefs_conf_t conf = {
        .base_path             = STORAGE_MOUNT_POINT,
        .partition_label       = STORAGE_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount            = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed [%s] — check partitions.csv", esp_err_to_name(ret));
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
        return ret;
    }

    // ── Step 3: Report filesystem capacity ────────────────────────────────────
    size_t total_b = 0U, used_b = 0U;
    if (esp_littlefs_info(STORAGE_PARTITION_LABEL, &total_b, &used_b) == ESP_OK) {
        ESP_LOGI(TAG, "Partition: %u KB total | %u KB used | %u KB free",
                 (unsigned)(total_b  / 1024U),
                 (unsigned)(used_b   / 1024U),
                 (unsigned)((total_b - used_b) / 1024U));
    }

    // ── Step 4: Recovery — count records from previous session ─────────────────
    s_ctx.mounted       = true;
    s_ctx.sync_cursor   = 0L;
    s_ctx.pending_count = prv_count_records();

    if (s_ctx.pending_count > 0U) {
        ESP_LOGW(TAG, "[RECOVERY] %lu record(s) pending from previous session",
                 (unsigned long)s_ctx.pending_count);
    } else {
        ESP_LOGI(TAG, "Storage ready — no pending records");
    }

    return ESP_OK;
}

// -----------------------------------------------------------------------------

esp_err_t storage_append(const char *json_record)
{
    if (!s_ctx.mounted) {
        ESP_LOGE(TAG, "append: not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    if (!json_record) {
        return ESP_ERR_INVALID_ARG;
    }
    if (storage_is_near_full()) {
        ESP_LOGE(TAG, "STORAGE FULL — record dropped! Trigger RESYNC.");
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "append: mutex timeout (50 ms)");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_FAIL;

    FILE *f = fopen(STORAGE_LOG_FILE, "a");
    if (f) {
        /* Write: JSON record + newline delimiter */
        int written = fprintf(f, "%s\n", json_record);
        fclose(f);

        if (written > 0) {
            s_ctx.pending_count++;
            ret = ESP_OK;
            ESP_LOGD(TAG, "[+] Stored record #%lu: %s",
                     (unsigned long)s_ctx.pending_count, json_record);
        } else {
            ESP_LOGE(TAG, "fprintf failed (errno=%d)", errno);
        }
    } else {
        ESP_LOGE(TAG, "fopen(APPEND) failed (errno=%d)", errno);
    }

    xSemaphoreGive(s_ctx.mutex);
    return ret;
}

// -----------------------------------------------------------------------------

esp_err_t storage_read_next(char *buffer, size_t buf_size)
{
    if (!s_ctx.mounted || !buffer || buf_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.pending_count == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;

    FILE *f = fopen(STORAGE_LOG_FILE, "r");
    if (f) {
        fseek(f, s_ctx.sync_cursor, SEEK_SET);
        if (fgets(buffer, (int)buf_size, f) != NULL) {
            /* Strip trailing newline for clean JSON string */
            size_t len = strlen(buffer);
            if (len > 0U && buffer[len - 1U] == '\n') {
                buffer[len - 1U] = '\0';
            }
            ret = ESP_OK;
        }
        fclose(f);
    }

    xSemaphoreGive(s_ctx.mutex);
    return ret;
}

// -----------------------------------------------------------------------------

esp_err_t storage_pop_synced(void)
{
    if (!s_ctx.mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.pending_count == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    if (xSemaphoreTake(s_ctx.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    FILE *f = fopen(STORAGE_LOG_FILE, "r");
    if (f) {
        fseek(f, s_ctx.sync_cursor, SEEK_SET);

        /* Advance cursor past the current record */
        char discard[STORAGE_MAX_LINE_LEN];
        if (fgets(discard, sizeof(discard), f) != NULL) {
            s_ctx.sync_cursor = ftell(f);
            s_ctx.pending_count--;
            ESP_LOGD(TAG, "[-] Popped record. Cursor=%ld | Pending=%lu",
                     s_ctx.sync_cursor, (unsigned long)s_ctx.pending_count);
        }
        fclose(f);

        /* All records synced: truncate log file and reset state */
        if (s_ctx.pending_count == 0U) {
            f = fopen(STORAGE_LOG_FILE, "w"); /* "w" mode truncates to zero */
            if (f) fclose(f);
            s_ctx.sync_cursor = 0L;
            ESP_LOGI(TAG, "All records synced — log file cleared.");
        }
    }

    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

// -----------------------------------------------------------------------------

uint32_t storage_get_pending_count(void)
{
    return s_ctx.mounted ? s_ctx.pending_count : 0U;
}

// -----------------------------------------------------------------------------

bool storage_is_near_full(void)
{
    if (!s_ctx.mounted) return false;

    struct stat st;
    if (stat(STORAGE_LOG_FILE, &st) == 0) {
        return ((size_t)st.st_size > STORAGE_MAX_FILE_SIZE);
    }
    return false; /* File doesn't exist yet — not full */
}

// -----------------------------------------------------------------------------

void storage_deinit(void)
{
    if (!s_ctx.mounted) return;

    esp_vfs_littlefs_unregister(STORAGE_PARTITION_LABEL);

    if (s_ctx.mutex) {
        vSemaphoreDelete(s_ctx.mutex);
        s_ctx.mutex = NULL;
    }

    s_ctx.mounted       = false;
    s_ctx.pending_count = 0U;
    s_ctx.sync_cursor   = 0L;

    ESP_LOGI(TAG, "LittleFS unmounted.");
}