#include "bus_decoder.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdbool.h>

// GPIO used to receive the TCS bus signal (after level-shifter / optocoupler)
#define TCS_BUS_GPIO  14

// ISR → decoder task queue depth
#define QUEUE_LEN     256

// ── Pulse-width classification thresholds (µs) ───────────────────────────────
// TCS bus symbols are encoded as pulse widths:
//   0-bit  : 1000 – 2999 µs
//   1-bit  : 3000 – 4999 µs
//   START  : 5000 – 7000 µs  (marks the beginning of a frame)
//   IDLE   : > 9000 µs       (bus at rest / inter-frame gap)
#define ZERO_MIN   1000
#define ZERO_MAX   2999
#define ONE_MIN    3000
#define ONE_MAX    4999
#define START_MIN  5000
#define START_MAX  7000
#define IDLE_MIN   9000

// Number of payload bits (excludes the length flag and CRC bit)
#define DATA_BITS_SHORT  16
#define DATA_BITS_LONG   32

static const char *TAG = "bus_decoder";

// A single edge event captured in the ISR
typedef struct {
    uint32_t dt_us;  // Time since the previous edge in µs
    uint8_t  level;  // GPIO level after the edge (reserved for future use)
} bus_pulse_t;

static QueueHandle_t        s_pulse_queue;
static tcs_frame_callback_t s_frame_cb;

