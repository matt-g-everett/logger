#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "tcpip_adapter.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "crc32.h"

#define STACK_SIZE 4096
#define GPIO_DS18B20_0       (CONFIG_ONE_WIRE_GPIO)
#define MAX_DEVICES          (8)
#define DS18B20_RESOLUTION   (DS18B20_RESOLUTION_12_BIT)
#define SAMPLE_PERIOD        (1000)   // milliseconds

static const char *TAG = "LOGGER";
static const char *SOFTWARE = "logger";
static const char *OK_MSG_JSON = "{\"ip\":\"%s\",\"id\":\"%02x%02x%02x%02x%02x%02x\",\"temp\":%.1f}";
static const char *ERROR_MSG_JSON = "{\"ip\":\"%s\",\"id\":\"%02x%02x%02x%02x%02x%02x\",\"error\":\"%s\"}";
static const char *VERSION_MSG_JSON = "{\"ip\":\"%s\",\"type\":\"%s\",\"version\":\"%s\"}";
static const char *DS18B20_ERROR_MSG_DEVICE = "deviceError";
static const char *DS18B20_ERROR_MSG_CRC = "crcError";
static const char *DS18B20_ERROR_MSG_OWB = "owbError";
static const char *DS18B20_ERROR_MSG_NULL = "nullValueError";
static const char *DS18B20_ERROR_MSG_UNKNOWN = "unknownError";

// Embedded files
extern const uint8_t version_start[] asm("_binary_version_txt_start");
extern const uint8_t version_end[] asm("_binary_version_txt_end");

const static int CONNECTED_BIT = BIT0;

static EventGroupHandle_t wifi_event_group;
static esp_mqtt_client_handle_t _client;
static char _ip[16];
static uint8_t _connected = 0;
static int update_started = false;
static int update_msg_id = 0;
static uint32_t update_crc = 0;
static uint32_t advertised_crc32 = 0;
static char update_channel[32] = "";

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    int process_bin = false;
    char software_type[16];
    char version[32];

    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            _connected = 1;
            _client = client;
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

            msg_id = esp_mqtt_client_subscribe(_client, "home/ota/advertise", 1);
            ESP_LOGI(TAG, "Sent subscribe to home/ota/advertise, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            _connected = 0;
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
            ESP_LOGI(TAG, "MQTT_EVENT_DATA, msg_id=%d, topic=%.*s, offset=%d, len=%d",
                event->msg_id, event->topic_len, event->topic, event->current_data_offset, event->total_data_len);

            if (event->topic_len > 0 && strncmp(event->topic, "home/ota/advertise", event->topic_len) == 0) {
                ESP_LOGI(TAG, "home/ota/advertise message");

                // Decode advertise message
                sscanf(event->data, "%s%s%s%x", software_type, version, update_channel, &advertised_crc32);
                ESP_LOGI(TAG, "type=%s, version=%s, channel=%s, crc32=%08x",
                    software_type, version, update_channel, advertised_crc32);

                if (!update_started) {
                    msg_id = esp_mqtt_client_subscribe(_client, update_channel, 1);
                    ESP_LOGI(TAG, "Subscription %d sent for channel %s.", msg_id, update_channel);

                    update_crc = 0;
                    update_started = true;
                }
                else {
                    ESP_LOGI(TAG, "Update has already started, ignoring home/ota/advertise message for channel %s.", update_channel);
                }
            }
            else if (event->topic_len > 0 && strncmp(event->topic, update_channel, event->topic_len) == 0) {
                ESP_LOGI(TAG, "%s message", update_channel);
                if (event->current_data_offset == 0) {
                    // Start receiving update
                    ESP_LOGI(TAG, "Upgrade message: start");
                    update_msg_id = event->msg_id;
                    process_bin = true;
                }
            }
            else if (event->topic_len == 0 && update_started && update_msg_id == event->msg_id) {
                ESP_LOGI(TAG, "Upgrade message: continuation");
                process_bin = true;
            }
            else {
                ESP_LOGI(TAG, "Upgrade message: upgrade unsuccessful, ignoring data");
                esp_mqtt_client_unsubscribe(_client, update_channel);
            }

            if (process_bin) {
                ESP_LOGI(TAG, "Processing bytes %d-%d.", event->current_data_offset, event->current_data_offset + event->data_len);

                crc32(event->data, (size_t)event->data_len, &update_crc);

                if (event->current_data_offset + event->data_len == event->total_data_len) {
                    ESP_LOGI(TAG, "Upgrade message: completed CRC32 %08x %s", update_crc,
                        update_crc == advertised_crc32 ? "(confirmed)" : "(error)");
                    esp_mqtt_client_unsubscribe(_client, update_channel);
                    update_started = false;
                }
            }

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

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;
        default:
            break;
    }
    return;
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            const ip_event_got_ip_t *event = (const ip_event_got_ip_t *) event_data;
            sprintf(_ip, IPSTR, IP2STR(&event->ip_info.ip));
            break;
        default:
            break;
    }
    return;
}

