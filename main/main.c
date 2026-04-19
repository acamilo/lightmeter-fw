#include <math.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

#include "as7341.h"

// --- OTA parameters ---------------------------------------------------------
//
// These three values must match what the OTA image header advertises.
// Zigbee Alliance assigns manufacturer codes to commercial vendors — we're
// a hobby project, so we pick a fixed value in the "unused" space and stick
// with it. Changing any of these after devices are deployed orphans them
// from future updates.
#define LIGHTMETER_MANUFACTURER  0x1289
#define LIGHTMETER_IMAGE_TYPE    0x0001
#define LIGHTMETER_FW_VERSION    0x00000003   // bump for each release

// Shown in ZHA's "Manage Device" view and used by the matching quirk. These
// are ZCL character strings so they get the usual length-byte prefix in RAM;
// keep them short enough that the packed form fits the 32-byte attr limit.
#define BASIC_MANUFACTURER_NAME  "Espressif"
#define BASIC_MODEL_IDENTIFIER   "lightmeter"

static void mark_image_valid_once(void);
static void pack_zcl_string(const char *in, uint8_t *out, size_t out_capacity);

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

// --- Channel table -----------------------------------------------------------

#define NUM_CHANNELS   13
#define ENDPOINT_BASE  1   // EP IDs run 1..NUM_CHANNELS

typedef enum {
    CH_TYPE_ANALOG,    // Analog Input cluster, float PresentValue
    CH_TYPE_BINARY,    // Binary Input cluster, bool PresentValue
} channel_type_t;

typedef enum {
    CH_MODE_BAND_PPFD,     // per-band PPFD from F1..F8 (visible PAR)
    CH_MODE_NIR_PPFD,      // same formula applied to the NIR channel
    CH_MODE_PAR_TOTAL,     // sum of F1..F8 PPFD
    CH_MODE_LUX,           // photopic-weighted illuminance
    CH_MODE_FLICKER,       // 100 Hz / 120 Hz mains flicker detected
    CH_MODE_SATURATED,     // any spectral channel saturated last read
} channel_mode_t;

typedef struct {
    channel_type_t type;
    channel_mode_t mode;
    uint16_t       wavelength_nm;   // 0 if not applicable
    int            band_idx;        // 0..7 for F1..F8, -1 otherwise
    const char    *description;
} channel_spec_t;

static const channel_spec_t channels[NUM_CHANNELS] = {
    { CH_TYPE_ANALOG, CH_MODE_BAND_PPFD, 415, 0, "F1 415nm PPFD umol/m2/s" },
    { CH_TYPE_ANALOG, CH_MODE_BAND_PPFD, 445, 1, "F2 445nm PPFD umol/m2/s" },
    { CH_TYPE_ANALOG, CH_MODE_BAND_PPFD, 480, 2, "F3 480nm PPFD umol/m2/s" },
    { CH_TYPE_ANALOG, CH_MODE_BAND_PPFD, 515, 3, "F4 515nm PPFD umol/m2/s" },
    { CH_TYPE_ANALOG, CH_MODE_BAND_PPFD, 555, 4, "F5 555nm PPFD umol/m2/s" },
    { CH_TYPE_ANALOG, CH_MODE_BAND_PPFD, 590, 5, "F6 590nm PPFD umol/m2/s" },
    { CH_TYPE_ANALOG, CH_MODE_BAND_PPFD, 630, 6, "F7 630nm PPFD umol/m2/s" },
    { CH_TYPE_ANALOG, CH_MODE_BAND_PPFD, 680, 7, "F8 680nm PPFD umol/m2/s" },
    { CH_TYPE_ANALOG, CH_MODE_PAR_TOTAL,   0, -1, "PAR total PPFD umol/m2/s" },
    { CH_TYPE_ANALOG, CH_MODE_LUX,         0, -1, "Illuminance lux photopic" },
    { CH_TYPE_ANALOG, CH_MODE_NIR_PPFD,   910, -1, "NIR 910nm PFD umol/m2/s" },
    { CH_TYPE_BINARY, CH_MODE_FLICKER,     0, -1, "Flicker 100/120Hz detected" },
    { CH_TYPE_BINARY, CH_MODE_SATURATED,   0, -1, "Spectral channel saturated" },
};

