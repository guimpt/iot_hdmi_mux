/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"

// GPIO mapping
#define TS3_EN     GPIO_NUM_8
#define TS3_SEL1   GPIO_NUM_2
#define TS3_SEL2   GPIO_NUM_3

static const char *TAG = "IOT_HDMI_MUX";

// Store latest received payload
static char last_mux_cmd[8] = "DISABLED";

typedef enum {
    TS3_STATE_DISABLED,
    TS3_STATE_D0A_ONLY,
    TS3_STATE_D0B_ONLY,
    TS3_STATE_ALL_A,
    TS3_STATE_ALL_B
} ts3_state_t;

void ts3dv642_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TS3_EN) | (1ULL << TS3_SEL1) | (1ULL << TS3_SEL2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    // Set default state: disabled
    gpio_set_level(TS3_EN, 0);
}

void ts3dv642_set_state(ts3_state_t state) {
    switch (state) {
        case TS3_STATE_DISABLED:
            gpio_set_level(TS3_EN, 0); // Disable switch
            break;
        case TS3_STATE_D0A_ONLY:
            gpio_set_level(TS3_EN, 1);
            gpio_set_level(TS3_SEL1, 0);
            gpio_set_level(TS3_SEL2, 0);
            break;
        case TS3_STATE_D0B_ONLY:
            gpio_set_level(TS3_EN, 1);
            gpio_set_level(TS3_SEL1, 0);
            gpio_set_level(TS3_SEL2, 1);
            break;
        case TS3_STATE_ALL_A:
            gpio_set_level(TS3_EN, 1);
            gpio_set_level(TS3_SEL1, 1);
            gpio_set_level(TS3_SEL2, 0);
            break;
        case TS3_STATE_ALL_B:
            gpio_set_level(TS3_EN, 1);
            gpio_set_level(TS3_SEL1, 1);
            gpio_set_level(TS3_SEL2, 1);
            break;
        default:
            ESP_LOGW(TAG, "Unknown state");
            break;
    }
    ESP_LOGI(TAG, "Switch set to state %d", state);
}

static void mqtt5_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            esp_mqtt_client_subscribe(event->client, "hdmi_mux", 0);
            break;

        case MQTT_EVENT_DATA:
            if (event->topic_len && event->data_len) {
                char topic[event->topic_len + 1];
                char payload[event->data_len + 1];
                memcpy(topic, event->topic, event->topic_len);
                topic[event->topic_len] = '\0';
                memcpy(payload, event->data, event->data_len);
                payload[event->data_len] = '\0';

                ESP_LOGI(TAG, "Received topic: %s, payload: %s", topic, payload);
                if (strcmp(topic, "hdmi_mux") == 0) {
                    strncpy(last_mux_cmd, payload, sizeof(last_mux_cmd) - 1);
                    last_mux_cmd[sizeof(last_mux_cmd) - 1] = '\0';
                }
            }
            break;

        default:
            break;
    }
}

// Task to poll the state every 5 seconds
static void mux_control_task(void *arg) {
    while (1) {
        if (strcmp(last_mux_cmd, "A") == 0) {
            ts3dv642_set_state(TS3_STATE_ALL_A);
        } else if (strcmp(last_mux_cmd, "B") == 0) {
            ts3dv642_set_state(TS3_STATE_ALL_B);
        } else {
            ts3dv642_set_state(TS3_STATE_DISABLED);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// Main app start
static void hdmi_mux_app_start(void) {
    ts3dv642_init();

    esp_mqtt_client_config_t mqtt5_cfg = {
        .broker.address.uri = "mqtt://mqtt.flespi.io",
        .credentials.username = "BoSauuQkeDan8lz3FEI5ReEU6jmk0BwHIHn6VSCO9lRkOPEnnDLa08UFKeuf7FTV",
        .credentials.authentication.password = "",
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt5_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt5_event_handler, NULL);
    esp_mqtt_client_start(client);

    xTaskCreate(mux_control_task, "mux_control_task", 2048, NULL, 5, NULL);
}

void app_main(void)
{

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_example", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ts3dv642_init();

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    hdmi_mux_app_start();
}
