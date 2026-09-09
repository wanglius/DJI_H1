#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sd_card_file sd_card_file_t;

typedef struct {
    spi_host_device_t spi_host;
    int pin_cs;
    int pin_mosi;
    int pin_sclk;
    int pin_miso;
    uint32_t max_frequency_khz;
    size_t max_transfer_size;
    size_t max_open_files;
    const char *mount_point;
} sd_card_config_t;

/** Proven LilyGO T8-S3 TF-slot wiring with a conservative 10 MHz clock. */
#define SD_CARD_LILYGO_T8_S3_DEFAULT_CONFIG() { \
    .spi_host = SPI2_HOST,                         \
    .pin_cs = 10,                                  \
    .pin_mosi = 11,                                \
    .pin_sclk = 12,                                \
    .pin_miso = 13,                                \
    .max_frequency_khz = 10000,                    \
    .max_transfer_size = 4096,                     \
    .max_open_files = 8,                           \
    .mount_point = "/sdcard",                     \
}

esp_err_t sd_card_mount(const sd_card_config_t *config);
esp_err_t sd_card_unmount(void);
bool sd_card_is_mounted(void);
/** Read FAT capacity under the component lock; may block on card I/O. */
esp_err_t sd_card_get_space(uint64_t *total_bytes, uint64_t *free_bytes);
esp_err_t sd_card_print_info(FILE *stream);

/**
 * Create one directory relative to the mount point.
 * Returns ESP_OK if the directory already exists. Parent directories must
 * already exist; call this function once for each level of a nested path.
 */
esp_err_t sd_card_mkdir(const char *path);
/** Test whether a validated relative path already exists on the mounted card. */
esp_err_t sd_card_path_exists(const char *path, bool *exists);
/** Set a file or directory's FAT modification time from Unix UTC milliseconds.
 * FAT stores calendar fields with two-second resolution and no timezone. */
esp_err_t sd_card_set_modified_time(const char *path, uint64_t utc_ms);
/**
 * Replace target with a fully written temporary file. If target exists it is
 * moved to backup first, so an interrupted checkpoint leaves at least one
 * recoverable copy. All paths are relative to the mount point.
 */
esp_err_t sd_card_replace_file(const char *temporary_path,
                               const char *target_path,
                               const char *backup_path);

/**
 * Open a path relative to the mount point using an fopen-style mode.
 * Paths must use forward slashes and may not contain empty, ".", or ".."
 * components. The caller must not use a handle concurrently with close().
 *
 * CONFIG_FATFS_FS_LOCK is deliberately zero. All filesystem access while the
 * card is mounted must therefore use this component so s_lock serializes it;
 * direct stdio/POSIX access from another task violates that invariant.
 */
esp_err_t sd_card_file_open(const char *path,
                            const char *mode,
                            sd_card_file_t **out_file);
esp_err_t sd_card_file_write(sd_card_file_t *file,
                             const void *data,
                             size_t length,
                             size_t *bytes_written);
esp_err_t sd_card_file_read(sd_card_file_t *file,
                            void *data,
                            size_t capacity,
                            size_t *bytes_read);
esp_err_t sd_card_file_flush(sd_card_file_t *file);
/** Return the current file length without changing the caller's position. */
esp_err_t sd_card_file_size(sd_card_file_t *file, uint64_t *size_bytes);
esp_err_t sd_card_file_close(sd_card_file_t *file);

#ifdef __cplusplus
}
#endif
