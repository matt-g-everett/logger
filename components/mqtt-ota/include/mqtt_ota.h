#pragma once
#ifndef MQTT_OTA_H
#define MQTT_OTA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_ota_ops.h"

typedef enum {
    RUNNING,
    AWAITING_DOWNLOAD,
    DOWNLOADING
} ota_state_t;

typedef struct {
    esp_mqtt_client_handle_t client;
    const char *software;
    const char *version;

    int connected;
    uint32_t advertised_crc32;
    ota_state_t ota_state;
    time_t update_start_time;
    int update_msg_id;
    uint32_t update_crc;
    char update_channel[32];

    esp_ota_handle_t update_handle;
    const esp_partition_t *update_partition;
} mqtt_ota_state_t;

typedef mqtt_ota_state_t *mqtt_ota_state_handle_t;

mqtt_ota_state_t* mqtt_ota_init(esp_mqtt_client_handle_t client, const char *software, const char *version);
int mqtt_ota_get_connected(mqtt_ota_state_handle_t state);
void mqtt_ota_set_connected(mqtt_ota_state_handle_t state, int connected);
void mqtt_ota_task(void *pParam);
void mqtt_ota_subscribe(esp_mqtt_client_handle_t client, const char *advertise_topic);
esp_err_t mqtt_ota_handle_data(mqtt_ota_state_handle_t handle, esp_mqtt_event_handle_t event, const char *advertise_topic);

#ifdef __cplusplus
}
#endif

#endif // MQTT_OTA_H
