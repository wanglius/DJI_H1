#include "h1.h"

#include <stdlib.h>

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "esp_log.h"


static const char *TAG = "H1";


// ============================================================
// H1 protocol constants
// ============================================================

#define H1_CMD_HEADER_0     0xCC
#define H1_CMD_HEADER_1     0x01

#define H1_RESP_HEADER_0    0xCC
#define H1_RESP_HEADER_1    0x81

#define H1_PACKET_END_0     0x0D
#define H1_PACKET_END_1     0x0A


#define H1_CMD_GET_INFO          0x08
#define H1_CMD_SET_EXPOSURE      0x0A
#define H1_CMD_GET_EXPOSURE      0x0B
#define H1_CMD_GET_SINGLE_SPECTRUM   0x32

#define H1_CMD_START_STREAM          0x33
#define H1_CMD_STOP_STREAM           0x04

#define H1_RX_CLEAR_MAX_US           100000
#define H1_RX_CLEAR_MAX_BYTES        32768U

// ============================================================
// Internal forward declarations
// ============================================================

static esp_err_t h1_validate_packet(
    const uint8_t *packet,
    size_t length,
    uint8_t expected_type
);

static uint16_t h1_read_u16_le(
    const uint8_t *p
);

static int16_t h1_read_i16_le(
    const uint8_t *p
);

static uint32_t h1_read_u32_le(
    const uint8_t *p
);

static esp_err_t h1_send_command_ex(
    h1_device_t *dev,
    uint8_t command_type,
    const uint8_t *payload,
    size_t payload_length,
    bool clear_rx
);


// ============================================================
// Utility: H1 checksum
//
// checksum = low 8 bits of sum of all bytes before checksum
// ============================================================

static uint8_t h1_checksum(
    const uint8_t *data,
    size_t length)
{
    uint32_t sum = 0;

    for (size_t i = 0; i < length; i++) {
        sum += data[i];
    }

    return (uint8_t)(sum & 0xFF);
}


// ============================================================
// Clear UART RX before sending a new request
// ============================================================

static void h1_clear_rx(
    h1_device_t *dev)
{
    uint8_t temp[64];
    size_t total_drained = 0;
    int64_t deadline_us = esp_timer_get_time() + H1_RX_CLEAR_MAX_US;

    while (esp_timer_get_time() < deadline_us &&
           total_drained < H1_RX_CLEAR_MAX_BYTES) {

        uint8_t level = 0;


        if (sc16_rx_level(
                dev->channel,
                &level) != ESP_OK) {

            break;
        }


        if (level == 0) {
            break;
        }


        /*
         * With the dual RX service active, level describes bytes in the
         * software stream buffer and can be greater than this local array.
         * Drain it in bounded chunks to avoid overwriting the task stack.
         */
        size_t read_size =
            level < sizeof(temp)
            ? level
            : sizeof(temp);

        size_t got = 0;


        if (sc16_read_fifo(
                dev->channel,
                temp,
                read_size,
                &got) != ESP_OK) {

            break;
        }

        total_drained += got;
        if (got == 0) {
            vTaskDelay(1);
        }
    }

    if (esp_timer_get_time() >= deadline_us ||
        total_drained >= H1_RX_CLEAR_MAX_BYTES) {
        ESP_LOGW(TAG, "RX purge bounded after %u bytes on UART-%c",
                 (unsigned)total_drained,
                 dev->channel == SC16_CHANNEL_A ? 'A' : 'B');
    }
}


// ============================================================
// Receive one complete H1 packet
//
// Assumptions for this first version:
// - one request outstanding
// - response length <= buffer size
// - blocking reception is acceptable
// ============================================================

