#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "as7341.h"

static const char *TAG = "lightmeter";

#define AS7341_I2C_ADDR 0x39
#define I2C_PROBE_TIMEOUT_MS 50

// Candidate SDA/SCL pairs in priority order. The firmware probes each
// pair in both polarities until the AS7341 ACKs at 0x39. Covers:
//   12/22 - Arduino-ESP32 default for esp32-h2-devkitm-1
//    4/5  - other commonly wired pair on H2 breakouts
//    1/0  - what the first version of this firmware guessed
//   10/11 - header-adjacent pair on the DevKitM-1
//    2/3  - header-adjacent pair on the DevKitM-1
// Avoiding: 8/9 (strapping), 23/24 (UART0 console), 25/26/27 (USB pins).
typedef struct { gpio_num_t a; gpio_num_t b; } pin_pair_t;
static const pin_pair_t candidate_pairs[] = {
    { GPIO_NUM_12, GPIO_NUM_22 },
    { GPIO_NUM_4,  GPIO_NUM_5  },
    { GPIO_NUM_1,  GPIO_NUM_0  },
    { GPIO_NUM_10, GPIO_NUM_11 },
    { GPIO_NUM_2,  GPIO_NUM_3  },
};

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
    if (found) {
        *bus_out = bus;
        return true;
    }
    i2c_del_master_bus(bus);
    return false;
}

void app_main(void) {
    ESP_LOGI(TAG, "lightmeter boot — scanning for AS7341");

    i2c_master_bus_handle_t bus = NULL;
    gpio_num_t sda = -1, scl = -1;

    for (size_t i = 0; i < sizeof(candidate_pairs) / sizeof(candidate_pairs[0]); i++) {
        pin_pair_t p = candidate_pairs[i];
        if (try_pair(p.a, p.b, &bus)) { sda = p.a; scl = p.b; break; }
        if (try_pair(p.b, p.a, &bus)) { sda = p.b; scl = p.a; break; }
    }

    if (bus == NULL) {
        ESP_LOGE(TAG, "AS7341 not found on any candidate pin pair — check wiring/power");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "AS7341 at 0x%02x on SDA=%d SCL=%d", AS7341_I2C_ADDR, sda, scl);

    as7341_config_t dev_cfg = I2C_AS7341_CONFIG_DEFAULT;
    // Crank up for ambient indoor light. Integration = (atime+1)*(astep+1)*2.78us
    // 101 * 1000 * 2.78us = ~281 ms per read, and 512x analog gain.
    dev_cfg.spectral_gain = AS7341_SPECTRAL_GAIN_512X;
    dev_cfg.atime         = 100;
    dev_cfg.astep         = 999;
    as7341_handle_t sensor = NULL;
    ESP_ERROR_CHECK(as7341_init(bus, &dev_cfg, &sensor));
    ESP_LOGI(TAG, "gain=512x atime=%d astep=%d", dev_cfg.atime, dev_cfg.astep);

    printf("ts_ms,F1_415,F2_445,F3_480,F4_515,F5_555,F6_590,F7_630,F8_680,clear,nir\n");

    while (1) {
        as7341_channels_spectral_data_t d;
        esp_err_t r = as7341_get_spectral_measurements(sensor, &d);
        if (r == ESP_OK) {
            printf("%" PRId64 ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                   esp_timer_get_time() / 1000,
                   d.f1, d.f2, d.f3, d.f4, d.f5, d.f6, d.f7, d.f8,
                   d.clear, d.nir);
        } else {
            ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(r));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
