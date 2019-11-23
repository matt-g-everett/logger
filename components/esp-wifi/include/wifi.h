#ifndef __WIFI_H
#define __WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "freertos/FreeRTOS.h"

void wifi_init(const char *ssid, const char *password);
uint8_t wifi_wait(TickType_t xTicksToWait);
char* wifi_get_ip(void);

#ifdef __cplusplus
}
#endif

#endif
