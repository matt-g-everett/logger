#ifndef __IOTP_HEATER_H
#define __IOTP_HEATER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>

#include "freertos/FreeRTOS.h"

typedef struct {
    TickType_t last_state_change;
    TickType_t holdoff;
    bool current;
    bool desired;
    float reading;
    bool ntp_synced;
    uint8_t gpio_pin;
    TickType_t interval;
} iotp_heater_t;

typedef iotp_heater_t *iotp_heater_handle_t;

iotp_heater_handle_t iotp_heater_init(uint8_t gpio_pin);
void iotp_heater_task(void *pParam);
void iotp_heater_start(iotp_heater_handle_t handle);
void iotp_heater_stop(iotp_heater_handle_t handle);
void iotp_heater_set_reading(iotp_heater_handle_t handle, float reading);
void iotp_heater_set_ntp(iotp_heater_handle_t handle, bool synced);

#ifdef __cplusplus
}
#endif

#endif
