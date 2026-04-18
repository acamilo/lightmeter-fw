#include <math.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

#include "as7341.h"

static const char *TAG = "lightmeter";

// --- AS7341 bring-up ---------------------------------------------------------

#define AS7341_I2C_ADDR         0x39
#define I2C_PROBE_TIMEOUT_MS    50
#define READ_INTERVAL_MS        2000   // Zigbee report cadence

typedef struct { gpio_num_t a; gpio_num_t b; } pin_pair_t;

// Candidate SDA/SCL pairs probed on boot in both polarities. Covers the
// Arduino-ESP32 default for this variant plus the header-adjacent pairs
// people commonly use for breadboard wiring. Avoids strapping pins (8/9),
// the UART0 console pins (23/24), and the native USB pins (25/26/27).
static const pin_pair_t candidate_pairs[] = {
    { GPIO_NUM_12, GPIO_NUM_22 },
    { GPIO_NUM_4,  GPIO_NUM_5  },
    { GPIO_NUM_1,  GPIO_NUM_0  },
    { GPIO_NUM_10, GPIO_NUM_11 },
    { GPIO_NUM_2,  GPIO_NUM_3  },
};

static i2c_master_bus_handle_t g_bus    = NULL;
static as7341_handle_t         g_sensor = NULL;

static esp_err_t make_bus(gpio_num_t sda, gpio_num_t scl, i2c_master_bus_handle_t *out) {
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, out);
}

static bool try_pair(gpio_num_t sda, gpio_num_t scl, i2c_master_bus_handle_t *bus_out) {
    i2c_master_bus_handle_t bus = NULL;
    if (make_bus(sda, scl, &bus) != ESP_OK) return false;
    bool found = i2c_master_probe(bus, AS7341_I2C_ADDR, I2C_PROBE_TIMEOUT_MS) == ESP_OK;
    if (found) { *bus_out = bus; return true; }
    i2c_del_master_bus(bus);
    return false;
}

static esp_err_t sensor_bringup(void) {
    gpio_num_t sda = -1, scl = -1;
    for (size_t i = 0; i < sizeof(candidate_pairs) / sizeof(candidate_pairs[0]); i++) {
        pin_pair_t p = candidate_pairs[i];
        if (try_pair(p.a, p.b, &g_bus)) { sda = p.a; scl = p.b; break; }
        if (try_pair(p.b, p.a, &g_bus)) { sda = p.b; scl = p.a; break; }
    }
    if (g_bus == NULL) {
        ESP_LOGE(TAG, "AS7341 not found on any candidate pin pair");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "AS7341 at 0x%02x on SDA=%d SCL=%d", AS7341_I2C_ADDR, sda, scl);

    as7341_config_t dev_cfg = I2C_AS7341_CONFIG_DEFAULT;
    dev_cfg.spectral_gain = AS7341_SPECTRAL_GAIN_512X;
    dev_cfg.atime         = 100;
    dev_cfg.astep         = 999;
    return as7341_init(g_bus, &dev_cfg, &g_sensor);
}

// --- Lux estimation + ZCL encoding -------------------------------------------

// Rough lux estimate from the AS7341 clear channel. Empirical single-point
// calibration: at 512x gain / 281 ms integration, 1 clear count ≈ 8 lux in
// warm-white indoor light. TODO: replace with a proper photopic-weighted
// sum of F2..F8 and a reference-meter calibration.
static float clear_counts_to_lux(uint16_t clear_counts) {
    const float LUX_PER_COUNT = 8.0f;
    return (float)clear_counts * LUX_PER_COUNT;
}

// ZCL 0x0400 MeasuredValue = 10000 * log10(lux) + 1 (spec §4.2 Illum Meas).
// 0x0000 means "too low", 0xFFFF means "invalid".
static uint16_t lux_to_zcl_measured_value(float lux) {
    if (lux <= 1.0f) return 0;
    float mv = 10000.0f * log10f(lux) + 1.0f;
    if (mv < 1.0f)      return 0;
    if (mv > 0xFFFEu)   return 0xFFFEu;
    return (uint16_t)mv;
}

