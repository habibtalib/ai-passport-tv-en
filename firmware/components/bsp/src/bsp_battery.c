// components/bsp/src/bsp_battery.c
// CW2017 fuel gauge driver.
#include "bsp_battery.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static const char *TAG = "bsp_batt";

#define CW_REG_VERSION   0x00   // Version number; an ACK at power-up means the chip is present
#define CW_REG_VCELL_H   0x02   // 14-bit voltage, V(uV) = raw * 312.5
#define CW_REG_SOC_H     0x04   // High byte = integer percent; low byte (0x05) = 1/256 %
#define CW_REG_CONFIG    0x08   // 0xF0=sleep / 0x30=reset state / 0x00=normal
#define CW_REG_SOC_ALERT 0x0B   // bit7=profile UPDATE_FLAG; bit6:0=SOC alert threshold
#define CW_REG_PROFILE   0x10   // 80-byte battery profile start address

#define CW_CONFIG_ACTIVE  0x00
#define CW_CONFIG_RESTART 0x30
#define CW_CONFIG_SLEEP   0xF0
#define CW_UPDATE_FLAG    0x80
#define CW_PROFILE_SIZE   80

static const uint8_t s_battery_profile[CW_PROFILE_SIZE] = {
    0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAD, 0xC7, 0xC8, 0xCA, 0xBD, 0xB1, 0xC1, 0x94,
    0x88, 0xD1, 0xBD, 0x97, 0x88, 0x66, 0x56, 0x4A,
    0x3F, 0x33, 0x26, 0x5C, 0x37, 0xD1, 0x27, 0xD8,
    0xCC, 0xB7, 0xCF, 0xB3, 0xB2, 0xAE, 0xA6, 0x9E,
    0x99, 0x97, 0x9B, 0x86, 0x47, 0x1E, 0x17, 0x26,
    0x49, 0x96, 0xD9, 0xE1, 0xDD, 0xDC, 0xD4, 0x59,
    0x00, 0x00, 0x90, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5C,
};

_Static_assert(sizeof(s_battery_profile) == CW_PROFILE_SIZE,
               "CW2017 battery profile must contain exactly 80 bytes");

static i2c_master_dev_handle_t s_dev;

static esp_err_t cw_read(uint8_t reg, uint8_t *buf, size_t n) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 100);
}

static esp_err_t cw_write(uint8_t reg, uint8_t val) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100);
}

// The low 4 bits of CONFIG are reserved. Reset, sleep and activate all switch with the sequence the chip specifies.
static int cw_enter_sleep(void) {
    if (cw_write(CW_REG_CONFIG, CW_CONFIG_RESTART) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (cw_write(CW_REG_CONFIG, CW_CONFIG_SLEEP) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
}

static int cw_enter_active(void) {
    if (cw_write(CW_REG_CONFIG, CW_CONFIG_RESTART) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(20));
    if (cw_write(CW_REG_CONFIG, CW_CONFIG_ACTIVE) != 0) return -1;
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
}

// Check both UPDATE_FLAG and the 80-byte content, to avoid wrongly using old cell parameters based on the flag alone.
static int cw_profile_matches(bool *matches) {
    uint8_t val = 0;
    *matches = false;

    if (cw_read(CW_REG_SOC_ALERT, &val, 1) != 0) return -1;
    if ((val & CW_UPDATE_FLAG) == 0) return 0;

    for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
        if (cw_read((uint8_t)(CW_REG_PROFILE + i), &val, 1) != 0) return -1;
        if (val != s_battery_profile[i]) return 0;
    }

    *matches = true;
    return 0;
}

// The profile must be written byte by byte in the sleep state; set UPDATE_FLAG after readback verification, then restart the calculation.
static int cw_update_profile(void) {
    uint8_t val = 0;
    if (cw_enter_sleep() != 0) return -1;

    for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
        if (cw_write((uint8_t)(CW_REG_PROFILE + i), s_battery_profile[i]) != 0) {
            ESP_LOGE(TAG, "Profile write failed: index=%u", (unsigned)i);
            return -1;
        }
    }

    for (size_t i = 0; i < CW_PROFILE_SIZE; i++) {
        if (cw_read((uint8_t)(CW_REG_PROFILE + i), &val, 1) != 0) return -1;
        if (val != s_battery_profile[i]) {
            ESP_LOGE(TAG, "Profile verification failed: index=%u expected=0x%02X actual=0x%02X",
                     (unsigned)i, s_battery_profile[i], val);
            return -1;
        }
    }

    if (cw_read(CW_REG_SOC_ALERT, &val, 1) != 0) return -1;
    if (cw_write(CW_REG_SOC_ALERT, val | CW_UPDATE_FLAG) != 0) return -1;
    return cw_enter_active();
}

