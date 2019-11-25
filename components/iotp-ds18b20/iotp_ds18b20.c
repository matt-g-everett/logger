#include "esp_log.h"
#include "mqtt_client.h"

#include "owb.h"
#include "owb_rmt.h"
#include "ds18b20.h"
#include "iotp_wifi.h"

#include "iotp_ds18b20.h"

static const char *TAG = "LOGGER";

static const char *OK_MSG_JSON = "{\"ip\":\"%s\",\"id\":\"%02x%02x%02x%02x%02x%02x\",\"temp\":%.1f}";
static const char *ERROR_MSG_JSON = "{\"ip\":\"%s\",\"id\":\"%02x%02x%02x%02x%02x%02x\",\"error\":\"%s\"}";
static const char *DS18B20_ERROR_MSG_DEVICE = "deviceError";
static const char *DS18B20_ERROR_MSG_CRC = "crcError";
static const char *DS18B20_ERROR_MSG_OWB = "owbError";
static const char *DS18B20_ERROR_MSG_NULL = "nullValueError";
static const char *DS18B20_ERROR_MSG_UNKNOWN = "unknownError";

#define MAX_DEVICES (8)

iotp_ds18b20_handle_t iotp_ds18b20_init(esp_mqtt_client_handle_t mqtt_client, uint8_t gpio_pin) {
    iotp_ds18b20_t *state = malloc(sizeof(iotp_ds18b20_t));
    state->mqtt_client = mqtt_client;
    state->connected = false;
    state->gpio_pin = gpio_pin;
    state->resolution = DS18B20_RESOLUTION_12_BIT;
    state->interval = 1000;

    return state;
}

int iotp_ds18b20_get_connected(iotp_ds18b20_handle_t state) {
    return state->connected;
}

void iotp_ds18b20_set_connected(iotp_ds18b20_handle_t state, int connected) {
    state->connected = connected;
}

void iotp_ds18b20_task(void *pParam)
{
    iotp_ds18b20_handle_t state = (iotp_ds18b20_handle_t)pParam;

    // Stable readings require a brief period before communication
    vTaskDelay(2000.0 / portTICK_PERIOD_MS);

    // Create a 1-Wire bus, using the RMT timeslot driver
    OneWireBus * owb;
    owb_rmt_driver_info rmt_driver_info;
    owb = owb_rmt_initialize(&rmt_driver_info, state->gpio_pin, RMT_CHANNEL_1, RMT_CHANNEL_0);
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
        ds18b20_set_resolution(ds18b20_info, state->resolution);
    }

    // Read temperatures more efficiently by starting conversions on all devices at the same time
    if (num_devices > 0)
    {
        int msg_id;
        char message[400];
        uint8_t wifi_connected = wifi_wait(portMAX_DELAY);
        ESP_LOGI(TAG, "WiFi state after initial wait is %s.", wifi_connected ? "connected" : "not connected");

        TickType_t last_wake_time = xTaskGetTickCount();

        while (true)
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
            if (iotp_ds18b20_get_connected(state) && wifi_connected) {
                // Print results in a separate loop, after all have been read
                for (int i = 0; i < num_devices; ++i)
                {
                    if (errors[i] == DS18B20_OK) {
                        sprintf(message, OK_MSG_JSON,
                            wifi_get_ip(),
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
                            wifi_get_ip(),
                            device_rom_codes[i].fields.serial_number[0],
                            device_rom_codes[i].fields.serial_number[1],
                            device_rom_codes[i].fields.serial_number[2],
                            device_rom_codes[i].fields.serial_number[3],
                            device_rom_codes[i].fields.serial_number[4],
                            device_rom_codes[i].fields.serial_number[5],
                            error_msg);
                    }

                    msg_id = esp_mqtt_client_publish(state->mqtt_client, "home/pool", message, 0, 0, 0);
                    ESP_LOGI(TAG, "Published logger message msg_id=%d: %s", msg_id, message);
                }
            }
            else {
                ESP_LOGI(TAG, "WiFi not connected, skipping publish...");
            }

            vTaskDelayUntil(&last_wake_time, state->resolution / portTICK_PERIOD_MS);
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
