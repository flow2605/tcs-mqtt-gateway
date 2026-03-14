// Standard ESP-IDF includes
#include "driver/gpio.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

// Project components
#include "bus_decoder.h"
#include "bus_writer.h"
#include "mqtt_client_mod.h"
#include "wifi_manager.h"

// ── GPIO pin assignments ─────────────────────────────────────────────────────
#define TCS_BUS_GPIO 26       // TCS bus TX output (inverted via NPN transistor)
#define LED_STATUS_GPIO 2     // Onboard LED: system enabled/disabled
#define LED_DOOR_OPEN_GPIO 27 // External LED: lights up when a door is unlocked

// ── Timing ───────────────────────────────────────────────────────────────────
#define DOUBLE_RING_TIMEOUT_MS                                                 \
  3000 // Two rings within this window trigger unlock
#define DOOR_LED_DURATION_US 5000000 // Door-open LED stays on for 5 seconds

// ── TCS bus frame codes
// ─────────────────────────────────────────────────────── Captured by sniffing
// the bus of the specific installation. Bell frames:   upper byte 0x03 → ring
// event, no door action. Unlock frames: upper byte 0x13 → door release command
// sent back on the bus. The lower three bytes encode the stairwell and
// apartment address.
#define TCS_FRAME_BELL_OUTER 0x03EAB586U // Outer entrance doorbell
#define TCS_FRAME_BELL_OUTER_ALT                                               \
  0x00001204U // Alternate outer bell code (observed on bus)
#define TCS_FRAME_BELL_INNER 0x03EAB584U        // Inner door doorbell
#define TCS_FRAME_BELL_STAIR_D 0x03EA7B86U      // Stairwell D outer doorbell
#define TCS_FRAME_BELL_STAIR_D_IN 0x03EA7B82U   // Stairwell D inner doorbell
#define TCS_FRAME_UNLOCK_OUTER 0x13EAB586U      // Unlock outer entrance
#define TCS_FRAME_UNLOCK_INNER 0x13EAB584U      // Unlock inner door
#define TCS_FRAME_UNLOCK_STAIR_D 0x13EA7B86U    // Unlock stairwell D (outer)
#define TCS_FRAME_UNLOCK_STAIR_D_IN 0x13EA7B82U // Unlock stairwell D (inner)

static const char *TAG = "main";

// ════════════════════════════════════════════════════════════════════════════
// System state
// ════════════════════════════════════════════════════════════════════════════

typedef struct {
  bool enabled; // When false, all auto-unlock actions are suppressed
  uint64_t
      door_open_led_until; // Absolute µs timestamp until the door LED stays on
} system_state_t;

static system_state_t system_state = {.enabled = true,
                                      .door_open_led_until = 0};

// ════════════════════════════════════════════════════════════════════════════
// LED helpers
// ════════════════════════════════════════════════════════════════════════════

static void led_init(void) {
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << LED_STATUS_GPIO) | (1ULL << LED_DOOR_OPEN_GPIO),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io_conf);
  gpio_set_level(LED_STATUS_GPIO, system_state.enabled ? 1 : 0);
  gpio_set_level(LED_DOOR_OPEN_GPIO, 0);
}

// Reflect current system_state.enabled on the status LED.
static void update_status_led(void) {
  gpio_set_level(LED_STATUS_GPIO, system_state.enabled ? 1 : 0);
}

// Turn on the door LED and schedule it to turn off after DOOR_LED_DURATION_US.
static void trigger_door_open_led(void) {
  system_state.door_open_led_until =
      esp_timer_get_time() + DOOR_LED_DURATION_US;
  gpio_set_level(LED_DOOR_OPEN_GPIO, 1);
  ESP_LOGI(TAG, "Door LED on (5 s)");
}