// ── ISR ──────────────────────────────────────────────────────────────────────
// Fires on every edge of the TCS bus signal.
// Measures the time between edges and enqueues the result for the decoder task.
// Kept minimal — no decoding logic here.
static void IRAM_ATTR tcs_isr(void *arg)
{
    static uint32_t last_us = 0;
    uint32_t now = (uint32_t)esp_timer_get_time();
    bus_pulse_t p = {
        .dt_us = now - last_us,
        .level = gpio_get_level(TCS_BUS_GPIO)
    };
    last_us = now;

    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_pulse_queue, &p, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

// ── Helpers ──────────────────────────────────────────────────────────────────

// Map a pulse width to a TCS symbol:
//   0 → zero-bit, 1 → one-bit, 2 → START, 3 → IDLE, -1 → unknown/noise
static inline int classify_pulse(uint32_t us)
{
    if (us >= ZERO_MIN  && us <= ZERO_MAX)  return 0;
    if (us >= ONE_MIN   && us <= ONE_MAX)   return 1;
    if (us >= START_MIN && us <= START_MAX) return 2;
    if (us >= IDLE_MIN)                     return 3;
    return -1;
}

// Reset all FSM state variables to their initial values.
static inline void reset_fsm(bool *in_frame, uint8_t *bit_pos,
                              uint32_t *cur_cmd, uint8_t *crc_calc,
                              bool *long_frame, uint8_t *crc_recv)
{
    *in_frame   = false;
    *bit_pos    = 0;
    *cur_cmd    = 0;
    *crc_calc   = 1;   // CRC initial value matches the TCS reference implementation
    *crc_recv   = 0;
    *long_frame = false;
}

// Deliver a completed frame to the registered callback.
static inline void finalize_frame(uint32_t cur_cmd, bool long_frame, bool crc_ok)
{
    if (s_frame_cb) {
        tcs_frame_t f = { .data = cur_cmd, .length = long_frame, .crc_ok = crc_ok };
        s_frame_cb(&f);
    }
}

// ── Decoder FSM task ─────────────────────────────────────────────────────────
//
// TCS frame structure (transmitted MSB-first):
//   [START pulse] [length flag: 0=short / 1=long] [16 or 32 data bits] [CRC bit]
//
// CRC is a running XOR of all data bits, initialised to 1.
static void bus_decoder_task(void *arg)
{
    bus_pulse_t p;
    uint32_t cur_cmd    = 0;
    uint8_t  bit_pos    = 0;
    uint8_t  crc_calc   = 1;
    uint8_t  crc_recv   = 0;
    bool     long_frame = false;
    bool     in_frame   = false;

    ESP_LOGI(TAG, "Decoder task started on GPIO %d", TCS_BUS_GPIO);

    for (;;) {
        if (!xQueueReceive(s_pulse_queue, &p, portMAX_DELAY)) continue;
        if (p.dt_us < 200) continue;  // ignore micro-spikes

        int symbol = classify_pulse(p.dt_us);

        ESP_LOGD(TAG, "Pulse %lu µs → sym=%d bit=%u", (unsigned long)p.dt_us, symbol, bit_pos);

        // ── IDLE / inter-frame gap → reset ───────────────────────────────────
        if (symbol == 3) {
            if (bit_pos > 1)
                ESP_LOGW(TAG, "Frame timeout after %u bits (gap %lu µs)",
                         bit_pos, (unsigned long)p.dt_us);
            reset_fsm(&in_frame, &bit_pos, &cur_cmd, &crc_calc, &long_frame, &crc_recv);
            continue;
        }

        // ── Unexpected START mid-frame → resync ──────────────────────────────
        if (symbol == 2 && in_frame) {
            ESP_LOGW(TAG, "Unexpected START after %u bits → resync", bit_pos);
            reset_fsm(&in_frame, &bit_pos, &cur_cmd, &crc_calc, &long_frame, &crc_recv);
            // Fall through to handle this pulse as a fresh START below
        }

        // ── Waiting for START ─────────────────────────────────────────────────
        if (!in_frame) {
            if (symbol != 2) continue;  // ignore everything until START
            in_frame = true;
            bit_pos  = 0;
            cur_cmd  = 0;
            crc_calc = 1;
            ESP_LOGI(TAG, "START %lu µs", (unsigned long)p.dt_us);
            continue;
        }

        // ── Length flag (first bit after START) ──────────────────────────────
        if (bit_pos == 0) {
            if (symbol != 0 && symbol != 1) {
                ESP_LOGW(TAG, "Invalid length-flag symbol %d", symbol);
                reset_fsm(&in_frame, &bit_pos, &cur_cmd, &crc_calc, &long_frame, &crc_recv);
                continue;
            }
            long_frame = (symbol == 1);
            ESP_LOGI(TAG, "Frame type: %s", long_frame ? "LONG (32-bit)" : "SHORT (16-bit)");
            bit_pos = 1;
            continue;
        }

        const uint8_t data_bits = long_frame ? DATA_BITS_LONG : DATA_BITS_SHORT;

        // ── Data bits ────────────────────────────────────────────────────────
        if (bit_pos >= 1 && bit_pos <= data_bits) {
            if (symbol != 0 && symbol != 1) {
                ESP_LOGW(TAG, "Invalid data symbol %d at bit %u", symbol, bit_pos);
                reset_fsm(&in_frame, &bit_pos, &cur_cmd, &crc_calc, &long_frame, &crc_recv);
                continue;
            }
            if (symbol) cur_cmd |= (1u << (data_bits - bit_pos));  // MSB-first
            crc_calc ^= (symbol & 1);
            bit_pos++;
            continue;
        }

        // ── CRC bit ───────────────────────────────────────────────────────────
        if (bit_pos == data_bits + 1) {
            if (symbol != 0 && symbol != 1) {
                ESP_LOGW(TAG, "Invalid CRC symbol %d", symbol);
                reset_fsm(&in_frame, &bit_pos, &cur_cmd, &crc_calc, &long_frame, &crc_recv);
                continue;
            }
            crc_recv   = (uint8_t)symbol;
            bool ok    = (crc_recv == crc_calc);
            ESP_LOGI(TAG, "CRC recv=%d calc=%d → %s", crc_recv, crc_calc, ok ? "OK" : "FAIL");
            finalize_frame(cur_cmd, long_frame, ok);
            reset_fsm(&in_frame, &bit_pos, &cur_cmd, &crc_calc, &long_frame, &crc_recv);
            continue;
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t bus_decoder_init(tcs_frame_callback_t cb)
{
    s_frame_cb    = cb;
    s_pulse_queue = xQueueCreate(QUEUE_LEN, sizeof(bus_pulse_t));
    if (!s_pulse_queue) return ESP_FAIL;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << TCS_BUS_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(TCS_BUS_GPIO, tcs_isr, NULL));

    // Higher priority than MQTT/Wi-Fi tasks to avoid pulse-queue overflow
    xTaskCreate(bus_decoder_task, "bus_decoder", 4096, NULL, 10, NULL);
    return ESP_OK;
}