static esp_err_t h1_receive_packet(
    h1_device_t *dev,
    uint8_t *buffer,
    size_t buffer_size,
    size_t *packet_length,
    uint32_t timeout_ms)
{
    if (dev == NULL ||
        buffer == NULL ||
        packet_length == NULL) {

        return ESP_ERR_INVALID_ARG;
    }

    size_t count = 0;
    size_t expected_length = 0;

    int64_t start_us =
        esp_timer_get_time();

    int64_t deadline_us =
        start_us +
        ((int64_t)timeout_ms * 1000);


    while (esp_timer_get_time() < deadline_us) {

        uint8_t level = 0;

        esp_err_t ret =
            sc16_rx_level(
                dev->channel,
                &level
            );

        if (ret != ESP_OK) {
            return ret;
        }

        if (level == 0) {
            /*
             * Block instead of merely yielding. The acquisition tasks run
             * above IDLE1, so taskYIELD() can still starve the watched idle
             * task while the RX service is waiting for the next UART bytes.
             * With a 1 kHz tick and per-channel software buffering, this
             * bounds the wait to 1 ms without risking the SC16 FIFO.
             */
            vTaskDelay(1);
            continue;
        }


        size_t read_size;

        /*
         * A UART overrun can remove any byte in the stream. Search for the
         * response signature byte-by-byte so the next intact frame can be
         * recovered instead of interpreting spectrum data as a new length.
         */
        if (count < 2) {
            uint8_t byte = 0;
            size_t got = 0;
            ret = sc16_read_fifo(dev->channel, &byte, 1, &got);
            if (ret != ESP_OK) {
                return ret;
            }
            if (got == 0) {
                continue;
            }

            if (count == 0) {
                if (byte == H1_RESP_HEADER_0) {
                    buffer[count++] = byte;
                }
            } else if (byte == H1_RESP_HEADER_1) {
                buffer[count++] = byte;
            } else if (byte == H1_RESP_HEADER_0) {
                buffer[0] = byte;
                count = 1;
            } else {
                count = 0;
            }
            continue;
        }


        /*
         * Before packet length is known,
         * read only enough to obtain the
         * complete 5-byte H1 header.
         */
        if (expected_length == 0) {

            size_t header_remaining =
                5 - count;

            read_size =
                level < header_remaining
                ? level
                : header_remaining;
        }

        else {

            /*
             * Never consume bytes belonging
             * to the next stream frame.
             */
            size_t packet_remaining =
                expected_length - count;

            read_size =
                level < packet_remaining
                ? level
                : packet_remaining;
        }


        if (count + read_size > buffer_size) {
            return ESP_ERR_NO_MEM;
        }


        size_t got = 0;

        ret =
            sc16_read_fifo(
                dev->channel,
                &buffer[count],
                read_size,
                &got
            );

        if (ret != ESP_OK) {
            return ret;
        }

        count += got;


        if (expected_length == 0 &&
            count >= 5) {

            expected_length =
                ((size_t)buffer[2]) |
                ((size_t)buffer[3] << 8) |
                ((size_t)buffer[4] << 16);


            if (expected_length < 9 ||
                expected_length > buffer_size) {
                /* Keep a possible first header byte and continue scanning. */
                count = buffer[4] == H1_RESP_HEADER_0 ? 1 : 0;
                if (count == 1) {
                    buffer[0] = H1_RESP_HEADER_0;
                }
                expected_length = 0;
                continue;
            }
        }


        if (expected_length > 0 &&
            count == expected_length) {

            *packet_length =
                expected_length;

            return ESP_OK;
        }
    }


    ESP_LOGE(
        TAG,
        "RX timeout after %lu ms, received %u bytes",
        (unsigned long)timeout_ms,
        (unsigned)count
    );

    return ESP_ERR_TIMEOUT;
}

/**
 * decode a spectrum packet
 */
