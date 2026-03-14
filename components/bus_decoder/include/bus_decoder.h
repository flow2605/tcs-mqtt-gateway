#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>   // <-- for bool

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t data;
    uint8_t  length;   // 0 = short, 1 = long
    bool     crc_ok;
} tcs_frame_t;

/** Callback type used to deliver decoded frames. */
typedef void (*tcs_frame_callback_t)(const tcs_frame_t *frame);

/**
 * @brief Initialize the TCS bus decoder.
 * @param cb Callback called each time a valid frame is decoded.
 */
esp_err_t bus_decoder_init(tcs_frame_callback_t cb);

#ifdef __cplusplus
}
#endif
