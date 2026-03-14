#pragma once

/**
 * @brief  Connect to the HiveMQ broker (credentials from secrets.h).
 *         Must be called after wifi_manager_init().
 *
 *         Configures a Last Will Testament so the broker automatically
 *         publishes tcs/status/online = "false" (retained) if the device
 *         drops the connection without a clean disconnect.
 */
void mqtt_client_start(void);

/**
 * @brief  Publish a QoS-1 message.  Safe to call from any task.
 * @param  topic    Null-terminated MQTT topic string.
 * @param  payload  Null-terminated payload string.
 */
void mqtt_publish(const char *topic, const char *payload);

/**
 * @brief  Publish a QoS-1 retained message.  Safe to call from any task.
 *
 *         The broker stores the last value and delivers it to any subscriber
 *         that connects later — ideal for persistent state like online/enabled.
 *
 * @param  topic    Null-terminated MQTT topic string.
 * @param  payload  Null-terminated payload string.
 */
void mqtt_publish_retained(const char *topic, const char *payload);

/**
 * @brief  Register a callback for incoming command messages.
 *         The callback is invoked from the MQTT event task with
 *         null-terminated topic and data strings.
 * @param  callback  Function called on every received tcs/cmd/... message.
 */
void mqtt_subscribe_commands(void (*callback)(const char *topic, const char *data));