// --- Zigbee plumbing ---------------------------------------------------------

#define LIGHT_SENSOR_ENDPOINT      1
#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

static void start_network_steering(uint8_t param) {
    (void)param;
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p     = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Zigbee stack initialized");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Joining a Zigbee network (factory new, starting steering)...");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Rejoined existing network: short=0x%04x PAN=0x%04x ch=%d",
                         esp_zb_get_short_address(),
                         esp_zb_get_pan_id(),
                         esp_zb_get_current_channel());
            }
        } else {
            ESP_LOGW(TAG, "Commissioning init failed (%s), retrying in 1s",
                     esp_err_to_name(err_status));
            esp_zb_scheduler_alarm(start_network_steering, 0, 1000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Joined network: PAN=0x%04x ch=%d short=0x%04x",
                     esp_zb_get_pan_id(),
                     esp_zb_get_current_channel(),
                     esp_zb_get_short_address());
        } else {
            ESP_LOGW(TAG, "Steering failed (%s), retrying in 5s", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm(start_network_steering, 0, 5000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type,
                 esp_err_to_name(err_status));
        break;
    }
}

static void esp_zb_task(void *pvParameters) {
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role         = ESP_ZB_DEVICE_TYPE_ED,
        .install_code_policy = false,
        .nwk_cfg.zed_cfg = {
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN,
            .keep_alive = 3000,
        },
    };
    esp_zb_init(&zb_nwk_cfg);

    // Note: esp-zigbee-sdk calls this a "light sensor" but the cluster list
    // it builds is Basic + Identify + Illuminance Measurement — exactly the
    // HA 0x0106 "Light Sensor" / ZHA Illuminance sensor profile.
    esp_zb_light_sensor_cfg_t sensor_cfg = ESP_ZB_DEFAULT_LIGHT_SENSOR_CONFIG();
    esp_zb_ep_list_t *ep_list =
        esp_zb_light_sensor_ep_create(LIGHT_SENSOR_ENDPOINT, &sensor_cfg);
    esp_zb_device_register(ep_list);

    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

// --- Sensor loop --------------------------------------------------------------

static void sensor_task(void *pvParameters) {
    printf("ts_ms,F1_415,F2_445,F3_480,F4_515,F5_555,F6_590,F7_630,F8_680,clear,nir,lux_est,zcl_mv\n");
    while (1) {
        as7341_channels_spectral_data_t d;
        esp_err_t r = as7341_get_spectral_measurements(g_sensor, &d);
        if (r == ESP_OK) {
            float lux        = clear_counts_to_lux(d.clear);
            uint16_t zcl_mv  = lux_to_zcl_measured_value(lux);

            printf("%" PRId64 ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.1f,%u\n",
                   esp_timer_get_time() / 1000,
                   d.f1, d.f2, d.f3, d.f4, d.f5, d.f6, d.f7, d.f8,
                   d.clear, d.nir, lux, zcl_mv);

            esp_zb_lock_acquire(portMAX_DELAY);
            esp_zb_zcl_set_attribute_val(
                LIGHT_SENSOR_ENDPOINT,
                ESP_ZB_ZCL_CLUSTER_ID_ILLUMINANCE_MEASUREMENT,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_ILLUMINANCE_MEASUREMENT_MEASURED_VALUE_ID,
                &zcl_mv,
                false);
            esp_zb_lock_release();
        } else {
            ESP_LOGW(TAG, "AS7341 read failed: %s", esp_err_to_name(r));
        }
        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}

// --- Boot --------------------------------------------------------------------

void app_main(void) {
    ESP_LOGI(TAG, "lightmeter boot — ESP-IDF + Zigbee end device");

    ESP_ERROR_CHECK(nvs_flash_init());

    esp_zb_platform_config_t config = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    if (sensor_bringup() != ESP_OK) {
        ESP_LOGE(TAG, "AS7341 bring-up failed — will still start Zigbee so the device is pairable");
    }

    xTaskCreate(esp_zb_task,  "zb_main",     4096, NULL, 5, NULL);
    if (g_sensor) {
        xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 4, NULL);
    }
}
