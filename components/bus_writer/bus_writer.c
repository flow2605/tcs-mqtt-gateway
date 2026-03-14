#include "bus_writer.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

// ── Pulse-width constants (µs) ───────────────────────────────────────────────
// The TCS bus uses inverted logic via an NPN transistor:
//   GPIO HIGH → transistor conducts → bus pulled LOW
//   GPIO LOW  → transistor off      → bus HIGH (idle)
// All pulse widths here are for the GPIO signal, not the bus signal.
#define ZERO_US      2000   // pulse width for a 0-bit
#define ONE_US       4000   // pulse width for a 1-bit
#define START_US     6000   // pulse width for the START symbol
#define IDLE_GAP_US  10000  // mandatory idle gap after each frame

static const char *TAG = "bus_writer";
static QueueHandle_t s_tx_queue = NULL;
static gpio_num_t    s_tx_gpio;

// ─── helpers ─────────────────────────────────────────────────────────────
static inline uint8_t tcs_crc(uint32_t data, uint8_t bits)
{
    uint8_t c = 1;
    for (int i = bits - 1; i >= 0; --i)
        c ^= ((data >> i) & 1);
    return c;
}

// Busy-wait until an absolute µs timestamp is reached.
// Used for bit-accurate pulse timing — do not replace with vTaskDelay.
static inline void wait_until(uint64_t target_us)
{
    while (esp_timer_get_time() < target_us) {}
}

// ─── TX worker task ─────────────────────────────────────────────────────
static void bus_writer_task(void *arg)
{
    tcs_tx_frame_t f;

    for (;;) {
        if (!xQueueReceive(s_tx_queue, &f, portMAX_DELAY))
            continue;

        const uint8_t bits = f.long_frame ? 32 : 16;
        const uint8_t crc  = tcs_crc(f.data, bits);

        ESP_LOGI(TAG, "Sending %s frame 0x%08lX, CRC=%d",
                 f.long_frame ? "LONG" : "SHORT", (unsigned long)f.data, crc);

        // Ensure the line is idle (LOW) before starting
        gpio_set_level(s_tx_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(2));

        // ── Time-based transmission ───────────────────────────────────────────
        // Each symbol is a pulse: GPIO goes HIGH for START_US/ONE_US/ZERO_US,
        // then LOW again.  Edges are calculated as absolute timestamps so that
        // any brief scheduling jitter doesn't accumulate across bits.
        uint64_t next_edge = esp_timer_get_time();

        // START pulse: GPIO HIGH for START_US µs
        gpio_set_level(s_tx_gpio, 1);
        next_edge += START_US;

        // Length flag: LOW edge after START, then HIGH for ONE_US (long) or ZERO_US (short)
        wait_until(next_edge);
        gpio_set_level(s_tx_gpio, 0);
        next_edge += (f.long_frame ? ONE_US : ZERO_US);

        // Data bits: toggle on every edge, MSB-first
        wait_until(next_edge);
        gpio_set_level(s_tx_gpio, 1);

        int level = 1;
        for (int i = bits - 1; i >= 0; --i) {
            uint32_t bit = (f.data >> i) & 1;
            next_edge += (bit ? ONE_US : ZERO_US);
            wait_until(next_edge);
            level ^= 1;
            gpio_set_level(s_tx_gpio, level);
        }

        // CRC bit
        next_edge += (crc ? ONE_US : ZERO_US);
        wait_until(next_edge);
        level ^= 1;
        gpio_set_level(s_tx_gpio, level);

        // Return to idle (LOW)
        if (level == 1)
            gpio_set_level(s_tx_gpio, 0);

        // Mandatory inter-frame gap
        esp_rom_delay_us(IDLE_GAP_US);

        ESP_LOGI(TAG, "TX complete");
    }
}

// ─── public API ──────────────────────────────────────────────────────────
esp_err_t bus_writer_init(gpio_num_t tx_gpio)
{
    s_tx_gpio = tx_gpio;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_tx_gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,   // no pull-up (inverted hardware)
        .pull_down_en = GPIO_PULLDOWN_ENABLE,  // pull-down keeps line LOW when idle
        .intr_type    = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    // Idle state: GPIO LOW → bus HIGH via inverting transistor
    gpio_set_level(s_tx_gpio, 0);

    s_tx_queue = xQueueCreate(8, sizeof(tcs_tx_frame_t));
    if (!s_tx_queue) return ESP_FAIL;

    // Pin to Core 1 so busy-wait timing doesn't interfere with Wi-Fi (Core 0)
    xTaskCreatePinnedToCore(bus_writer_task, "bus_writer",
                            4096, NULL, 15, NULL, 1);

    ESP_LOGI(TAG, "Bus writer ready on GPIO %d (inverted output)", s_tx_gpio);
    return ESP_OK;
}

esp_err_t bus_writer_send(const tcs_tx_frame_t *frame)
{
    if (!frame || !s_tx_queue) return ESP_ERR_INVALID_ARG;
    return xQueueSend(s_tx_queue, frame, pdMS_TO_TICKS(50))
           ? ESP_OK : ESP_ERR_TIMEOUT;
}