#pragma once
#include "esp_err.h"
#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>

// A TCS bus frame to be transmitted.
typedef struct {
    uint32_t data;        // Payload: 16-bit (short) or 32-bit (long)
    bool     long_frame;  // false = 16-bit frame, true = 32-bit frame
} tcs_tx_frame_t;

/**
 * @brief  Initialize the TCS bus writer on the given GPIO pin.
 *         Spawns a dedicated FreeRTOS task pinned to Core 1.
 * @param  tx_gpio  Output GPIO connected to the NPN transistor base.
 */
esp_err_t bus_writer_init(gpio_num_t tx_gpio);

/**
 * @brief  Enqueue a frame for transmission (non-blocking).
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if the queue is full.
 */
esp_err_t bus_writer_send(const tcs_tx_frame_t *frame);
