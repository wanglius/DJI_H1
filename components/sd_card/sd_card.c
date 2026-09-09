#include "sd_card.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SD_CARD";

#define SD_CARD_PATH_CAPACITY 128
#define SD_CARD_MOUNT_POINT_CAPACITY 32

struct sd_card_file {
    FILE *stream;
};

static bool s_mounted;
static spi_host_device_t s_spi_host;
static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;
static portMUX_TYPE s_lock_init_guard = portMUX_INITIALIZER_UNLOCKED;
static size_t s_open_file_count;
static char s_mount_point[SD_CARD_MOUNT_POINT_CAPACITY];

static esp_err_t ensure_lock(void)
{
    taskENTER_CRITICAL(&s_lock_init_guard);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    taskEXIT_CRITICAL(&s_lock_init_guard);
    return s_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t lock_card(void)
{
    esp_err_t result = ensure_lock();
    if (result != ESP_OK) return result;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void unlock_card(void)
{
    xSemaphoreGive(s_lock);
}

static esp_err_t make_full_path(const char *path,
                                char full_path[SD_CARD_PATH_CAPACITY])
{
    if (path == NULL || path[0] == '\0') return ESP_ERR_INVALID_ARG;
    while (*path == '/') path++;
    if (*path == '\0') return ESP_ERR_INVALID_ARG;

    /* Keep every operation below the mount point on all VFS backends. */
    const char *component = path;
    for (const char *cursor = path;; cursor++) {
        if (*cursor == '\\' || *cursor == ':' ||
            ((unsigned char)*cursor < 0x20 && *cursor != '\0')) {
            return ESP_ERR_INVALID_ARG;
        }
        if (*cursor == '/' || *cursor == '\0') {
            size_t length = (size_t)(cursor - component);
            if (length == 0 ||
                (length == 1 && component[0] == '.') ||
                (length == 2 && component[0] == '.' && component[1] == '.')) {
                return ESP_ERR_INVALID_ARG;
            }
            if (*cursor == '\0') break;
            component = cursor + 1;
        }
    }

    int length = snprintf(full_path, SD_CARD_PATH_CAPACITY,
                          "%s/%s", s_mount_point, path);
    return length > 0 && length < SD_CARD_PATH_CAPACITY
        ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t sd_card_mount(const sd_card_config_t *config)
{
    if (config == NULL || config->mount_point == NULL ||
        config->mount_point[0] != '/' || config->max_open_files == 0 ||
        config->max_frequency_khz == 0 || config->max_transfer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    if (s_mounted) {
        unlock_card();
        return ESP_ERR_INVALID_STATE;
    }

    size_t mount_length = strlen(config->mount_point);
    if (mount_length >= sizeof(s_mount_point)) {
        unlock_card();
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_mount_point, config->mount_point, mount_length + 1);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = config->spi_host;
    host.max_freq_khz = config->max_frequency_khz;

    const spi_bus_config_t bus_config = {
        .mosi_io_num = config->pin_mosi,
        .miso_io_num = config->pin_miso,
        .sclk_io_num = config->pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = config->max_transfer_size,
    };
    result = spi_bus_initialize(
        host.slot, &bus_config, SDSPI_DEFAULT_DMA);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus initialization failed: %s",
                 esp_err_to_name(result));
        goto fail;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = host.slot;
    slot_config.gpio_cs = config->pin_cs;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = config->max_open_files,
        .allocation_unit_size = 16 * 1024,
    };
    result = esp_vfs_fat_sdspi_mount(
        s_mount_point, &host, &slot_config, &mount_config, &s_card);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Card initialization/mount failed: %s",
                 esp_err_to_name(result));
        spi_bus_free(host.slot);
        goto fail;
    }

    s_spi_host = host.slot;
    s_open_file_count = 0;
    __atomic_store_n(&s_mounted, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "Mounted %s at %s", s_card->cid.name, s_mount_point);
    unlock_card();
    return ESP_OK;

fail:
    s_card = NULL;
    s_mount_point[0] = '\0';
    unlock_card();
    return result;
}

