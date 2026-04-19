#include <math.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

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
#define READ_INTERVAL_MS        2000

typedef struct { gpio_num_t a; gpio_num_t b; } pin_pair_t;

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

// --- Channel table & photometric math ---------------------------------------

#define NUM_CHANNELS 10
#define ENDPOINT_BASE 1   // EP IDs run 1..NUM_CHANNELS

typedef enum {
    CH_MODE_BAND_PPFD,   // PPFD contribution of a single F1..F8 band
    CH_MODE_PAR_TOTAL,   // sum of F1..F8 PPFD
    CH_MODE_LUX,         // photopic-weighted illuminance
} channel_mode_t;

typedef struct {
    uint16_t       wavelength_nm;   // 0 for aggregate channels
    const char    *description;     // shown in ZCL Description attr
    channel_mode_t mode;
} channel_spec_t;

// Ordered by endpoint ID (EP 1 = index 0).
static const channel_spec_t channels[NUM_CHANNELS] = {
    { 415, "F1 415nm PPFD umol/m2/s", CH_MODE_BAND_PPFD },
    { 445, "F2 445nm PPFD umol/m2/s", CH_MODE_BAND_PPFD },
    { 480, "F3 480nm PPFD umol/m2/s", CH_MODE_BAND_PPFD },
    { 515, "F4 515nm PPFD umol/m2/s", CH_MODE_BAND_PPFD },
    { 555, "F5 555nm PPFD umol/m2/s", CH_MODE_BAND_PPFD },
    { 590, "F6 590nm PPFD umol/m2/s", CH_MODE_BAND_PPFD },
    { 630, "F7 630nm PPFD umol/m2/s", CH_MODE_BAND_PPFD },
    { 680, "F8 680nm PPFD umol/m2/s", CH_MODE_BAND_PPFD },
    {   0, "PAR total PPFD umol/m2/s", CH_MODE_PAR_TOTAL },
    {   0, "Illuminance lux photopic", CH_MODE_LUX },
};

// AS7341 datasheet-typical responsivity in counts per (uW/cm^2), measured at
// 128x gain / 50ms integration and re-normalized into the "basic counts"
// domain used by the k0i05 driver. TODO: calibrate each band against a
// reference meter; the shipped numbers are within a factor of ~2 at best.
static const float responsivity_basic[8] = {
    1.016f,  // F1 415nm
    1.078f,  // F2 445nm
    1.150f,  // F3 480nm
    1.094f,  // F4 515nm
    1.011f,  // F5 555nm
    1.038f,  // F6 590nm
    1.167f,  // F7 630nm
    1.166f,  // F8 680nm
};

// CIE 1931 photopic luminosity function V(lambda) sampled at our band centers.
static const float photopic_weight[8] = {
    0.00158f, 0.0355f, 0.139f, 0.608f,
    1.000f,   0.757f,  0.265f, 0.0170f,
};

// Converts AS7341 "basic counts" on a band to PPFD (umol/m^2/s).
//   basic_counts / responsivity  =>  irradiance in uW/cm^2
//   uW/cm^2 * 0.01               =>  W/m^2
//   W/m^2  * lambda_nm / 119.6   =>  umol/m^2/s   (planck-relation per-band)
static float band_to_ppfd(float basic_counts, int band_idx) {
    float irradiance_uw_per_cm2 = basic_counts / responsivity_basic[band_idx];
    float irradiance_w_per_m2   = irradiance_uw_per_cm2 * 0.01f;
    return irradiance_w_per_m2 * (float)channels[band_idx].wavelength_nm / 119.6f;
}

// Photopic-weighted lux estimate from F1..F8.
//   lux = 683 * sum_i( V(lambda_i) * irradiance_i(W/m^2) )
// Approximation: treats each AS7341 band as a delta at its center wavelength.
// Real lux would integrate over the band shapes — fine for horticulture-grade
// cross-checking against the PPFD channels, not for photometry certification.
static float compute_lux(const float basic[8]) {
    float sum = 0.0f;
    for (int i = 0; i < 8; i++) {
        float irr_w_m2 = (basic[i] / responsivity_basic[i]) * 0.01f;
        sum += photopic_weight[i] * irr_w_m2;
    }
    return 683.0f * sum;
}

// --- Zigbee plumbing ---------------------------------------------------------

#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

// BACnet engineering-units enum value for "no_units" / generic. Zigbee's
// fixed enum has no PPFD or lux-as-first-class; ZHA users typically
// override the display unit on the HA entity anyway. The Description
// attribute carries the real unit string.
#define ENG_UNITS_NO_UNITS 95

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
                ESP_LOGI(TAG, "Joining a Zigbee network (factory new, starting steering)");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Rejoined existing network: PAN=0x%04x ch=%d short=0x%04x",
                         esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                         esp_zb_get_short_address());
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
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(),
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