// SOC may briefly exceed 100 during the first calculation; wait up to 5 seconds before judging init failed.
static int cw_wait_soc_ready(void) {
    for (int retry = 0; retry < 50; retry++) {
        uint8_t soc = 0;
        vTaskDelay(pdMS_TO_TICKS(100));
        if (cw_read(CW_REG_SOC_H, &soc, 1) == 0 && soc <= 100) return 0;
    }
    return -1;
}

esp_err_t bsp_battery_init(void) {
    if (s_dev) return ESP_OK;

    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) return e;

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_I2C_CW2017_ADDR,
        .scl_speed_hz    = 100000,
    };
    e = i2c_master_bus_add_device(bsp_i2c_bus(), &dc, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(e)); return e; }

    uint8_t ver = 0;
    if (cw_read(CW_REG_VERSION, &ver, 1) != 0) {
        ESP_LOGW(TAG, "CW2017 did not ACK - use bsp_i2c_scan() to confirm whether 0x%02X is online;"
                      "boards without a fuel gauge can ignore this", BSP_I2C_CW2017_ADDR);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "CW2017 detected VERSION=0x%02X", ver);

    bool profile_matches = false;
    if (cw_profile_matches(&profile_matches) != 0) {
        ESP_LOGE(TAG, "Failed to read battery profile");
        e = ESP_FAIL;
        goto fail;
    }

    if (!profile_matches) {
        ESP_LOGI(TAG, "Writing Unitech 500mAh custom profile");
        if (cw_update_profile() != 0) {
            ESP_LOGE(TAG, "Custom profile write failed");
            e = ESP_FAIL;
            goto fail;
        }
    } else {
        uint8_t config = 0;
        if (cw_read(CW_REG_CONFIG, &config, 1) != 0) {
            e = ESP_FAIL;
            goto fail;
        }
        if (config != CW_CONFIG_ACTIVE && cw_enter_active() != 0) {
            e = ESP_FAIL;
            goto fail;
        }
        ESP_LOGI(TAG, "Unitech 500mAh profile matched");
    }

    if (cw_wait_soc_ready() != 0) {
        ESP_LOGE(TAG, "Timed out waiting for first SOC calculation");
        e = ESP_ERR_TIMEOUT;
        goto fail;
    }

    return ESP_OK;

fail:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return e;
}

esp_err_t bsp_battery_sleep(void) {
    if (!s_dev) return ESP_OK;

    for (unsigned attempt = 1; attempt <= 2; attempt++) {
        uint8_t actual = 0;
        esp_err_t write_error = cw_write(CW_REG_CONFIG, CW_CONFIG_SLEEP);
        vTaskDelay(pdMS_TO_TICKS(5));
        esp_err_t read_error = cw_read(CW_REG_CONFIG, &actual, 1);
        if (write_error == ESP_OK && read_error == ESP_OK &&
            actual == CW_CONFIG_SLEEP) {
            ESP_LOGI(TAG, "CW2017 entered sleep and passed register verification");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "CW2017 sleep failed attempt=%u write=%s(0x%x) "
                      "read=%s(0x%x) actual=0x%02X",
                 attempt, esp_err_to_name(write_error), (unsigned)write_error,
                 esp_err_to_name(read_error), (unsigned)read_error, actual);
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(5));
    }

    ESP_LOGE(TAG, "CW2017 failed to enter sleep twice in a row");
    return ESP_FAIL;
}

int bsp_battery_soc(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_SOC_H, b, 2) != 0) return -1;
    int soc = b[0];                       // high byte is the integer percent
    if (soc > 100) return -1;             // may read 0xFF when the chip is not ready
    return soc;
}

int bsp_battery_mv(void) {
    uint8_t b[2] = { 0 };
    if (cw_read(CW_REG_VCELL_H, b, 2) != 0) return -1;
    uint32_t raw = ((uint32_t)b[0] << 8 | b[1]) & 0x3FFF;   // 14bit
    return (int)((raw * 3125) / 10000);                     // raw * 312.5uV → mV
}
