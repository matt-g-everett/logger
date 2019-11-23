#include <time.h>

#include "esp_log.h"
#include "mqtt_client.h"

#include "crc32.h"
#include "wifi.h"

#include "mqtt_ota.h"

static const char *TAG = "OTA";
static const char *VERSION_MSG_JSON = "{\"ip\":\"%s\",\"type\":\"%s\",\"version\":\"%s\"}";
const static int UPDATE_TIMEOUT = 60; // 60 seconds to update the software before a reset


mqtt_ota_state_handle_t mqtt_ota_init(esp_mqtt_client_handle_t client, const char *software,
        const char *version) {
    mqtt_ota_state_t *state = malloc(sizeof(mqtt_ota_state_t));
    state->client = client;
    state->software = software;
    state->version = version;

    state->connected = false;
    state->advertised_crc32 = 0;
    state->ota_state = RUNNING;
    state->update_msg_id = 0;
    state->update_crc = 0;
    state->update_channel[0] = '\0';
    state->update_handle = 0;
    state->update_partition = NULL;

    return state;
}

int mqtt_ota_get_connected(mqtt_ota_state_handle_t state) {
    return state->connected;
}

void mqtt_ota_set_connected(mqtt_ota_state_handle_t state, int connected) {
    state->connected = connected;
}

static void reset_upgrade(mqtt_ota_state_handle_t state, int hard) {
    if (hard) {
        esp_restart();
    }

    if (strlen(state->update_channel) > 0) {
        int msg_id = esp_mqtt_client_unsubscribe(state->client, state->update_channel);
        ESP_LOGI(TAG, "Unsubscribed from %s, msg_id=%d", state->update_channel, msg_id);
        strncpy(state->update_channel, "", sizeof(state->update_channel));
    }

    state->update_crc = 0;
    state->advertised_crc32 = 0;
    state->update_msg_id = -1;
    time(&state->update_start_time);
    state->ota_state = RUNNING;
}

