#include "clock_sync.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "CLOCK_SYNC";

#define SYNC_QUEUE_LENGTH 16
#define SYNC_TASK_STACK 4096
#define SYNC_TASK_PRIORITY 6
#define SYNC_WINDOW 32
#define SYNC_LOCK_SAMPLES 5
#define SYNC_HOLDOVER_US 1500000LL
#define SYNC_INVALID_US 5000000LL
#define SYNC_RESIDUAL_LIMIT_US 50000.0

#ifndef CONFIG_DJI_H1_TIMEZONE_NAME
#define CONFIG_DJI_H1_TIMEZONE_NAME "Asia/Shanghai"
#endif
#ifndef CONFIG_DJI_H1_TIMEZONE_OFFSET_MINUTES
#define CONFIG_DJI_H1_TIMEZONE_OFFSET_MINUTES 480
#endif

_Static_assert(CONFIG_DJI_H1_TIMEZONE_OFFSET_MINUTES >= -720 &&
               CONFIG_DJI_H1_TIMEZONE_OFFSET_MINUTES <= 840,
               "timezone offset must be between UTC-12:00 and UTC+14:00");

typedef struct {
    uint32_t a_raw_ms;
    uint64_t utc_ms;
    int64_t b_receive_us;
    bool utc_valid;
} sync_observation_t;

typedef struct {
    uint64_t a_ms;
    int64_t b_us;
    uint64_t utc_ms;
    bool utc_valid;
} sync_sample_t;

typedef struct {
    clock_sync_state_t state;
    uint16_t generation;
    uint64_t anchor_a_ms;
    int64_t anchor_b_us;
    int64_t utc_offset_ms;
    double b_us_per_a_ms;
    int64_t last_observation_us;
    bool model_valid;
    bool utc_valid;
} sync_snapshot_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static sync_snapshot_t s_snapshot;
static sync_sample_t s_samples[SYNC_WINDOW];
static size_t s_sample_count, s_sample_next;
static bool s_have_raw;
static uint32_t s_last_raw;
static uint64_t s_extended_ms;
static uint32_t s_queue_drops;
static uint16_t s_wall_clock_generation;
static int64_t s_wall_clock_retry_after_us;

const char *clock_sync_state_name(clock_sync_state_t state)
{
    switch (state) {
    case CLOCK_SYNC_UNSYNCED: return "UNSYNCED";
    case CLOCK_SYNC_ACQUIRING: return "ACQUIRING";
    case CLOCK_SYNC_LOCKED: return "LOCKED";
    case CLOCK_SYNC_HOLDOVER: return "HOLDOVER";
    case CLOCK_SYNC_INVALID: return "INVALID";
    default: return "UNKNOWN";
    }
}

const char *clock_sync_timezone_name(void)
{
    return CONFIG_DJI_H1_TIMEZONE_NAME;
}

int16_t clock_sync_timezone_offset_minutes(void)
{
    return (int16_t)CONFIG_DJI_H1_TIMEZONE_OFFSET_MINUTES;
}

static bool valid_timezone_name(const char *name)
{
    if (name == NULL || *name == '\0' || strlen(name) > 63U) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        bool safe = (*p >= 'A' && *p <= 'Z') ||
                    (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') ||
                    *p == '/' || *p == '_' || *p == '-' || *p == '+' ||
                    *p == '.';
        if (!safe) return false;
    }
    return true;
}

