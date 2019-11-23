#include "freertos/FreeRTOS.h"

#ifndef __WIFI_H
#define __WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init(void);
uint8_t wifi_wait(TickType_t xTicksToWait);
char* wifi_get_ip(void);

#ifdef __cplusplus
}
#endif

#endif