esp_err_t sd_card_unmount(void)
{
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    if (!s_mounted) {
        unlock_card();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_open_file_count != 0) {
        ESP_LOGE(TAG, "Cannot unmount with %u open file(s)",
                 (unsigned)s_open_file_count);
        unlock_card();
        return ESP_ERR_INVALID_STATE;
    }

    result = esp_vfs_fat_sdcard_unmount(s_mount_point, s_card);
    if (result != ESP_OK) {
        unlock_card();
        return result;
    }

    esp_err_t bus_result = spi_bus_free(s_spi_host);

    __atomic_store_n(&s_mounted, false, __ATOMIC_RELEASE);
    s_card = NULL;
    s_mount_point[0] = '\0';
    unlock_card();
    return bus_result;
}

bool sd_card_is_mounted(void)
{
    return __atomic_load_n(&s_mounted, __ATOMIC_ACQUIRE);
}

esp_err_t sd_card_get_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (total_bytes == NULL || free_bytes == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    result = s_mounted ? esp_vfs_fat_info(s_mount_point, total_bytes, free_bytes)
                       : ESP_ERR_INVALID_STATE;
    unlock_card();
    return result;
}

esp_err_t sd_card_print_info(FILE *stream)
{
    if (stream == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    if (!s_mounted) {
        unlock_card();
        return ESP_ERR_INVALID_STATE;
    }
    sdmmc_card_print_info(stream, s_card);
    unlock_card();
    return ESP_OK;
}

esp_err_t sd_card_mkdir(const char *path)
{
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    if (!s_mounted) {
        unlock_card();
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[SD_CARD_PATH_CAPACITY];
    result = make_full_path(path, full_path);
    if (result == ESP_OK && mkdir(full_path, 0775) != 0) {
        if (errno == EEXIST) {
            struct stat info;
            result = stat(full_path, &info) == 0 && S_ISDIR(info.st_mode)
                ? ESP_OK : ESP_FAIL;
        } else {
            ESP_LOGE(TAG, "Could not create %s: errno=%d (%s)",
                     full_path, errno, strerror(errno));
            result = ESP_FAIL;
        }
    }
    unlock_card();
    return result;
}

esp_err_t sd_card_file_open(const char *path,
                            const char *mode,
                            sd_card_file_t **out_file)
{
    if (mode == NULL || out_file == NULL) return ESP_ERR_INVALID_ARG;
    *out_file = NULL;
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    if (!s_mounted) {
        unlock_card();
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[SD_CARD_PATH_CAPACITY];
    result = make_full_path(path, full_path);
    if (result != ESP_OK) {
        unlock_card();
        return result;
    }

    sd_card_file_t *file = calloc(1, sizeof(*file));
    if (file == NULL) {
        unlock_card();
        return ESP_ERR_NO_MEM;
    }
    file->stream = fopen(full_path, mode);
    if (file->stream == NULL) {
        ESP_LOGE(TAG, "Could not open %s: errno=%d (%s)",
                 full_path, errno, strerror(errno));
        free(file);
        unlock_card();
        return ESP_FAIL;
    }

    s_open_file_count++;
    *out_file = file;
    unlock_card();
    return ESP_OK;
}

esp_err_t sd_card_file_write(sd_card_file_t *file,
                             const void *data,
                             size_t length,
                             size_t *bytes_written)
{
    if (file == NULL || file->stream == NULL ||
        (data == NULL && length != 0)) return ESP_ERR_INVALID_ARG;
    if (bytes_written != NULL) *bytes_written = 0;
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    size_t written = fwrite(data, 1, length, file->stream);
    if (bytes_written != NULL) *bytes_written = written;
    result = written == length && ferror(file->stream) == 0
        ? ESP_OK : ESP_FAIL;
    unlock_card();
    return result;
}

esp_err_t sd_card_file_read(sd_card_file_t *file,
                            void *data,
                            size_t capacity,
                            size_t *bytes_read)
{
    if (file == NULL || file->stream == NULL || bytes_read == NULL ||
        (data == NULL && capacity != 0)) return ESP_ERR_INVALID_ARG;
    *bytes_read = 0;
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    *bytes_read = fread(data, 1, capacity, file->stream);
    result = ferror(file->stream) == 0 ? ESP_OK : ESP_FAIL;
    unlock_card();
    return result;
}

esp_err_t sd_card_file_flush(sd_card_file_t *file)
{
    if (file == NULL || file->stream == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    result = fflush(file->stream) == 0 ? ESP_OK : ESP_FAIL;
    unlock_card();
    return result;
}

esp_err_t sd_card_path_exists(const char *path, bool *exists)
{
    if (exists == NULL) return ESP_ERR_INVALID_ARG;
    *exists = false;
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    if (!s_mounted) {
        unlock_card();
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[SD_CARD_PATH_CAPACITY];
    result = make_full_path(path, full_path);
    if (result == ESP_OK) {
        struct stat info;
        if (stat(full_path, &info) == 0) {
            *exists = true;
        } else if (errno != ENOENT) {
            ESP_LOGE(TAG, "Could not inspect %s: errno=%d (%s)",
                     full_path, errno, strerror(errno));
            result = ESP_FAIL;
        }
    }
    unlock_card();
    return result;
}

esp_err_t sd_card_replace_file(const char *temporary_path,
                               const char *target_path,
                               const char *backup_path)
{
    if (temporary_path == NULL || target_path == NULL || backup_path == NULL)
        return ESP_ERR_INVALID_ARG;
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    if (!s_mounted) {
        unlock_card();
        return ESP_ERR_INVALID_STATE;
    }

    char temporary[SD_CARD_PATH_CAPACITY];
    char target[SD_CARD_PATH_CAPACITY];
    char backup[SD_CARD_PATH_CAPACITY];
    result = make_full_path(temporary_path, temporary);
    if (result == ESP_OK) result = make_full_path(target_path, target);
    if (result == ESP_OK) result = make_full_path(backup_path, backup);
    if (result != ESP_OK) {
        unlock_card();
        return result;
    }

    /* FatFs does not replace an existing destination. Keep the previous JSON
     * as a recovery copy until the new checkpoint has acquired its final name. */
    if (remove(backup) != 0 && errno != ENOENT) result = ESP_FAIL;
    bool had_target = false;
    struct stat info;
    if (result == ESP_OK && stat(target, &info) == 0) {
        had_target = true;
        if (rename(target, backup) != 0) result = ESP_FAIL;
    } else if (result == ESP_OK && errno != ENOENT) {
        result = ESP_FAIL;
    }
    if (result == ESP_OK) {
        if (rename(temporary, target) != 0) {
            result = ESP_FAIL;
            if (had_target) (void)rename(backup, target);
        } else if (had_target && remove(backup) != 0 && errno != ENOENT) {
            /* The new checkpoint is authoritative; a stale backup is harmless
             * and preferable to reporting the successful replacement failed. */
            ESP_LOGW(TAG, "Could not remove stale checkpoint backup %s", backup);
        }
    }
    unlock_card();
    return result;
}

esp_err_t sd_card_file_size(sd_card_file_t *file, uint64_t *size_bytes)
{
    if (file == NULL || file->stream == NULL || size_bytes == NULL)
        return ESP_ERR_INVALID_ARG;
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    long original = ftell(file->stream);
    if (original < 0 || fseek(file->stream, 0, SEEK_END) != 0) {
        result = ESP_FAIL;
    } else {
        long end = ftell(file->stream);
        if (end < 0 || fseek(file->stream, original, SEEK_SET) != 0)
            result = ESP_FAIL;
        else
            *size_bytes = (uint64_t)end;
    }
    unlock_card();
    return result;
}

esp_err_t sd_card_file_close(sd_card_file_t *file)
{
    if (file == NULL || file->stream == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t result = lock_card();
    if (result != ESP_OK) return result;
    int close_result = fclose(file->stream);
    file->stream = NULL;
    if (s_open_file_count > 0) s_open_file_count--;
    free(file);
    unlock_card();
    return close_result == 0 ? ESP_OK : ESP_FAIL;
}