// Per-band responsivity in the k0i05 "basic counts" domain (counts / (µW/cm²)).
// Datasheet-typical values for F1..F8 + a plausible NIR figure. Expect factor
// of ~2 accuracy until single-point calibrated against a reference meter.
static const float responsivity_basic_f1_f8[8] = {
    1.016f, 1.078f, 1.150f, 1.094f, 1.011f, 1.038f, 1.167f, 1.166f,
};
static const float responsivity_basic_nir = 1.10f;

// CIE 1931 photopic V(λ) at the AS7341 band centers.
static const float photopic_weight[8] = {
    0.00158f, 0.0355f, 0.139f, 0.608f,
    1.000f,   0.757f,  0.265f, 0.0170f,
};

// --- Photometric math --------------------------------------------------------

static float ppfd_from_basic(float basic, float responsivity, uint16_t lambda_nm) {
    float irradiance_uW_cm2 = basic / responsivity;
    float irradiance_W_m2   = irradiance_uW_cm2 * 0.01f;
    return irradiance_W_m2 * (float)lambda_nm / 119.6f;
}

static float compute_lux(const float basic_f1_f8[8]) {
    float sum = 0.0f;
    for (int i = 0; i < 8; i++) {
        float irr_W_m2 = (basic_f1_f8[i] / responsivity_basic_f1_f8[i]) * 0.01f;
        sum += photopic_weight[i] * irr_W_m2;
    }
    return 683.0f * sum;
}

// --- Zigbee plumbing ---------------------------------------------------------

#define ESP_ZB_PRIMARY_CHANNEL_MASK ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK
#define ENG_UNITS_NO_UNITS          95  // BACnet "no_units" — closest to µmol/m²/s

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
                ESP_LOGI(TAG, "Rejoined network: PAN=0x%04x ch=%d short=0x%04x",
                         esp_zb_get_pan_id(), esp_zb_get_current_channel(),
                         esp_zb_get_short_address());
                mark_image_valid_once();
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
            mark_image_valid_once();
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

// --- OTA client ---------------------------------------------------------------

static const esp_partition_t *s_ota_partition = NULL;
static esp_ota_handle_t       s_ota_handle    = 0;
static uint32_t               s_ota_written   = 0;
static bool                   s_running_image_validated = false;

static void add_ota_cluster_to(esp_zb_cluster_list_t *cluster_list) {
    esp_zb_ota_cluster_cfg_t ota_cfg = {
        .ota_upgrade_file_version         = LIGHTMETER_FW_VERSION,
        .ota_upgrade_manufacturer         = LIGHTMETER_MANUFACTURER,
        .ota_upgrade_image_type           = LIGHTMETER_IMAGE_TYPE,
        .ota_min_block_reque              = 0,
        .ota_upgrade_file_offset          = ESP_ZB_ZCL_OTA_UPGRADE_FILE_OFFSET_DEF_VALUE,
        .ota_upgrade_downloaded_file_ver  = ESP_ZB_ZCL_OTA_UPGRADE_FILE_VERSION_DEF_VALUE,
        .ota_upgrade_server_id            = { 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff },
        .ota_image_upgrade_status         = ESP_ZB_ZCL_OTA_UPGRADE_IMAGE_STATUS_DEF_VALUE,
    };
    esp_zb_cluster_list_add_ota_cluster(
        cluster_list, esp_zb_ota_cluster_create(&ota_cfg),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
}

// Called by the Zigbee stack on every OTA upgrade-state transition.
// Flow matches the esp-zigbee-sdk OTA example: open partition on START,
// stream chunks on RECEIVE, finalize + swap partitions + reboot on FINISH.
static esp_err_t zb_core_action_handler(esp_zb_core_action_callback_id_t callback_id,
                                        const void *message) {
    if (callback_id != ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID) {
        return ESP_OK;
    }
    const esp_zb_zcl_ota_upgrade_value_message_t *m = message;
    switch (m->upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        s_ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!s_ota_partition) {
            ESP_LOGE(TAG, "OTA: no next update partition");
            return ESP_FAIL;
        }
        if (esp_ota_begin(s_ota_partition, OTA_SIZE_UNKNOWN, &s_ota_handle) != ESP_OK) {
            ESP_LOGE(TAG, "OTA: esp_ota_begin failed");
            return ESP_FAIL;
        }
        s_ota_written = 0;
        ESP_LOGI(TAG, "OTA: starting download to partition %s", s_ota_partition->label);
        break;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE:
        if (esp_ota_write(s_ota_handle, m->payload, m->payload_size) != ESP_OK) {
            ESP_LOGE(TAG, "OTA: write failed at offset %" PRIu32, s_ota_written);
            return ESP_FAIL;
        }
        s_ota_written += m->payload_size;
        if ((s_ota_written & 0x3FFF) < m->payload_size) {   // log every ~16 KB
            ESP_LOGI(TAG, "OTA: received %" PRIu32 " bytes", s_ota_written);
        }
        break;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
        ESP_LOGI(TAG, "OTA: apply request accepted");
        break;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        if (esp_ota_end(s_ota_handle) != ESP_OK) {
            ESP_LOGE(TAG, "OTA: esp_ota_end failed (image invalid?)");
            return ESP_FAIL;
        }
        if (esp_ota_set_boot_partition(s_ota_partition) != ESP_OK) {
            ESP_LOGE(TAG, "OTA: set_boot_partition failed");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "OTA: %" PRIu32 " bytes written, rebooting into new image", s_ota_written);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        break;
    default:
        ESP_LOGI(TAG, "OTA: status 0x%04x", m->upgrade_status);
        break;
    }
    return ESP_OK;
}

