// components/bsp/include/bsp_i2c.h
// ES8311 (0x18) and CW2017 (0x63) share one I2C bus. This module owns the bus handle alone,
// and both drivers request it from here - avoiding the implicit "whoever inits first creates the bus" ordering dependency.
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

// Initialize the shared bus. Idempotent: repeat calls return ESP_OK immediately, so each driver's init can call it freely.
esp_err_t bsp_i2c_init(void);

// Get the shared bus handle. Returns NULL if not initialized.
i2c_master_bus_handle_t bsp_i2c_bus(void);

// Scan 0x08..0x77 and print every device that ACKs. Very useful for checking whether a chip is soldered or the address is right.
// Scans directly on the real bus; do not open a temporary bus - see the comment on bsp_i2c_scan() in bsp_i2c.c for why.
esp_err_t bsp_i2c_scan(void);

// Deep sleep only: after the last I2C transaction of every shared peripheral completes, set SDA/SCL
// to floating inputs with no pull-up/pull-down. The bus must not be accessed afterwards; the caller must immediately enter deep sleep
// or reboot. The static power draw of the external pull-up resistors on the board is not controlled by this interface.
esp_err_t bsp_i2c_prepare_deep_sleep(void);