static void publish(const sync_snapshot_t *snapshot)
{
    taskENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot = *snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

static bool extend_a_time(uint32_t raw, uint64_t *extended)
{
    if (!s_have_raw) {
        s_have_raw = true;
        s_last_raw = raw;
        s_extended_ms = raw;
    } else {
        uint32_t delta = raw - s_last_raw;
        if (delta > UINT32_MAX / 2U) return false; /* Backward jump/A reset. */
        s_extended_ms += delta; /* Correct across the uint32 forward wrap. */
        s_last_raw = raw;
    }
    *extended = s_extended_ms;
    return true;
}

static int compare_i64(const void *left, const void *right)
{
    int64_t a = *(const int64_t *)left, b = *(const int64_t *)right;
    return (a > b) - (a < b);
}

static bool fit_model(sync_snapshot_t *model)
{
    if (s_sample_count < 2) return false;
    const sync_sample_t *base = &s_samples[(s_sample_next + SYNC_WINDOW -
                                             s_sample_count) % SYNC_WINDOW];
    double sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
    int64_t offsets[SYNC_WINDOW];
    size_t utc_count = 0;
    for (size_t i = 0; i < s_sample_count; i++) {
        const sync_sample_t *sample =
            &s_samples[(s_sample_next + SYNC_WINDOW - s_sample_count + i) % SYNC_WINDOW];
        double x = (double)(sample->a_ms - base->a_ms);
        double y = (double)(sample->b_us - base->b_us);
        sum_x += x; sum_y += y; sum_xx += x * x; sum_xy += x * y;
        if (sample->utc_valid) offsets[utc_count++] =
            (int64_t)sample->utc_ms - (int64_t)sample->a_ms;
    }
    double denominator = s_sample_count * sum_xx - sum_x * sum_x;
    if (denominator <= 0) return false;
    double slope = (s_sample_count * sum_xy - sum_x * sum_y) / denominator;
    if (slope < 950.0 || slope > 1050.0) return false;
    const sync_sample_t *latest =
        &s_samples[(s_sample_next + SYNC_WINDOW - 1) % SYNC_WINDOW];
    model->anchor_a_ms = latest->a_ms;
    model->anchor_b_us = latest->b_us;
    model->b_us_per_a_ms = slope;
    model->model_valid = true;
    model->utc_valid = utc_count != 0;
    if (utc_count) {
        qsort(offsets, utc_count, sizeof(offsets[0]), compare_i64);
        model->utc_offset_ms = offsets[utc_count / 2];
    }
    return true;
}

static bool project_model_time(const sync_snapshot_t *model,
                               int64_t b_monotonic_us,
                               uint64_t *a_monotonic_ms,
                               uint64_t *utc_ms)
{
    if (!model->model_valid) return false;
    double delta_ms = (b_monotonic_us - model->anchor_b_us) /
                      model->b_us_per_a_ms;
    int64_t a_ms = (int64_t)model->anchor_a_ms + (int64_t)llround(delta_ms);
    if (a_ms < 0) return false;
    if (a_monotonic_ms != NULL) *a_monotonic_ms = (uint64_t)a_ms;
    if (utc_ms != NULL) {
        if (!model->utc_valid || a_ms + model->utc_offset_ms < 0) return false;
        *utc_ms = (uint64_t)(a_ms + model->utc_offset_ms);
    }
    return true;
}

static void update_wall_clock(const sync_snapshot_t *model)
{
    /* FatFs get_fattime() reads the POSIX wall clock. Set it only after a
     * validated A-board clock model locks, and at most once per generation;
     * repeated 5 Hz settimeofday() calls would turn UART jitter into wall-clock
     * steps. Monotonic acquisition timing remains based on esp_timer. */
    if (model->state != CLOCK_SYNC_LOCKED || !model->utc_valid ||
        s_wall_clock_generation == model->generation) return;
    int64_t now_us = esp_timer_get_time();
    if (now_us < s_wall_clock_retry_after_us) return;
    uint64_t utc_ms;
    if (!project_model_time(model, now_us, NULL, &utc_ms)) return;
    struct timeval wall_time = {
        .tv_sec = (time_t)(utc_ms / 1000ULL),
        .tv_usec = (suseconds_t)((utc_ms % 1000ULL) * 1000ULL),
    };
    if ((uint64_t)wall_time.tv_sec != utc_ms / 1000ULL ||
        settimeofday(&wall_time, NULL) != 0) {
        s_wall_clock_retry_after_us = now_us + 5000000LL;
        ESP_LOGE(TAG, "Could not synchronize POSIX/FatFs wall clock");
        return;
    }
    s_wall_clock_generation = model->generation;
    s_wall_clock_retry_after_us = 0;
    ESP_LOGI(TAG, "POSIX/FatFs wall clock synchronized, generation=%u",
             model->generation);
}

static void reset_estimator(sync_snapshot_t *model, const char *reason)
{
    s_sample_count = s_sample_next = 0;
    s_have_raw = false;
    model->generation++;
    model->state = CLOCK_SYNC_ACQUIRING;
    model->model_valid = model->utc_valid = false;
    ESP_LOGW(TAG, "Synchronization generation %u: %s",
             model->generation, reason);
}

static void set_state(sync_snapshot_t *model, clock_sync_state_t state)
{
    if (model->state == state) return;
    ESP_LOGI(TAG, "State %s -> %s, generation=%u",
             clock_sync_state_name(model->state), clock_sync_state_name(state),
             model->generation);
    model->state = state;
}

static bool utc_discontinuous(const sync_snapshot_t *model,
                              uint64_t a_ms, uint64_t utc_ms)
{
    if (!model->utc_valid) return false;
    int64_t candidate = (int64_t)utc_ms - (int64_t)a_ms;
    return llabs(candidate - model->utc_offset_ms) > 1000;
}

static void process_observation(sync_snapshot_t *model,
                                const sync_observation_t *observation)
{
    uint64_t extended;
    if (!extend_a_time(observation->a_raw_ms, &extended)) {
        reset_estimator(model, "A monotonic timer moved backward");
        (void)extend_a_time(observation->a_raw_ms, &extended);
    }
    if (observation->utc_valid && utc_discontinuous(model, extended,
                                                     observation->utc_ms)) {
        reset_estimator(model, "UTC offset jumped by over one second");
        (void)extend_a_time(observation->a_raw_ms, &extended);
    }
    if (model->model_valid) {
        double predicted = model->anchor_b_us +
            ((double)((int64_t)extended - (int64_t)model->anchor_a_ms) *
             model->b_us_per_a_ms);
        if (fabs(predicted - observation->b_receive_us) > SYNC_RESIDUAL_LIMIT_US) {
            reset_estimator(model, "clock residual exceeded 50 ms");
            (void)extend_a_time(observation->a_raw_ms, &extended);
        }
    }
    sync_sample_t *sample = &s_samples[s_sample_next];
    *sample = (sync_sample_t) {
        .a_ms = extended, .b_us = observation->b_receive_us,
        .utc_ms = observation->utc_ms, .utc_valid = observation->utc_valid,
    };
    s_sample_next = (s_sample_next + 1) % SYNC_WINDOW;
    if (s_sample_count < SYNC_WINDOW) s_sample_count++;
    model->last_observation_us = observation->b_receive_us;
    if (s_sample_count == 1) set_state(model, CLOCK_SYNC_ACQUIRING);
    if (fit_model(model) && s_sample_count >= SYNC_LOCK_SAMPLES) {
        set_state(model, CLOCK_SYNC_LOCKED);
    }
}

static void update_timeout(sync_snapshot_t *model, int64_t now_us)
{
    if (!model->last_observation_us) return;
    int64_t age = now_us - model->last_observation_us;
    if (age >= SYNC_INVALID_US) set_state(model, CLOCK_SYNC_INVALID);
    else if (age >= SYNC_HOLDOVER_US && model->state == CLOCK_SYNC_LOCKED) {
        set_state(model, CLOCK_SYNC_HOLDOVER);
    }
}

static void clock_sync_task(void *unused)
{
    (void)unused;
    sync_snapshot_t model = {.state = CLOCK_SYNC_UNSYNCED,
                             .generation = 1,
                             .b_us_per_a_ms = 1000.0};
    publish(&model);
    sync_observation_t observation;
    while (true) {
        if (xQueueReceive(s_queue, &observation, pdMS_TO_TICKS(250)) == pdTRUE) {
            process_observation(&model, &observation);
        }
        update_timeout(&model, esp_timer_get_time());
        update_wall_clock(&model);
        publish(&model);
    }
}

esp_err_t clock_sync_init(void)
{
    if (s_task != NULL) return ESP_ERR_INVALID_STATE;
    if (!valid_timezone_name(CONFIG_DJI_H1_TIMEZONE_NAME)) {
        ESP_LOGE(TAG, "Invalid mission timezone name");
        return ESP_ERR_INVALID_ARG;
    }
    /* Unix timestamps remain UTC. POSIX TZ only controls conversion to the
     * timezone-less calendar fields stored by FAT, so Windows in the selected
     * deployment zone displays the intended local modification time. POSIX
     * signs are reversed: local UTC+08:00 is encoded as UTC-8:00. */
    int offset = CONFIG_DJI_H1_TIMEZONE_OFFSET_MINUTES;
    int posix_offset = -offset;
    unsigned magnitude = (unsigned)abs(posix_offset);
    char posix_tz[24];
    int length = snprintf(posix_tz, sizeof(posix_tz), "UTC%c%u:%02u",
                          posix_offset >= 0 ? '+' : '-', magnitude / 60U,
                          magnitude % 60U);
    if (length <= 0 || (size_t)length >= sizeof(posix_tz))
        return ESP_ERR_INVALID_SIZE;
    if (setenv("TZ", posix_tz, 1) != 0) return ESP_ERR_NO_MEM;
    tzset();
    unsigned display_offset = (unsigned)abs(offset);
    ESP_LOGI(TAG, "Mission timezone %s (UTC%c%02u:%02u)",
             CONFIG_DJI_H1_TIMEZONE_NAME, offset >= 0 ? '+' : '-',
             display_offset / 60U, display_offset % 60U);
    s_queue = xQueueCreate(SYNC_QUEUE_LENGTH, sizeof(sync_observation_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(clock_sync_task, "clock_sync", SYNC_TASK_STACK, NULL,
                    SYNC_TASK_PRIORITY, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t clock_sync_submit(const ab_realtime_data_t *data, int64_t b_receive_us)
{
    if (data == NULL || b_receive_us < 0) return ESP_ERR_INVALID_ARG;
    if (s_queue == NULL) return ESP_ERR_INVALID_STATE;
    sync_observation_t observation = {
        .a_raw_ms = data->a_monotonic_ms,
        .b_receive_us = b_receive_us,
        .utc_valid = (data->valid_flags & (1U << 2)) != 0 &&
                     data->utc_milliseconds <= 999,
    };
    if (observation.utc_valid) observation.utc_ms =
        (uint64_t)data->utc_seconds * 1000ULL + data->utc_milliseconds;
    if (xQueueSend(s_queue, &observation, 0) != pdTRUE) {
        s_queue_drops++;
        ESP_LOGW(TAG, "Observation queue full; drops=%lu",
                 (unsigned long)s_queue_drops);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t clock_sync_timestamp(int64_t b_monotonic_us, record_time_t *out)
{
    if (out == NULL || b_monotonic_us < 0) return ESP_ERR_INVALID_ARG;
    sync_snapshot_t model;
    taskENTER_CRITICAL(&s_snapshot_lock);
    model = s_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);
    memset(out, 0, sizeof(*out));
    out->b_monotonic_us = (uint64_t)b_monotonic_us;
    out->valid_flags = RECORD_TIME_VALID_B_MONOTONIC;
    out->sync_generation = model.generation;
    int64_t age_us = b_monotonic_us - model.last_observation_us;
    if (age_us < 0) age_us = 0;
    out->sync_age_ms = age_us / 1000 > UINT32_MAX ? UINT32_MAX :
                       (uint32_t)(age_us / 1000);
    clock_sync_state_t state = model.state;
    if (age_us >= SYNC_INVALID_US) state = CLOCK_SYNC_INVALID;
    else if (age_us >= SYNC_HOLDOVER_US && state == CLOCK_SYNC_LOCKED) {
        state = CLOCK_SYNC_HOLDOVER;
    }
    out->sync_state = state;
    if (model.model_valid && (state == CLOCK_SYNC_LOCKED ||
                              state == CLOCK_SYNC_HOLDOVER)) {
        uint64_t a_ms, utc_ms;
        if (project_model_time(&model, b_monotonic_us, &a_ms, NULL)) {
            out->a_monotonic_ms = a_ms;
            out->valid_flags |= RECORD_TIME_VALID_A_MONOTONIC;
            if (project_model_time(&model, b_monotonic_us, NULL, &utc_ms)) {
                out->utc_ms = utc_ms;
                out->valid_flags |= RECORD_TIME_VALID_UTC;
            }
        }
    }
    return ESP_OK;
}

esp_err_t clock_sync_self_test(void)
{
    /* Test the critical wire conversion and size without touching live state. */
    const uint32_t seconds = 1767225600U;
    const uint16_t milliseconds = 999U;
    uint64_t utc = (uint64_t)seconds * 1000ULL + milliseconds;
    if (utc != 1767225600999ULL || sizeof(record_time_t) != 32) return ESP_FAIL;
    /* Unsigned subtraction is the documented forward-wrap extension rule. */
    uint32_t before = UINT32_MAX - 99U, after = 100U;
    if ((uint32_t)(after - before) != 200U) return ESP_FAIL;
    sync_snapshot_t model = {.utc_valid = true, .utc_offset_ms = 1000000};
    if (utc_discontinuous(&model, 1000, 1001000) ||
        !utc_discontinuous(&model, 1000, 1002500)) return ESP_FAIL;
    model.model_valid = true;
    model.anchor_a_ms = 5000;
    model.anchor_b_us = 2000000;
    model.b_us_per_a_ms = 1000.0;
    uint64_t projected_a, projected_utc;
    if (!project_model_time(&model, 2500000, &projected_a, &projected_utc) ||
        projected_a != 5500 || projected_utc != 1005500) return ESP_FAIL;
    return ESP_OK;
}