static esp_err_t h1_decode_spectrum_packet(
    const uint8_t *response,
    size_t response_length,
    uint8_t expected_type,
    h1_spectrum_frame_t *frame)
{
    if (response == NULL ||
        frame == NULL) {

        return ESP_ERR_INVALID_ARG;
    }


    esp_err_t ret =
        h1_validate_packet(
            response,
            response_length,
            expected_type
        );

    if (ret != ESP_OK) {
        return ret;
    }


    const size_t OFFSET_EXPOSURE_STATUS = 6;
    const size_t OFFSET_EXPOSURE_TIME   = 7;

    const size_t OFFSET_SCALE =
        6
        + 1
        + 4
        + 47 * 4
        + 1 * 4
        + 3 * 4
        + 16 * 4;

    const size_t OFFSET_SPECTRUM =
        OFFSET_SCALE + 2;

    const size_t TRAILER_SIZE = 3;


    if (response_length <
        OFFSET_SPECTRUM + TRAILER_SIZE) {

        return ESP_ERR_INVALID_SIZE;
    }


    memset(frame, 0, sizeof(*frame));


    frame->exposure_status =
        (h1_exposure_status_t)
        response[OFFSET_EXPOSURE_STATUS];


    frame->exposure_us =
        h1_read_u32_le(
            &response[OFFSET_EXPOSURE_TIME]
        );


    frame->spectrum_scale =
        h1_read_i16_le(
            &response[OFFSET_SCALE]
        );


    size_t spectrum_bytes =
        response_length
        - OFFSET_SPECTRUM
        - TRAILER_SIZE;


    if ((spectrum_bytes % 2) != 0) {
        return ESP_ERR_INVALID_SIZE;
    }


    size_t sample_count =
        spectrum_bytes / 2;


    if (sample_count >
        H1_MAX_SPECTRUM_SAMPLES) {

        return ESP_ERR_INVALID_SIZE;
    }


    frame->sample_count =
        sample_count;


    for (size_t i = 0;
         i < sample_count;
         i++) {

        frame->spectrum[i] =
            h1_read_u16_le(
                &response[
                    OFFSET_SPECTRUM + i * 2
                ]
            );
    }


    return ESP_OK;
}

// ============================================================
// Validate H1 response packet
// ============================================================

static esp_err_t h1_validate_packet(
    const uint8_t *packet,
    size_t length,
    uint8_t expected_type)
{
    if (packet == NULL || length < 9) {
        return ESP_ERR_INVALID_ARG;
    }


    // Header
    if (packet[0] != H1_RESP_HEADER_0 ||
        packet[1] != H1_RESP_HEADER_1) {

        ESP_LOGE(TAG, "Bad packet header");

        return ESP_ERR_INVALID_RESPONSE;
    }


    // Declared packet length
    uint32_t declared_length =
        ((uint32_t)packet[2]) |
        ((uint32_t)packet[3] << 8) |
        ((uint32_t)packet[4] << 16);

    if (declared_length != length) {

        ESP_LOGE(
            TAG,
            "Length mismatch: declared=%lu actual=%u",
            (unsigned long)declared_length,
            (unsigned)length
        );

        return ESP_ERR_INVALID_SIZE;
    }


    // Data type
    if (packet[5] != expected_type) {

        ESP_LOGE(
            TAG,
            "Unexpected response type: 0x%02X",
            packet[5]
        );

        return ESP_ERR_INVALID_RESPONSE;
    }


    // Terminator
    if (packet[length - 2] != H1_PACKET_END_0 ||
        packet[length - 1] != H1_PACKET_END_1) {

        ESP_LOGE(TAG, "Bad packet terminator");

        return ESP_ERR_INVALID_RESPONSE;
    }


    // Checksum is byte before 0D 0A
    uint8_t received_checksum =
        packet[length - 3];

    uint8_t calculated_checksum =
        h1_checksum(
            packet,
            length - 3
        );

    if (received_checksum != calculated_checksum) {

        ESP_LOGE(
            TAG,
            "Checksum mismatch: received=0x%02X calculated=0x%02X",
            received_checksum,
            calculated_checksum
        );

        return ESP_ERR_INVALID_CRC;
    }


    return ESP_OK;
}


