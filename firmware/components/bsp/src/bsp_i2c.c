// components/bsp/src/bsp_i2c.c
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "bsp_i2c";

static i2c_master_bus_handle_t s_bus;

esp_err_t bsp_i2c_init(void) {
    if (s_bus) return ESP_OK;                 // idempotent
    i2c_master_bus_config_t cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&cfg, &s_bus);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus creation failed (%s) - check whether SDA=GPIO%d / SCL=GPIO%d is used by another peripheral",
                 esp_err_to_name(e), BSP_I2C_SDA, BSP_I2C_SCL);
        s_bus = NULL;
        return e;
    }
    ESP_LOGI(TAG, "I2C ready SDA=GPIO%d SCL=GPIO%d", BSP_I2C_SDA, BSP_I2C_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_bus(void) { return s_bus; }

esp_err_t bsp_i2c_scan(void) {
    // WARNING: never open a separate temporary bus to scan in order to "isolate NACKs" - that kills the whole I2C bus:
    //   a second i2c_new_master_bus() on the same port always fails, but in i2c_common.c
    //   the "already acquired" branch still hands the REAL bus's bus object out through *i2c_new_bus;
    //   the err cleanup path i2c_master_bus_destroy() then takes it and calls i2c_common_deinit_pins(),
    //   which unbinds SDA/SCL from the I2C peripheral. After that every transaction hangs at "clear bus failed",
    //   and both the ES8311 and CW2017 are lost.
    //   (Observed on IDF v5.5.3; the symptom is a screen full of "reset hardware failed", very hard to trace back to the scan.)
    //
    // Scanning directly on the real bus is safe: i2c_master_probe() resets status to
    // I2C_STATUS_DONE at the end ("in order not influence next time transaction"), so a NACK leaves no residue;
    // the 100kHz timing it sets temporarily is also overridden by the next device transaction with its own scl_speed_hz.
    if (!s_bus) {
        ESP_LOGE(TAG, "Call bsp_i2c_init() successfully first");
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "I2C scan start:");
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            const char *who = (addr == BSP_I2C_ES8311_ADDR) ? "  <- ES8311 audio codec"
                            : (addr == BSP_I2C_CW2017_ADDR) ? "  <- CW2017 fuel gauge"
                            : "";
            ESP_LOGI(TAG, "  device found @ 0x%02X%s", addr, who);
            found++;
        }
    }
    if (found == 0) ESP_LOGW(TAG, "  no I2C device found - check wiring, pull-up resistors and power");
    else            ESP_LOGI(TAG, "I2C scan done, %d device(s) in total", found);
    return ESP_OK;
}

esp_err_t bsp_i2c_prepare_deep_sleep(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BSP_I2C_SDA) | (1ULL << BSP_I2C_SCL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t e = gpio_config(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "I2C SDA/SCL high-Z configuration failed: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(TAG, "I2C SDA/SCL switched to high-Z; external pull-up power draw is still determined by hardware");
    return ESP_OK;
}