// Build one endpoint: Basic + Identify + Analog Input (with Description +
// EngineeringUnits). Each endpoint is presented as a "Simple Sensor" device.
static void add_analog_endpoint(esp_zb_ep_list_t *ep_list, uint8_t ep_id,
                                const char *description_str) {
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_cluster_list_add_basic_cluster(
        cluster_list,
        esp_zb_basic_cluster_create(&basic_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_cluster_list_add_identify_cluster(
        cluster_list,
        esp_zb_identify_cluster_create(&identify_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_analog_input_cluster_cfg_t ai_cfg = {
        .out_of_service = false,
        .present_value  = 0.0f,
        .status_flags   = 0,
    };
    esp_zb_attribute_list_t *ai_attrs = esp_zb_analog_input_cluster_create(&ai_cfg);

    // Description attribute is a ZCL character_string: [length byte][bytes...].
    size_t desc_len = strlen(description_str);
    if (desc_len > 32) desc_len = 32;  // Description attr spec caps at 32 chars
    uint8_t desc_buf[33];
    desc_buf[0] = (uint8_t)desc_len;
    memcpy(&desc_buf[1], description_str, desc_len);
    esp_zb_analog_input_cluster_add_attr(
        ai_attrs, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID, desc_buf);

    uint16_t eng_units = ENG_UNITS_NO_UNITS;
    esp_zb_analog_input_cluster_add_attr(
        ai_attrs, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_ENGINEERING_UNITS_ID, &eng_units);

    esp_zb_cluster_list_add_analog_input_cluster(
        cluster_list, ai_attrs, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint           = ep_id,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg);
}

// Configure attribute reporting so the coordinator (ZHA) gets pushed updates
// instead of having to poll. Per ZCL: report no faster than min_interval, no
// slower than max_interval, and always if PresentValue drifts by >= delta.
//   min_interval  = 2 s   (matches sensor_task cadence)
//   max_interval  = 60 s  (heartbeat so HA knows we're alive)
//   delta         = 0.1   (0.1 µmol/m²/s for PPFD bands; 0.1 lux for EP 10)
// Re-reporting is cheap and the radio duty cycle is already tiny.
static void configure_reporting(void) {
    for (int i = 0; i < NUM_CHANNELS; i++) {
        esp_zb_zcl_reporting_info_t info = {
            .direction    = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
            .ep           = (uint8_t)(ENDPOINT_BASE + i),
            .cluster_id   = ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
            .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            .attr_id      = ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
            .flags        = 0,
            .run_time     = 0,
        };
        info.u.send_info.min_interval     = 2;
        info.u.send_info.max_interval     = 60;
        info.u.send_info.def_min_interval = 2;
        info.u.send_info.def_max_interval = 60;
        info.u.send_info.delta.f32        = 0.1f;
        esp_zb_zcl_update_reporting_info(&info);
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

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    for (int i = 0; i < NUM_CHANNELS; i++) {
        add_analog_endpoint(ep_list, (uint8_t)(ENDPOINT_BASE + i), channels[i].description);
    }
    esp_zb_device_register(ep_list);
    configure_reporting();

    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

// --- Sensor loop --------------------------------------------------------------

static void push_present_value(uint8_t ep_id, float value) {
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(
        ep_id,
        ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID,
        &value,
        false);
    esp_zb_lock_release();
}

static void sensor_task(void *pvParameters) {
    printf("ts_ms,");
    for (int i = 0; i < NUM_CHANNELS; i++) printf("%s%s", channels[i].description, i == NUM_CHANNELS - 1 ? "\n" : ",");

    while (1) {
        as7341_channels_spectral_data_t raw;
        esp_err_t r = as7341_get_spectral_measurements(g_sensor, &raw);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "AS7341 read failed: %s", esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
            continue;
        }

        as7341_channels_basic_counts_data_t basic;
        r = as7341_get_basic_counts(g_sensor, raw, &basic);
        if (r != ESP_OK) {
            ESP_LOGW(TAG, "basic_counts conversion failed: %s", esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
            continue;
        }

        float basic_by_band[8] = {
            basic.f1, basic.f2, basic.f3, basic.f4,
            basic.f5, basic.f6, basic.f7, basic.f8,
        };

        float values[NUM_CHANNELS];
        float par_total = 0.0f;
        for (int i = 0; i < 8; i++) {
            values[i] = band_to_ppfd(basic_by_band[i], i);
            par_total += values[i];
        }
        values[8] = par_total;
        values[9] = compute_lux(basic_by_band);

        printf("%" PRId64, esp_timer_get_time() / 1000);
        for (int i = 0; i < NUM_CHANNELS; i++) printf(",%.3f", values[i]);
        printf("\n");

        for (int i = 0; i < NUM_CHANNELS; i++) {
            push_present_value((uint8_t)(ENDPOINT_BASE + i), values[i]);
        }

        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}

// --- Boot --------------------------------------------------------------------

void app_main(void) {
    ESP_LOGI(TAG, "lightmeter boot — %d-endpoint Analog Input, ZHA-compatible", NUM_CHANNELS);

    ESP_ERROR_CHECK(nvs_flash_init());

    esp_zb_platform_config_t config = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    if (sensor_bringup() != ESP_OK) {
        ESP_LOGE(TAG, "AS7341 bring-up failed — Zigbee will still start so the device is pairable");
    }

    xTaskCreate(esp_zb_task, "zb_main", 4096, NULL, 5, NULL);
    if (g_sensor) {
        xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 4, NULL);
    }
}