// ============================================================
// Send H1 command
// ============================================================

static esp_err_t h1_send_command(
    h1_device_t *dev,
    uint8_t command_type,
    const uint8_t *payload,
    size_t payload_length)
{
    return h1_send_command_ex(
        dev,
        command_type,
        payload,
        payload_length,
        true
    );
}

static esp_err_t h1_send_command_ex(
    h1_device_t *dev,
    uint8_t command_type,
    const uint8_t *payload,
    size_t payload_length,
    bool clear_rx)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }


    /*
     * Packet:
     *
     * CC 01
     * LEN[3]
     * CMD
     * payload...
     * checksum
     * 0D 0A
     *
     * total length = 9 + payload length
     */

    size_t total_length =
        9 + payload_length;

    if (total_length > 64) {
        return ESP_ERR_INVALID_SIZE;
    }


    uint8_t packet[64];

    packet[0] = H1_CMD_HEADER_0;
    packet[1] = H1_CMD_HEADER_1;

    packet[2] =
        (uint8_t)(total_length & 0xFF);

    packet[3] =
        (uint8_t)((total_length >> 8) & 0xFF);

    packet[4] =
        (uint8_t)((total_length >> 16) & 0xFF);

    packet[5] = command_type;


    if (payload_length > 0 &&
        payload != NULL) {

        memcpy(
            &packet[6],
            payload,
            payload_length
        );
    }


    size_t checksum_index =
        6 + payload_length;

    packet[checksum_index] =
        h1_checksum(
            packet,
            checksum_index
        );

    packet[checksum_index + 1] =
        H1_PACKET_END_0;

    packet[checksum_index + 2] =
        H1_PACKET_END_1;


    if (clear_rx) {
        h1_clear_rx(dev);
    }


    size_t written =
        sc16_write(
            dev->channel,
            packet,
            total_length,
            100
        );

    if (written != total_length) {

        ESP_LOGE(
            TAG,
            "TX incomplete: %u / %u bytes",
            (unsigned)written,
            (unsigned)total_length
        );

        return ESP_FAIL;
    }


    return ESP_OK;
}

// little-endian helpers
static uint16_t h1_read_u16_le(
    const uint8_t *p)
{
    return
        ((uint16_t)p[0]) |
        ((uint16_t)p[1] << 8);
}


static int16_t h1_read_i16_le(
    const uint8_t *p)
{
    uint16_t value =
        ((uint16_t)p[0]) |
        ((uint16_t)p[1] << 8);

    return (int16_t)value;
}


static uint32_t h1_read_u32_le(
    const uint8_t *p)
{
    return
        ((uint32_t)p[0]) |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
}

// ============================================================
// Public API
// ============================================================

esp_err_t h1_init(
    h1_device_t *dev,
    sc16_channel_t channel)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(dev, 0, sizeof(*dev));

    dev->channel = channel;
    dev->initialized = true;

    return ESP_OK;
}


// ============================================================
// Device information
// ============================================================