void mqtt_ota_task(void *pParam) {
    mqtt_ota_state_handle_t state = (mqtt_ota_state_handle_t)pParam;

    int msg_id;
    char message[100];
    time_t current_time;
    uint8_t wifi_connected = wifi_wait(portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi state after initial wait is %s.", wifi_connected ? "connected" : "not connected");

    while (1) {
        // Check that wifi is still connected and the update is not started
        wifi_connected = wifi_wait(0);
        if (state->ota_state > RUNNING) {
            time(&current_time);
            if (difftime(current_time, state->update_start_time) > UPDATE_TIMEOUT) {
                ESP_LOGI(TAG, "Update timed-out, aborting.");
                reset_upgrade(state, false);
            }
        }
        else if (state->connected && wifi_connected) {

            sprintf(message, VERSION_MSG_JSON, wifi_get_ip(), state->software, state->version);
            msg_id = esp_mqtt_client_publish(state->client, "home/ota/report", message, 0, 0, 0);
            ESP_LOGI(TAG, "Published version message msg_id=%d: %s", msg_id, message);
        }
        else {
            ESP_LOGI(TAG, "WiFi not connected, skipping publish...");
        }

        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

void mqtt_ota_subscribe(esp_mqtt_client_handle_t client, const char *advertise_topic) {
    int msg_id = esp_mqtt_client_subscribe(client, advertise_topic, 0);
    ESP_LOGI(TAG, "Sent subscribe to %s, msg_id=%d", advertise_topic, msg_id);
}

esp_err_t mqtt_ota_handle_data(mqtt_ota_state_handle_t state, esp_mqtt_event_handle_t event, const char *advertise_topic) {
    int process_bin = false;

    if (event->topic_len > 0 && strncmp(event->topic, "home/ota/advertise", event->topic_len) == 0) {
        reset_upgrade(state, false);

        // Decode advertise message
        char software_type[16], version[32];
        sscanf(event->data, "%s%s%s%x", software_type, version, state->update_channel, &state->advertised_crc32);
        ESP_LOGI(TAG, "home/ota/advertise: type=%s, version=%s, channel=%s, crc32=%08x",
            software_type, version, state->update_channel, state->advertised_crc32);

        if (state->advertised_crc32 == 0xffffffff) {
            ESP_LOGI(TAG, "TOPIC: %.*s, OFFSET: %d, LENGTH: %d, TOTAL: %d",
                event->topic_len, event->topic, event->current_data_offset, event->data_len, event->total_data_len);
        }

        state->ota_state = AWAITING_DOWNLOAD;

        int msg_id = esp_mqtt_client_subscribe(event->client, state->update_channel, 0);
        ESP_LOGI(TAG, "Subscription %d sent for channel %s.", msg_id, state->update_channel);
    }
    else if (event->topic_len > 0 && state->ota_state == RUNNING) {
        ESP_LOGI(TAG, "Waiting for home/ota/advertise message. Ignoring message on %.*s.", event->topic_len, event->topic);
        ESP_LOGI(TAG, "TOPIC: %.*s, OFFSET: %d, LENGTH: %d, TOTAL: %d",
            event->topic_len, event->topic, event->current_data_offset, event->data_len, event->total_data_len);
    }
    else if (state->ota_state == AWAITING_DOWNLOAD) {
        if (event->topic_len > 0 && strncmp(event->topic, state->update_channel, event->topic_len) == 0 &&
            event->current_data_offset == 0) {

            // Start receiving update
            ESP_LOGI(TAG, "start on %s", state->update_channel);
            state->ota_state = DOWNLOADING;
            state->update_msg_id = event->msg_id;
            process_bin = true;

            const esp_partition_t *configured = esp_ota_get_boot_partition();
            const esp_partition_t *running = esp_ota_get_running_partition();

            if (configured != running) {
                ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                    configured->address, running->address);
                ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
            }
            ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
                running->type, running->subtype, running->address);


            state->update_partition = esp_ota_get_next_update_partition(NULL);
            ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
                state->update_partition->subtype, state->update_partition->address);

            esp_err_t err = esp_ota_begin(state->update_partition, OTA_SIZE_UNKNOWN, &(state->update_handle));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                reset_upgrade(state, true);
                return 0;
            }
            ESP_LOGI(TAG, "esp_ota_begin succeeded");
        }
        else {
            ESP_LOGI(TAG, "Expecting start of software binary. Aborting.");
            ESP_LOGI(TAG, "TOPIC: %.*s, OFFSET: %d, LENGTH: %d, TOTAL: %d",
                event->topic_len, event->topic, event->current_data_offset, event->data_len, event->total_data_len);
            reset_upgrade(state, true);
        }
    }
    else if (event->topic_len == 0 && state->ota_state == DOWNLOADING && state->update_msg_id == event->msg_id ) {
        process_bin = true;
    }
    else {
        if (event->current_data_offset ==  0) {
            ESP_LOGI(TAG, "Unexpected state, aborting.");
            ESP_LOGI(TAG, "TOPIC: %.*s, OFFSET: %d, LENGTH: %d, TOTAL: %d",
                event->topic_len, event->topic, event->current_data_offset, event->data_len, event->total_data_len);
        }

        reset_upgrade(state, true);
    }

    if (process_bin) {
        esp_err_t err = esp_ota_write(state->update_handle, (const void *)event->data, event->data_len);

        crc32(event->data, (size_t)event->data_len, &state->update_crc);

        if (event->current_data_offset + event->data_len == event->total_data_len) {
            ESP_LOGI(TAG, "Upgrade message: completed CRC32 %08x %s", state->update_crc,
                state->update_crc == state->advertised_crc32 ? "(confirmed)" : "(error)");

            if (state->update_crc == state->advertised_crc32) {
                ESP_LOGI(TAG, "Running esp_ota_end");
                if (esp_ota_end(state->update_handle) == ESP_OK) {
                    ESP_LOGI(TAG, "Running esp_ota_set_boot_partition");
                    err = esp_ota_set_boot_partition(state->update_partition);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "***** UPGRADE COMPLETE *****");
                        reset_upgrade(state, true);
                    }
                    else {
                        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
                    }
                }
                else {
                    ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
                }
            }
        }
    }

    return ESP_OK;
}