// Once we're up and joined, mark the current image valid so the bootloader
// stops holding it in the rollback-pending state. Called from the signal
// handler after a successful steering or rejoin.
static void mark_image_valid_once(void) {
    if (s_running_image_validated) return;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "OTA: marked running image valid, rollback cancelled");
        }
    }
    s_running_image_validated = true;
}

// Add Basic + Identify to a cluster list. All our endpoints want both.
static void add_basic_and_identify(esp_zb_cluster_list_t *cluster_list) {
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *basic_attrs = esp_zb_basic_cluster_create(&basic_cfg);

    // Manufacturer + Model give the ZHA quirk a stable thing to match on.
    // Without these, the Basic cluster reports blank strings and our quirk
    // has nothing reliable to hook into, so ZHA falls back to its (flaky
    // for multi-endpoint devices) generic AnalogInput discovery path.
    uint8_t manuf_buf[1 + sizeof(BASIC_MANUFACTURER_NAME)];
    uint8_t model_buf[1 + sizeof(BASIC_MODEL_IDENTIFIER)];
    pack_zcl_string(BASIC_MANUFACTURER_NAME, manuf_buf, sizeof(manuf_buf));
    pack_zcl_string(BASIC_MODEL_IDENTIFIER,  model_buf, sizeof(model_buf));
    esp_zb_basic_cluster_add_attr(basic_attrs,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manuf_buf);
    esp_zb_basic_cluster_add_attr(basic_attrs,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model_buf);

    esp_zb_cluster_list_add_basic_cluster(
        cluster_list, basic_attrs,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    esp_zb_cluster_list_add_identify_cluster(
        cluster_list, esp_zb_identify_cluster_create(&identify_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

// Pack a plain C string into the ZCL character_string format (length-prefix).
static void pack_zcl_string(const char *in, uint8_t *out, size_t out_capacity) {
    size_t n = strlen(in);
    if (n > out_capacity - 1) n = out_capacity - 1;
    out[0] = (uint8_t)n;
    memcpy(&out[1], in, n);
}

static void add_analog_endpoint(esp_zb_ep_list_t *ep_list, uint8_t ep_id,
                                const char *description_str) {
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    add_basic_and_identify(cluster_list);

    esp_zb_analog_input_cluster_cfg_t ai_cfg = {
        .out_of_service = false, .present_value = 0.0f, .status_flags = 0,
    };
    esp_zb_attribute_list_t *ai_attrs = esp_zb_analog_input_cluster_create(&ai_cfg);

    uint8_t desc_buf[33];
    pack_zcl_string(description_str, desc_buf, sizeof(desc_buf));
    esp_zb_analog_input_cluster_add_attr(
        ai_attrs, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID, desc_buf);

    uint16_t eng_units = ENG_UNITS_NO_UNITS;
    esp_zb_analog_input_cluster_add_attr(
        ai_attrs, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_ENGINEERING_UNITS_ID, &eng_units);

    esp_zb_cluster_list_add_analog_input_cluster(
        cluster_list, ai_attrs, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = ep_id,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg);
}

static void add_binary_endpoint(esp_zb_ep_list_t *ep_list, uint8_t ep_id,
                                const char *description_str) {
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    add_basic_and_identify(cluster_list);

    esp_zb_binary_input_cluster_cfg_t bi_cfg = {
        .out_of_service = false, .present_value = false, .status_flags = 0,
    };
    esp_zb_attribute_list_t *bi_attrs = esp_zb_binary_input_cluster_create(&bi_cfg);

    uint8_t desc_buf[33];
    pack_zcl_string(description_str, desc_buf, sizeof(desc_buf));
    esp_zb_cluster_add_attr(
        bi_attrs, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
        ESP_ZB_ZCL_ATTR_BINARY_INPUT_DESCRIPTION_ID,
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, desc_buf);

    esp_zb_cluster_list_add_binary_input_cluster(
        cluster_list, bi_attrs, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = ep_id,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg);
}

// See the long comment on the previous revision: coordinators usually override
// these with their own ConfigureReport bindings, but they're good fallbacks
// and get used by stacks that don't bind aggressively (z2m sometimes).
static void configure_reporting(void) {
    for (int i = 0; i < NUM_CHANNELS; i++) {
        esp_zb_zcl_reporting_info_t info = {
            .direction    = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
            .ep           = (uint8_t)(ENDPOINT_BASE + i),
            .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
            .attr_id      = 0x0055,   // PresentValue is 0x0055 on both AI and BI
            .flags        = 0,
            .run_time     = 0,
        };
        info.cluster_id = (channels[i].type == CH_TYPE_ANALOG)
            ? ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT
            : ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT;
        info.u.send_info.min_interval     = 2;
        info.u.send_info.max_interval     = 60;
        info.u.send_info.def_min_interval = 2;
        info.u.send_info.def_max_interval = 60;
        if (channels[i].type == CH_TYPE_ANALOG) {
            info.u.send_info.delta.f32 = 0.1f;
        } else {
            info.u.send_info.delta.u8 = 1;   // any bool transition counts
        }
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
        uint8_t ep_id = (uint8_t)(ENDPOINT_BASE + i);
        if (channels[i].type == CH_TYPE_ANALOG) {
            add_analog_endpoint(ep_list, ep_id, channels[i].description);
        } else {
            add_binary_endpoint(ep_list, ep_id, channels[i].description);
        }
    }

    // Add OTA Upgrade (0x0019) client cluster to EP 1 so the coordinator can
    // push firmware updates. Convention: OTA sits on the first endpoint.
    {
        esp_zb_endpoint_config_t ep1_cfg;  // only consulted for placement lookup
        (void)ep1_cfg;
        esp_zb_cluster_list_t *ep1_clusters =
            esp_zb_ep_list_get_ep(ep_list, ENDPOINT_BASE);
        if (ep1_clusters) add_ota_cluster_to(ep1_clusters);
    }

    esp_zb_core_action_handler_register(zb_core_action_handler);
    esp_zb_device_register(ep_list);
    configure_reporting();

    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

// --- Sensor loop --------------------------------------------------------------

// Updates the local attribute value AND triggers an immediate ZCL Report
// Attributes message toward bound destinations. ZHA's ConfigureReport default
// for AnalogInput is 30/900/1 (min/max/delta), which makes values trickle
// in. By spontaneously reporting on every sensor cycle we bypass that and
// the coordinator sees updates at our sensor_task cadence (~0.5 Hz).
static void push_and_report(uint8_t ep_id, uint16_t cluster_id, uint16_t attr_id, void *value) {
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(
        ep_id, cluster_id, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr_id, value, false);

    // Address explicitly to the coordinator on EP 1. Using addr_mode=0 (use
    // binding table) means reports only flow if ZHA actually bound this
    // specific (endpoint, cluster). In practice ZHA binds BinaryInput on
    // EP 12/13 but not always AnalogInput on EP 1..11, which silently
    // drops those reports. Explicit addressing bypasses that.
    esp_zb_zcl_report_attr_cmd_t report = {
        .zcl_basic_cmd = {
            .src_endpoint = ep_id,
            .dst_endpoint = 1,
            .dst_addr_u   = { .addr_short = 0x0000 },
        },
        .address_mode     = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID        = cluster_id,
        .direction        = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .manuf_code       = 0,
        .attributeID      = attr_id,
    };
    esp_err_t rc = esp_zb_zcl_report_attr_cmd_req(&report);
    esp_zb_lock_release();
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "report_attr_cmd_req ep=%u cluster=0x%04x attr=0x%04x rc=%s",
                 ep_id, cluster_id, attr_id, esp_err_to_name(rc));
    }
}

static void push_analog(uint8_t ep_id, float value) {
    push_and_report(ep_id, ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
                    ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, &value);
}

static void push_binary(uint8_t ep_id, bool value) {
    push_and_report(ep_id, ESP_ZB_ZCL_CLUSTER_ID_BINARY_INPUT,
                    ESP_ZB_ZCL_ATTR_BINARY_INPUT_PRESENT_VALUE_ID, &value);
}

static void sensor_task(void *pvParameters) {
    printf("ts_ms,");
    for (int i = 0; i < NUM_CHANNELS; i++) {
        printf("%s%s", channels[i].description, i == NUM_CHANNELS - 1 ? "\n" : ",");
    }

    while (1) {
        as7341_channels_spectral_data_t raw;
        if (as7341_get_spectral_measurements(g_sensor, &raw) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
            continue;
        }
        as7341_channels_basic_counts_data_t basic;
        if (as7341_get_basic_counts(g_sensor, raw, &basic) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
            continue;
        }

        float basic_f1_f8[8] = {
            basic.f1, basic.f2, basic.f3, basic.f4,
            basic.f5, basic.f6, basic.f7, basic.f8,
        };

        // Flicker detection status — only treat the explicit 100/120 Hz
        // verdicts as "yes"; UNKNOWN/INVALID mean the engine hasn't latched
        // onto a frequency yet, not that flicker is absent.
        as7341_flicker_detection_states_t flicker_state = AS7341_FLICKER_DETECTION_INVALID;
        (void)as7341_get_flicker_detection_status(g_sensor, &flicker_state);
        bool flicker_detected = (flicker_state == AS7341_FLICKER_DETECTION_100HZ) ||
                                (flicker_state == AS7341_FLICKER_DETECTION_120HZ);

        // Saturation — any kind of rail-out on the spectral or flicker path.
        as7341_status2_register_t st2 = { .reg = 0 };
        (void)as7341_get_status2_register(g_sensor, &st2);
        bool saturated = st2.bits.digital_saturation ||
                         st2.bits.analog_saturation ||
                         st2.bits.flicker_detect_digital_saturation ||
                         st2.bits.flicker_detect_analog_saturation;

        // Analog values by channel.
        float values[NUM_CHANNELS];
        float par_total = 0.0f;
        for (int i = 0; i < 8; i++) {
            float v = ppfd_from_basic(basic_f1_f8[i], responsivity_basic_f1_f8[i],
                                      channels[i].wavelength_nm);
            values[i] = v;
            par_total += v;
        }
        values[8]  = par_total;
        values[9]  = compute_lux(basic_f1_f8);
        values[10] = ppfd_from_basic(basic.nir, responsivity_basic_nir,
                                     channels[10].wavelength_nm);

        // CSV debug line
        printf("%" PRId64, esp_timer_get_time() / 1000);
        for (int i = 0; i < 11; i++) printf(",%.3f", values[i]);
        printf(",%d,%d\n", flicker_detected, saturated);

        // Push to Zigbee
        for (int i = 0; i < 11; i++) {
            push_analog((uint8_t)(ENDPOINT_BASE + i), values[i]);
        }
        push_binary((uint8_t)(ENDPOINT_BASE + 11), flicker_detected);
        push_binary((uint8_t)(ENDPOINT_BASE + 12), saturated);

        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}

// --- Boot --------------------------------------------------------------------

void app_main(void) {
    ESP_LOGI(TAG, "lightmeter boot — %d endpoints (11 analog + 2 binary) + OTA, fw=0x%08x",
             NUM_CHANNELS, (unsigned)LIGHTMETER_FW_VERSION);
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) ESP_LOGI(TAG, "built %s %s, idf=%s", app->date, app->time, app->idf_ver);

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
