#ifndef __ESP_LOGGER_H
#define __ESP_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"
#include "ds18b20.h"

typedef struct {
    esp_mqtt_client_handle_t mqtt_client;
    uint8_t connected;
    uint8_t gpio_pin;
    DS18B20_RESOLUTION resolution;
    TickType_t interval;
} iotp_ds18b20_t;

typedef iotp_ds18b20_t *iotp_ds18b20_handle_t;

iotp_ds18b20_handle_t iotp_ds18b20_init(esp_mqtt_client_handle_t mqtt_client, uint8_t gpio_pin);
int iotp_ds18b20_get_connected(iotp_ds18b20_handle_t state);
void iotp_ds18b20_set_connected(iotp_ds18b20_handle_t state, int connected);
void iotp_ds18b20_task(void *pParam);

#ifdef __cplusplus
}
#endif

#endif