esp_err_t h1_get_device_info(
    h1_device_t *dev)
{
    if (dev == NULL || !dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }


    /*
     * Payload:
     *
     * 0x18 = request 24 bytes device info
     */

    const uint8_t payload[] = {
        0x18
    };


    esp_err_t ret =
        h1_send_command(
            dev,
            H1_CMD_GET_INFO,
            payload,
            sizeof(payload)
        );

    if (ret != ESP_OK) {
        return ret;
    }


    uint8_t response[64];
    size_t response_length = 0;


    ret =
        h1_receive_packet(
            dev,
            response,
            sizeof(response),
            &response_length,
            1000
        );

    if (ret != ESP_OK) {
        return ret;
    }


    ret =
        h1_validate_packet(
            response,
            response_length,
            H1_CMD_GET_INFO
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /*
     * Expected response:
     *
     * 33 bytes total
     * 24-byte info field at offset 6
     */

    if (response_length != 33) {

        ESP_LOGE(
            TAG,
            "Unexpected device info packet size: %u",
            (unsigned)response_length
        );

        return ESP_ERR_INVALID_SIZE;
    }


    memcpy(
        dev->device_info,
        &response[6],
        24
    );

    dev->device_info[24] = '\0';


    ESP_LOGI(
        TAG,
        "Device info: %s",
        dev->device_info
    );


    return ESP_OK;
}


// ============================================================
// Set exposure mode
// ============================================================

esp_err_t h1_set_exposure_mode(
    h1_device_t *dev,
    h1_exposure_mode_t mode)
{
    if (dev == NULL || !dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }


    uint8_t payload;

    switch (mode) {

        case H1_EXPOSURE_MANUAL:
            payload = 0x00;
            break;

        case H1_EXPOSURE_AUTO:
            payload = 0x01;
            break;

        default:
            return ESP_ERR_INVALID_ARG;
    }


    esp_err_t ret =
        h1_send_command(
            dev,
            H1_CMD_SET_EXPOSURE,
            &payload,
            1
        );

    if (ret != ESP_OK) {
        return ret;
    }


    uint8_t response[32];
    size_t response_length = 0;


    ret =
        h1_receive_packet(
            dev,
            response,
            sizeof(response),
            &response_length,
            1000
        );

    if (ret != ESP_OK) {
        return ret;
    }


    ret =
        h1_validate_packet(
            response,
            response_length,
            H1_CMD_SET_EXPOSURE
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /*
     * Expected ACK packet length = 10 bytes
     *
     * payload:
     *
     * 0x00 success
     * 0x15 invalid command
     * 0xFF unsupported mode
     */

    if (response_length != 10) {
        return ESP_ERR_INVALID_SIZE;
    }


    uint8_t result =
        response[6];


    if (result == 0x00) {

        ESP_LOGI(
            TAG,
            "Exposure mode set successfully"
        );

        return ESP_OK;
    }


    ESP_LOGE(
        TAG,
        "Set exposure mode failed: 0x%02X",
        result
    );


    return ESP_FAIL;
}


// ============================================================
// Get exposure mode
// ============================================================

esp_err_t h1_get_exposure_mode(
    h1_device_t *dev,
    h1_exposure_mode_t *mode)
{
    if (dev == NULL ||
        mode == NULL ||
        !dev->initialized) {

        return ESP_ERR_INVALID_ARG;
    }


    esp_err_t ret =
        h1_send_command(
            dev,
            H1_CMD_GET_EXPOSURE,
            NULL,
            0
        );

    if (ret != ESP_OK) {
        return ret;
    }


    uint8_t response[32];
    size_t response_length = 0;


    ret =
        h1_receive_packet(
            dev,
            response,
            sizeof(response),
            &response_length,
            1000
        );

    if (ret != ESP_OK) {
        return ret;
    }


    ret =
        h1_validate_packet(
            response,
            response_length,
            H1_CMD_GET_EXPOSURE
        );

    if (ret != ESP_OK) {
        return ret;
    }


    if (response_length != 10) {
        return ESP_ERR_INVALID_SIZE;
    }


    switch (response[6]) {

        case 0x00:
            *mode = H1_EXPOSURE_MANUAL;
            break;

        case 0x01:
            *mode = H1_EXPOSURE_AUTO;
            break;

        default:

            ESP_LOGE(
                TAG,
                "Unknown exposure mode: 0x%02X",
                response[6]
            );

            return ESP_ERR_INVALID_RESPONSE;
    }


    return ESP_OK;
}

// ============================================================
// Single-frame spectrum acquisition
//
// H1 command type = 0x32
// ============================================================

esp_err_t h1_get_single_spectrum(
    h1_device_t *dev,
    h1_spectrum_frame_t *frame)
{
    if (dev == NULL ||
        frame == NULL ||
        !dev->initialized) {

        return ESP_ERR_INVALID_ARG;
    }


    memset(frame, 0, sizeof(*frame));


    // --------------------------------------------------------
    // Send command 0x32
    //
    // No payload
    // --------------------------------------------------------

    esp_err_t ret =
        h1_send_command(
            dev,
            H1_CMD_GET_SINGLE_SPECTRUM,
            NULL,
            0
        );

    if (ret != ESP_OK) {
        return ret;
    }


    /*
     * Example packet is 1706 bytes.
     * 2048 gives enough room for the documented full spectrum.
     *
     * Use heap instead of putting this large temporary buffer
     * on the task stack.
     */

    const size_t rx_buffer_size = 2048;

    uint8_t *response =
        malloc(rx_buffer_size);

    if (response == NULL) {

        ESP_LOGE(
            TAG,
            "Unable to allocate spectrum RX buffer"
        );

        return ESP_ERR_NO_MEM;
    }


    size_t response_length = 0;


    /*
     * Auto exposure could make this command significantly
     * slower than the short configuration commands.
     *
     * Give the H1 up to 7 seconds for this first test.
     */

    ret =
        h1_receive_packet(
            dev,
            response,
            rx_buffer_size,
            &response_length,
            7000
        );

    if (ret != ESP_OK) {

        free(response);

        return ret;
    }


    ESP_LOGI(
        TAG,
        "Spectrum packet received: %u bytes",
        (unsigned)response_length
    );


    // --------------------------------------------------------
    // Packet integrity
    // --------------------------------------------------------

    ret =
        h1_validate_packet(
            response,
            response_length,
            H1_CMD_GET_SINGLE_SPECTRUM
        );

    if (ret != ESP_OK) {

        free(response);

        return ret;
    }


    /*
     * Layout before spectrum:
     *
     * bytes 0..5
     *     packet header / length / type
     *
     * byte 6
     *     exposure status
     *
     * bytes 7..10
     *     exposure time uint32
     *
     * bytes 11..198
     *     47 photometric float values
     *     47 * 4 = 188 bytes
     *
     * bytes 199..202
     *     blue hazard
     *
     * bytes 203..214
     *     NIR, 3 * 4
     *
     * bytes 215..278
     *     plant parameters, 16 * 4
     *
     * bytes 279..280
     *     spectrum scale int16
     *
     * byte 281 onward
     *     spectrum uint16[]
     *
     * final 3 bytes
     *     checksum + 0D 0A
     */

    const size_t OFFSET_EXPOSURE_STATUS = 6;
    const size_t OFFSET_EXPOSURE_TIME   = 7;
    const size_t OFFSET_SCALE           = 279;
    const size_t OFFSET_SPECTRUM        = 281;

    const size_t TRAILER_SIZE = 3;


    if (response_length <
        OFFSET_SPECTRUM + TRAILER_SIZE) {

        ESP_LOGE(
            TAG,
            "Spectrum packet is too short"
        );

        free(response);

        return ESP_ERR_INVALID_SIZE;
    }


    // --------------------------------------------------------
    // Decode basic metadata
    // --------------------------------------------------------

    frame->exposure_status =
        (h1_exposure_status_t)
        response[OFFSET_EXPOSURE_STATUS];


    frame->exposure_us =
        h1_read_u32_le(
            &response[OFFSET_EXPOSURE_TIME]
        );


    frame->spectrum_scale =
        h1_read_i16_le(
            &response[OFFSET_SCALE]
        );


    // --------------------------------------------------------
    // Determine number of samples from packet size
    // --------------------------------------------------------

    size_t spectrum_bytes =
        response_length
        - OFFSET_SPECTRUM
        - TRAILER_SIZE;


    if ((spectrum_bytes % 2) != 0) {

        ESP_LOGE(
            TAG,
            "Spectrum byte count is odd: %u",
            (unsigned)spectrum_bytes
        );

        free(response);

        return ESP_ERR_INVALID_SIZE;
    }


    size_t sample_count =
        spectrum_bytes / 2;


    if (sample_count >
        H1_MAX_SPECTRUM_SAMPLES) {

        ESP_LOGE(
            TAG,
            "Too many spectrum samples: %u",
            (unsigned)sample_count
        );

        free(response);

        return ESP_ERR_INVALID_SIZE;
    }


    frame->sample_count =
        sample_count;


    // --------------------------------------------------------
    // Decode spectrum samples
    // --------------------------------------------------------

    for (size_t i = 0;
         i < sample_count;
         i++) {

        size_t offset =
            OFFSET_SPECTRUM +
            (i * 2);

        frame->spectrum[i] =
            h1_read_u16_le(
                &response[offset]
            );
    }


    free(response);


    ESP_LOGI(
        TAG,
        "Decoded spectrum: samples=%u exposure=%lu us scale=%d",
        (unsigned)frame->sample_count,
        (unsigned long)frame->exposure_us,
        (int)frame->spectrum_scale
    );


    return ESP_OK;
}

/**
 * Start streaming mode (cmd 0x33)
 */
esp_err_t h1_start_stream(
    h1_device_t *dev)
{
    if (dev == NULL ||
        !dev->initialized) {

        return ESP_ERR_INVALID_STATE;
    }


    if (dev->streaming) {
        return ESP_OK;
    }


    /*
     * H1 continuous spectrum command:
     *
     * CC 01 09 00 00 33 09 0D 0A
     */

    esp_err_t ret =
        h1_send_command(
            dev,
            H1_CMD_START_STREAM,
            NULL,
            0
        );


    if (ret != ESP_OK) {
        return ret;
    }


    dev->streaming = true;
    dev->stream_frame_count = 0;


    ESP_LOGI(
        TAG,
        "Continuous stream started"
    );


    return ESP_OK;
}

/**
 * Stop streaming mode
 */
esp_err_t h1_stop_stream(
    h1_device_t *dev)
{
    if (dev == NULL ||
        !dev->initialized) {

        return ESP_ERR_INVALID_STATE;
    }


    if (!dev->streaming) {
        return ESP_OK;
    }


    /*
     * Do NOT clear RX first.
     */
    esp_err_t ret =
        h1_send_command_ex(
            dev,
            H1_CMD_STOP_STREAM,
            NULL,
            0,
            false
        );


    if (ret != ESP_OK) {
        return ret;
    }


    dev->streaming = false;


    /*
     * Give any already-transmitting frame
     * time to finish arriving.
     */
    vTaskDelay(
        pdMS_TO_TICKS(250)
    );


    h1_clear_rx(dev);


    ESP_LOGI(
        TAG,
        "Continuous stream stopped"
    );


    return ESP_OK;
}

esp_err_t h1_read_stream_frame(
    h1_device_t *dev,
    h1_spectrum_frame_t *frame,
    uint32_t timeout_ms)
{
    if (dev == NULL ||
        frame == NULL) {

        return ESP_ERR_INVALID_ARG;
    }


    if (!dev->initialized ||
        !dev->streaming) {

        return ESP_ERR_INVALID_STATE;
    }


    size_t response_length = 0;


    esp_err_t ret =
        h1_receive_packet(
            dev,
            dev->rx_packet,
            sizeof(dev->rx_packet),
            &response_length,
            timeout_ms
        );


    if (ret != ESP_OK) {
        return ret;
    }


    ret =
        h1_decode_spectrum_packet(
            dev->rx_packet,
            response_length,
            H1_CMD_START_STREAM,   // expected response type = 0x33
            frame
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Invalid stream frame"
        );

        return ret;
    }


    dev->stream_frame_count++;


    return ESP_OK;
}
