#include <driver/gpio.h>

#include "esp_log.h"

#include "iotp_heater.h"
#include "freertos/event_groups.h"

static const char *TAG = "HEATER";

iotp_heater_handle_t iotp_heater_init(uint8_t gpio_pin) {
    iotp_heater_t *state = malloc(sizeof(iotp_heater_t));
    state->gpio_pin = gpio_pin;
    state->current = true;
    state->desired = false;
    state->ntp_synced = false;
    state->interval = 1000;
    state->holdoff = 5;
    state->last_state_change = time(NULL);

    gpio_config_t config;
    config.pin_bit_mask = (1 << GPIO_NUM_17);
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLUP_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&config);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "GPIO OK");
    }
    else
    {
        ESP_LOGI(TAG, "GPIO Not OK");
    }

    return state;
}

void iotp_heater_start(iotp_heater_handle_t handle) {
    iotp_heater_t *state = (iotp_heater_t*)handle;
    if (!state->desired) {
        ESP_LOGI(TAG, "Heater starting (pending)");
        state->desired = true;
    }
    else {
        ESP_LOGI(TAG, "Heater already started");
    }
}

void iotp_heater_stop(iotp_heater_handle_t handle) {
    iotp_heater_t *state = (iotp_heater_t*)handle;
    if (state->desired) {
        ESP_LOGI(TAG, "Heater stopping (pending)");
        state->desired = false;
    }
    else {
        ESP_LOGI(TAG, "Heater already stopped");
    }
}

void iotp_heater_set_ntp(iotp_heater_handle_t handle, bool synced) {
    iotp_heater_t *state = (iotp_heater_t*)handle;
    ESP_LOGI(TAG, "NTP set to %s", synced ? "synced" : "unsynced");
    state->ntp_synced = synced;
}

void iotp_heater_set_reading(iotp_heater_handle_t handle, float reading) {
    iotp_heater_t *state = (iotp_heater_t*)handle;
    state->reading = reading;
    ESP_LOGI(TAG, "New reading: %.1f", state->reading);
}

static void evaluate_heater_state(iotp_heater_t *state) {
    ESP_LOGI(TAG, "Evaluating heater state");
    if (state->desired != state->current && (time(NULL) > state->last_state_change + state->holdoff)) {
        ESP_LOGI(TAG, "Changing heater state");
        state->current = state->desired;
        state->last_state_change = time(NULL);
    }
}

void iotp_heater_task(void *pParam) {
    iotp_heater_t *state = (iotp_heater_t*)pParam;

    // Initialise the wake time
    TickType_t last_wake_time = xTaskGetTickCount();
    while (true)
    {
        evaluate_heater_state(state);
        vTaskDelayUntil(&last_wake_time, state->interval / portTICK_PERIOD_MS);
    }
}
