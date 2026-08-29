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
    .max_open_files = 4,                           \
    .mount_point = "/sdcard",                     \
}

esp_err_t sd_card_mount(const sd_card_config_t *config);
esp_err_t sd_card_unmount(void);
bool sd_card_is_mounted(void);
esp_err_t sd_card_print_info(FILE *stream);

/**
 * Open a path relative to the mount point using an fopen-style mode.
 * Paths must use forward slashes and may not contain empty, ".", or ".."
 * components. The caller must not use a handle concurrently with close().
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
esp_err_t sd_card_file_close(sd_card_file_t *file);

#ifdef __cplusplus
}
#endif