static void wifi_init(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "start the WIFI SSID:[%s] password:[%s]", CONFIG_WIFI_SSID, "******");
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Waiting for wifi");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URL,
        .event_handle = mqtt_event_handler,
        .username = CONFIG_MQTT_USERNAME,
        .password = CONFIG_MQTT_PASSWORD
        // .user_context = (void *)your_context
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);
}

uint8_t wifi_wait(TickType_t xTicksToWait) {
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
    return (uint8_t)((bits & CONNECTED_BIT) == CONNECTED_BIT);
}

void version_report_task(void *pParam) {
    int msg_id;
    char message[100];
    uint8_t wifi_connected = wifi_wait(portMAX_DELAY);
    ESP_LOGI(TAG, "WiFi state after initial wait is %s.", wifi_connected ? "connected" : "not connected");

    while (1) {
        // Check that wifi is still connected
        wifi_connected = wifi_wait(0);
        if (_connected && wifi_connected) {
            sprintf(message, VERSION_MSG_JSON, _ip, SOFTWARE, (const char *)version_start);
            msg_id = esp_mqtt_client_publish(_client, "home/ota/report", message, 0, 1, 0);
            ESP_LOGI(TAG, "Published version message msg_id=%d: %s", msg_id, message);
        }
        else {
            ESP_LOGI(TAG, "WiFi not connected, skipping publish...");
        }

        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

void logging_task()
{
    // Override global log level
    esp_log_level_set("*", ESP_LOG_INFO);

    // Stable readings require a brief period before communication
    vTaskDelay(2000.0 / portTICK_PERIOD_MS);

    // Create a 1-Wire bus, using the RMT timeslot driver
    OneWireBus * owb;
    owb_rmt_driver_info rmt_driver_info;
    owb = owb_rmt_initialize(&rmt_driver_info, GPIO_DS18B20_0, RMT_CHANNEL_1, RMT_CHANNEL_0);
    owb_use_crc(owb, true);  // enable CRC check for ROM code

    // Find all connected devices
    ESP_LOGI(TAG, "Find devices:");
    OneWireBus_ROMCode device_rom_codes[MAX_DEVICES] = {0};
    int num_devices = 0;
    OneWireBus_SearchState search_state = {0};
    bool found = false;
    owb_search_first(owb, &search_state, &found);
    while (found)
    {
        char rom_code_s[17];
        owb_string_from_rom_code(search_state.rom_code, rom_code_s, sizeof(rom_code_s));
        ESP_LOGI(TAG, "  %d : %s", num_devices, rom_code_s);
        device_rom_codes[num_devices] = search_state.rom_code;
        ++num_devices;
        owb_search_next(owb, &search_state, &found);
    }
    ESP_LOGI(TAG, "Found %d device%s", num_devices, num_devices == 1 ? "" : "s");

    // In this example, if a single device is present, then the ROM code is probably
    // not very interesting, so just print it out. If there are multiple devices,
    // then it may be useful to check that a specific device is present.

    if (num_devices == 1)
    {
        // For a single device only:
        OneWireBus_ROMCode rom_code;
        owb_status status = owb_read_rom(owb, &rom_code);
        if (status == OWB_STATUS_OK)
        {
            char rom_code_s[OWB_ROM_CODE_STRING_LENGTH];
            owb_string_from_rom_code(rom_code, rom_code_s, sizeof(rom_code_s));
            ESP_LOGI(TAG, "Single device %s present", rom_code_s);
        }
        else
        {
            ESP_LOGE(TAG, "An error occurred reading ROM code: %d", status);
        }
    }
    else
    {
        // Search for a known ROM code (LSB first):
        // For example: 0x1502162ca5b2ee28
        OneWireBus_ROMCode known_device = {
            .fields.family = { 0x28 },
            .fields.serial_number = { 0xee, 0xb2, 0xa5, 0x2c, 0x16, 0x02 },
            .fields.crc = { 0x15 },
        };
        char rom_code_s[OWB_ROM_CODE_STRING_LENGTH];
        owb_string_from_rom_code(known_device, rom_code_s, sizeof(rom_code_s));
        bool is_present = false;

        owb_status search_status = owb_verify_rom(owb, known_device, &is_present);
        if (search_status == OWB_STATUS_OK)
        {
            ESP_LOGI(TAG, "Device %s is %s", rom_code_s, is_present ? "present" : "not present");
        }
        else
        {
            ESP_LOGE(TAG, "An error occurred searching for known device: %d", search_status);
        }
    }

    // Create DS18B20 devices on the 1-Wire bus
    DS18B20_Info * devices[MAX_DEVICES] = {0};
    for (int i = 0; i < num_devices; ++i)
    {
        DS18B20_Info * ds18b20_info = ds18b20_malloc();  // heap allocation
        devices[i] = ds18b20_info;

        if (num_devices == 1)
        {
            ESP_LOGI(TAG, "Single device optimisations enabled");
            ds18b20_init_solo(ds18b20_info, owb); // only one device on bus
        }
        else
        {
            ds18b20_init(ds18b20_info, owb, device_rom_codes[i]); // associate with bus and device
        }
        ds18b20_use_crc(ds18b20_info, true); // enable CRC check for temperature readings
        ds18b20_set_resolution(ds18b20_info, DS18B20_RESOLUTION);
    }

    // Read temperatures more efficiently by starting conversions on all devices at the same time
    if (num_devices > 0)
    {
        int msg_id;
        char message[400];
        uint8_t wifi_connected = wifi_wait(portMAX_DELAY);
        ESP_LOGI(TAG, "WiFi state after initial wait is %s.", wifi_connected ? "connected" : "not connected");

        TickType_t last_wake_time = xTaskGetTickCount();

        while (1)
        {
            last_wake_time = xTaskGetTickCount();

            ds18b20_convert_all(owb);

            // In this application all devices use the same resolution,
            // so use the first device to determine the delay
            ds18b20_wait_for_conversion(devices[0]);

            // Read the results immediately after conversion otherwise it may fail
            // (logging before reading may take too long)
            float readings[MAX_DEVICES] = { 0 };
            DS18B20_ERROR errors[MAX_DEVICES] = { 0 };

            for (int i = 0; i < num_devices; ++i)
            {
                errors[i] = ds18b20_read_temp(devices[i], &readings[i]);
            }

            // Check that wifi is still connected
            wifi_connected = wifi_wait(0);
            if (_connected && wifi_connected) {
                // Print results in a separate loop, after all have been read
                for (int i = 0; i < num_devices; ++i)
                {
                    if (errors[i] == DS18B20_OK) {
                        sprintf(message, OK_MSG_JSON,
                            _ip,
                            device_rom_codes[i].fields.serial_number[0],
                            device_rom_codes[i].fields.serial_number[1],
                            device_rom_codes[i].fields.serial_number[2],
                            device_rom_codes[i].fields.serial_number[3],
                            device_rom_codes[i].fields.serial_number[4],
                            device_rom_codes[i].fields.serial_number[5],
                            readings[i]);
                    }
                    else {
                        const char *error_msg;
                        switch (errors[i]) {
                            case DS18B20_ERROR_DEVICE:
                                error_msg = DS18B20_ERROR_MSG_DEVICE;
                                break;
                            case DS18B20_ERROR_CRC:
                                error_msg = DS18B20_ERROR_MSG_CRC;
                                break;
                            case DS18B20_ERROR_OWB:
                                error_msg = DS18B20_ERROR_MSG_OWB;
                                break;
                            case DS18B20_ERROR_NULL:
                                error_msg = DS18B20_ERROR_MSG_NULL;
                                break;
                            default:
                                error_msg = DS18B20_ERROR_MSG_UNKNOWN;
                                break;
                        }

                        sprintf(message, ERROR_MSG_JSON,
                            _ip,
                            device_rom_codes[i].fields.serial_number[0],
                            device_rom_codes[i].fields.serial_number[1],
                            device_rom_codes[i].fields.serial_number[2],
                            device_rom_codes[i].fields.serial_number[3],
                            device_rom_codes[i].fields.serial_number[4],
                            device_rom_codes[i].fields.serial_number[5],
                            error_msg);
                    }

                    msg_id = esp_mqtt_client_publish(_client, "home/pool", message, 0, 1, 0);
                    ESP_LOGI(TAG, "Published logger message msg_id=%d: %s", msg_id, message);
                }
            }
            else {
                ESP_LOGI(TAG, "WiFi not connected, skipping publish...");
            }

            vTaskDelayUntil(&last_wake_time, SAMPLE_PERIOD / portTICK_PERIOD_MS);
        }
    }

    // clean up dynamically allocated data
    for (int i = 0; i < num_devices; ++i)
    {
        ds18b20_free(&devices[i]);
    }
    owb_uninitialize(owb);

    ESP_LOGI(TAG, "Restarting now.");
    fflush(stdout);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
}

void app_main()
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    nvs_flash_init();
    wifi_init();
    mqtt_app_start();
    // xTaskCreate(logging_task, "logging", STACK_SIZE, NULL, 5, NULL);
    xTaskCreate(version_report_task, "logging", STACK_SIZE, NULL, 5, NULL);
}
