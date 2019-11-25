#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"

#include "mqtt_client.h"
#include "ota_mqtt.h"
#include "crc32.h"
#include "wifi.h"
#include "iotp_ds18b20.h"

#define STACK_SIZE 4096

static const char *TAG = "LOGGER";
static const char *SOFTWARE = "logger";

// Embedded files
extern const uint8_t version_start[] asm("_binary_version_txt_start");
extern const uint8_t version_end[] asm("_binary_version_txt_end");

static mqtt_ota_state_handle_t _mqtt_ota_state;
static iotp_ds18b20_handle_t _logger_handle;

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            ESP_LOGI(TAG, "***** logger started, version: %s *****", (const char *)version_start);

            mqtt_ota_set_connected(_mqtt_ota_state, true);
            iotp_ds18b20_set_connected(_logger_handle, true);
            mqtt_ota_subscribe(event->client, CONFIG_OTA_TOPIC_ADVERTISE);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            mqtt_ota_set_connected(_mqtt_ota_state, false);
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            mqtt_ota_handle_data(_mqtt_ota_state, event, CONFIG_OTA_TOPIC_ADVERTISE);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static esp_mqtt_client_handle_t mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URL,
        .event_handle = mqtt_event_handler,
        .username = CONFIG_MQTT_USERNAME,
        .password = CONFIG_MQTT_PASSWORD
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);

    return client;
}

void app_main()
{
    esp_err_t err;

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // OTA app partition table has a smaller NVS partition size than the non-OTA
        // partition table. This size mismatch may cause NVS initialization to fail.
        // If this happens, we erase NVS partition and initialize NVS again.
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    wifi_init(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
    esp_mqtt_client_handle_t client = mqtt_app_start();
    _mqtt_ota_state = mqtt_ota_init(client, SOFTWARE, (const char *)version_start);
    xTaskCreate(mqtt_ota_task, "ota", STACK_SIZE, _mqtt_ota_state, 5, NULL);

    _logger_handle = iotp_ds18b20_init(client, CONFIG_ONE_WIRE_GPIO);
    xTaskCreate(iotp_ds18b20_task, "logging", STACK_SIZE, _logger_handle, 5, NULL);
}