// Background task: turns the door LED off once its timer expires.
static void led_update_task(void *arg) {
  while (true) {
    if (system_state.door_open_led_until > 0 &&
        esp_timer_get_time() >= system_state.door_open_led_until) {
      gpio_set_level(LED_DOOR_OPEN_GPIO, 0);
      system_state.door_open_led_until = 0;
      ESP_LOGI(TAG, "Door LED off");
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ════════════════════════════════════════════════════════════════════════════
// MQTT command handler
//
// Subscribed topics:
//   tcs/cmd/enable   → "true" / "1"  or  "false" / "0"
//   tcs/cmd/status   → any payload  (triggers a retained-state republish)
//   tcs/cmd/action   → named TCS action:
//                        ring_outer | ring_inner |
//                        unlock_outer | unlock_inner |
//                        ring_stair_d | ring_stair_d_in |
//                        unlock_stair_d | unlock_stair_d_in
//   tcs/cmd/send     → hex string e.g. "0x13EAB586"  (raw frame injection)
//
// Published topics:
//   tcs/status/online    → "true" / "false"  (retained; LWT sets it to "false")
//   tcs/status/enabled   → "true" / "false"  (retained)
//   tcs/event/doorbell   → "outer" | "inner" | "stairwell_d_outer" |
//   "stairwell_d_inner" tcs/event/unlocked   → "outer" | "inner" |
//   "stairwell_d_outer" | "stairwell_d_inner" tcs/bus/rx           → hex string
//   of every received frame tcs/bus/tx           → hex string of every sent
//   frame tcs/log              → human-readable status and error messages
// ════════════════════════════════════════════════════════════════════════════

void mqtt_command_callback(const char *topic, const char *data) {
  ESP_LOGI(TAG, "CMD %s = %s", topic, data);

  if (strcmp(topic, "tcs/cmd/enable") == 0) {
    // ── Enable / disable the auto-unlock logic ──────────────────────────────
    if (strcmp(data, "true") == 0 || strcmp(data, "1") == 0) {
      system_state.enabled = true;
      update_status_led();
      mqtt_publish_retained("tcs/status/enabled", "true");
      mqtt_publish("tcs/log", "System ENABLED");
      ESP_LOGI(TAG, "System ENABLED");
    } else if (strcmp(data, "false") == 0 || strcmp(data, "0") == 0) {
      system_state.enabled = false;
      gpio_set_level(TCS_BUS_GPIO, 0); // ensure TX line is idle
      update_status_led();
      mqtt_publish_retained("tcs/status/enabled", "false");
      mqtt_publish("tcs/log", "System DISABLED");
      ESP_LOGI(TAG, "System DISABLED");
    }

  } else if (strcmp(topic, "tcs/cmd/status") == 0) {
    // ── Republish current retained state ───────────────────────────────────
    mqtt_publish_retained("tcs/status/enabled",
                          system_state.enabled ? "true" : "false");
    char msg[48];
    snprintf(msg, sizeof(msg), "System is %s",
             system_state.enabled ? "ENABLED" : "DISABLED");
    mqtt_publish("tcs/log", msg);

  } else if (strcmp(topic, "tcs/cmd/action") == 0) {
    // ── Send a named TCS bus frame ──────────────────────────────────────────
    uint32_t frame_data = 0;
    if (strcmp(data, "ring_outer") == 0)
      frame_data = TCS_FRAME_BELL_OUTER;
    else if (strcmp(data, "ring_inner") == 0)
      frame_data = TCS_FRAME_BELL_INNER;
    else if (strcmp(data, "unlock_outer") == 0)
      frame_data = TCS_FRAME_UNLOCK_OUTER;
    else if (strcmp(data, "unlock_inner") == 0)
      frame_data = TCS_FRAME_UNLOCK_INNER;
    else if (strcmp(data, "ring_stair_d") == 0)
      frame_data = TCS_FRAME_BELL_STAIR_D;
    else if (strcmp(data, "ring_stair_d_in") == 0)
      frame_data = TCS_FRAME_BELL_STAIR_D_IN;
    else if (strcmp(data, "unlock_stair_d") == 0)
      frame_data = TCS_FRAME_UNLOCK_STAIR_D;
    else if (strcmp(data, "unlock_stair_d_in") == 0)
      frame_data = TCS_FRAME_UNLOCK_STAIR_D_IN;
    else {
      mqtt_publish("tcs/log", "Unknown action");
      return;
    }

    tcs_tx_frame_t frame = {.data = frame_data, .long_frame = true};
    if (bus_writer_send(&frame) == ESP_OK) {
      char hex[16];
      snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)frame_data);
      mqtt_publish("tcs/bus/tx", hex);
      ESP_LOGI(TAG, "Action frame 0x%08lX sent", (unsigned long)frame_data);
    } else {
      mqtt_publish("tcs/log", "Action failed: send error");
      ESP_LOGW(TAG, "Action frame send failed");
    }

  } else if (strcmp(topic, "tcs/cmd/send") == 0) {
    // ── Send a raw hex frame (long frame only) ──────────────────────────────
    // Accepts both "0x13EAB586" and "13EAB586"
    uint32_t payload = 0;
    if (sscanf(data, "0x%lx", (unsigned long *)&payload) != 1 &&
        sscanf(data, "%lx", (unsigned long *)&payload) != 1) {
      mqtt_publish("tcs/log", "Invalid hex format (e.g. 0x13EAB586)");
      return;
    }

    tcs_tx_frame_t frame = {.data = payload, .long_frame = true};
    if (bus_writer_send(&frame) == ESP_OK) {
      char hex[16];
      snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)payload);
      mqtt_publish("tcs/bus/tx", hex);
      ESP_LOGI(TAG, "Custom frame 0x%08lX sent", (unsigned long)payload);
    } else {
      mqtt_publish("tcs/log", "Send failed: queue full");
      ESP_LOGW(TAG, "Custom frame send failed");
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Double-ring detection
//
// The intercom sends a bell frame on every button press.  For security, the
// system only unlocks when the same bell is pressed twice within
// DOUBLE_RING_TIMEOUT_MS.  Each doorbell has its own independent state.
// ════════════════════════════════════════════════════════════════════════════

typedef struct {
  uint64_t last_ring_us; // Timestamp of the previous ring
  uint8_t ring_count;    // How many rings have occurred in the current window
} doorbell_state_t;

static doorbell_state_t doorbell_outer = {0};
static doorbell_state_t doorbell_inner = {0};
static doorbell_state_t doorbell_stair_d = {0};
static doorbell_state_t doorbell_stair_d_in = {0};

// Returns true on the second (or later) ring within the timeout window.
static bool check_double_ring(doorbell_state_t *state) {
  uint64_t now_us = esp_timer_get_time();
  uint64_t delta_us = now_us - state->last_ring_us;

  if (delta_us > (uint64_t)DOUBLE_RING_TIMEOUT_MS * 1000) {
    // Window expired — start a fresh count
    state->ring_count = 1;
    state->last_ring_us = now_us;
    ESP_LOGI(TAG, "  Ring 1 (window reset)");
    return false;
  }

  state->ring_count++;
  state->last_ring_us = now_us;
  ESP_LOGI(TAG, "  Ring #%d (%.1f s since last)", state->ring_count,
           delta_us / 1e6);

  if (state->ring_count >= 2) {
    state->ring_count = 0;
    ESP_LOGI(TAG, "  Double-ring confirmed!");
    return true;
  }
  return false;
}

// ════════════════════════════════════════════════════════════════════════════
// TCS frame handler
//
// Called from the bus_decoder task for every correctly received frame.
// Bell frames (0x03xxxxxx) trigger the double-ring check.
// On confirmation the matching unlock frame (0x13xxxxxx) is queued.
// ════════════════════════════════════════════════════════════════════════════

// Send an unlock frame after a short delay and report the result via MQTT.
// door_name is used for log messages; event_key is the tcs/event/unlocked
// payload.
static void send_unlock(uint32_t unlock_code, const char *door_name,
                        const char *event_key) {
  tcs_tx_frame_t response = {.data = unlock_code, .long_frame = true};
  vTaskDelay(pdMS_TO_TICKS(500)); // brief pause before responding on the bus

  if (bus_writer_send(&response) == ESP_OK) {
    trigger_door_open_led();
    char tx_hex[16];
    snprintf(tx_hex, sizeof(tx_hex), "0x%08lX", (unsigned long)unlock_code);
    mqtt_publish("tcs/bus/tx", tx_hex);
    mqtt_publish("tcs/event/unlocked", event_key);
    ESP_LOGI(TAG, "%s unlocked (0x%08lX)", door_name,
             (unsigned long)unlock_code);
  } else {
    char msg[64];
    snprintf(msg, sizeof(msg), "ERROR: %s unlock failed", door_name);
    mqtt_publish("tcs/log", msg);
    ESP_LOGW(TAG, "%s unlock failed", door_name);
  }
}

static void on_tcs_frame(const tcs_frame_t *f) {
  if (!f || !f->crc_ok)
    return;

  ESP_LOGI(TAG, "RX 0x%08lX (%s)", (unsigned long)f->data,
           f->length ? "LONG" : "SHORT");

  // Publish every received frame for monitoring / debugging
  char payload[16];
  snprintf(payload, sizeof(payload), "0x%08lX", (unsigned long)f->data);
  mqtt_publish("tcs/bus/rx", payload);

  if (!system_state.enabled) {
    gpio_set_level(TCS_BUS_GPIO, 0); // keep TX idle
    return;
  }

  // ── Outer entrance doorbell ──────────────────────────────────────────────
  if (f->data == TCS_FRAME_BELL_OUTER || f->data == TCS_FRAME_BELL_OUTER_ALT) {
    ESP_LOGI(TAG, "Outer doorbell");
    if (check_double_ring(&doorbell_outer))
      send_unlock(TCS_FRAME_UNLOCK_OUTER, "Outer door", "outer");
    else
      mqtt_publish("tcs/event/doorbell", "outer");

    // ── Inner door doorbell ──────────────────────────────────────────────────
  } else if (f->data == TCS_FRAME_BELL_INNER) {
    ESP_LOGI(TAG, "Inner doorbell");
    if (check_double_ring(&doorbell_inner))
      send_unlock(TCS_FRAME_UNLOCK_INNER, "Inner door", "inner");
    else
      mqtt_publish("tcs/event/doorbell", "inner");

    // ── Stairwell D outer doorbell ───────────────────────────────────────────
  } else if (f->data == TCS_FRAME_BELL_STAIR_D) {
    ESP_LOGI(TAG, "Stairwell D outer bell");
    if (check_double_ring(&doorbell_stair_d))
      send_unlock(TCS_FRAME_UNLOCK_STAIR_D, "Stairwell D (outer)",
                  "stairwell_d_outer");
    else
      mqtt_publish("tcs/event/doorbell", "stairwell_d_outer");

    // ── Stairwell D inner doorbell ───────────────────────────────────────────
  } else if (f->data == TCS_FRAME_BELL_STAIR_D_IN) {
    ESP_LOGI(TAG, "Stairwell D inner bell");
    if (check_double_ring(&doorbell_stair_d_in))
      send_unlock(TCS_FRAME_UNLOCK_STAIR_D_IN, "Stairwell D (inner)",
                  "stairwell_d_inner");
    else
      mqtt_publish("tcs/event/doorbell", "stairwell_d_inner");
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Entry point
// ════════════════════════════════════════════════════════════════════════════

void app_main(void) {
  // NVS is required by the Wi-Fi driver
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  esp_log_level_set("*", ESP_LOG_INFO);

  ESP_LOGI(TAG, "═══════════════════════════════════════════");
  ESP_LOGI(TAG, "  tcs-mqtt-gateway  v1.0");
  ESP_LOGI(TAG, "  ESP-IDF v%d.%d.%d", ESP_IDF_VERSION_MAJOR,
           ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
  ESP_LOGI(TAG, "  Double-ring window: %d ms", DOUBLE_RING_TIMEOUT_MS);
  ESP_LOGI(TAG, "═══════════════════════════════════════════");

  led_init();
  ESP_LOGI(TAG, "LEDs: status=GPIO%d  door=GPIO%d", LED_STATUS_GPIO,
           LED_DOOR_OPEN_GPIO);

  // Bring up network and cloud connectivity before starting bus components
  wifi_manager_init();
  mqtt_client_start();
  mqtt_subscribe_commands(mqtt_command_callback);

  // Start TCS bus decoder (GPIO14 input) and writer (GPIO26 output)
  bus_decoder_init(on_tcs_frame);
  bus_writer_init(TCS_BUS_GPIO);

  // Low-priority task that handles LED timeouts
  xTaskCreate(led_update_task, "led_update", 2048, NULL, 5, NULL);

  ESP_LOGI(TAG, "System ready — status: %s",
           system_state.enabled ? "ENABLED" : "DISABLED");

  // Wait for MQTT to connect, then publish initial retained state
  vTaskDelay(pdMS_TO_TICKS(2000));
  mqtt_publish_retained("tcs/status/enabled",
                        system_state.enabled ? "true" : "false");
  mqtt_publish("tcs/log", "System started");

  ESP_LOGI(TAG, "MQTT subscribe topics:");
  ESP_LOGI(TAG, "  tcs/cmd/enable  → true/false");
  ESP_LOGI(TAG, "  tcs/cmd/status  → any");
  ESP_LOGI(TAG, "  tcs/cmd/action  → "
                "ring_outer|ring_inner|unlock_outer|unlock_inner|...");
  ESP_LOGI(TAG, "  tcs/cmd/send    → 0x13EAB586  (raw hex frame)");

  // Main task has nothing left to do — FreeRTOS tasks handle everything
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
