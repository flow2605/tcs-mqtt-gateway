#include "mqtt_client_mod.h"

#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include <string.h>

#include "secrets.h"

extern const uint8_t hivemq_ca_pem_start[] asm("_binary_hivemq_ca_pem_start");

static const char *TAG = "mqtt_mod";
static esp_mqtt_client_handle_t client = NULL;

// Callback registered by the application to receive incoming commands
static void (*command_callback)(const char *topic, const char *data) = NULL;

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");

        // Publish retained online status — overrides the Last Will "false" value.
        // Any subscriber (e.g. dashboard) that connects later will immediately
        // see "true" without waiting for the next event.
        esp_mqtt_client_publish(event->client, "tcs/status/online", "true", 0, 1, /*retain=*/1);

        // Re-subscribe to all command topics on every (re-)connect
        if (command_callback) {
            const char *topics[] = {
                "tcs/cmd/enable",
                "tcs/cmd/status",
                "tcs/cmd/action",
                "tcs/cmd/send"
            };
            for (size_t i = 0; i < sizeof(topics) / sizeof(topics[0]); i++) {
                esp_mqtt_client_subscribe(event->client, topics[i], 1);
                ESP_LOGI(TAG, "Subscribed: %s", topics[i]);
            }
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected — will retry automatically");
        break;

    case MQTT_EVENT_DATA:
        {
            char topic[128];
            char payload[256];
            snprintf(topic,   sizeof(topic),   "%.*s", event->topic_len, event->topic);
            snprintf(payload, sizeof(payload), "%.*s", event->data_len,  event->data);
            ESP_LOGI(TAG, "RX %s = %s", topic, payload);

            if (strncmp(topic, "tcs/cmd/", 8) == 0 && command_callback)
                command_callback(topic, payload);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error type=%d", event->error_handle->error_type);
        break;

    default:
        break;
    }
}

void mqtt_client_start(void)
{
    const esp_mqtt_client_config_t cfg = {
        .broker = {
            .address.uri = BROKER_URI,
            .verification.certificate = (const char *)hivemq_ca_pem_start,
        },
        .credentials = {
            .username = MQTT_USERNAME,
            .authentication.password = MQTT_PASSWORD,
        },
        // Last Will Testament: if the device drops the connection unexpectedly,
        // the broker publishes "false" to tcs/status/online automatically.
        .session = {
            .last_will = {
                .topic  = "tcs/status/online",
                .msg    = "false",
                .qos    = 1,
                .retain = 1,
            },
        },
    };
    client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
}

void mqtt_publish(const char *topic, const char *payload)
{
    if (client)
        esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
}

void mqtt_publish_retained(const char *topic, const char *payload)
{
    if (client)
        esp_mqtt_client_publish(client, topic, payload, 0, 1, /*retain=*/1);
}

void mqtt_subscribe_commands(void (*callback)(const char *topic, const char *data))
{
    command_callback = callback;
}
