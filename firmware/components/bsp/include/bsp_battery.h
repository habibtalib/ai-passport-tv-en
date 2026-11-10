// components/bsp/include/bsp_battery.h
// CellWise CW2017 fuel gauge: I2C 0x63, shares the bus with the ES8311.
// The BSP writes a custom battery profile; the chip reports SOC% directly, no external divider or lookup table needed.
#pragma once

#include "esp_err.h"

// Initialize. Calls bsp_i2c_init() internally (idempotent).
// Returns ESP_ERR_NOT_FOUND if the chip does not ACK - the upper layer can mark the item unavailable in the UI.
esp_err_t bsp_battery_init(void);

// Write the CW2017 CONFIG register to its sleep value and read it back to confirm; retries once on failure. This is the deep
// sleep terminal operation; if the chip was never initialized there is nothing to do and it returns success.
esp_err_t bsp_battery_sleep(void);

// Remaining charge percentage 0..100; returns -1 on read failure.
int bsp_battery_soc(void);

// Battery voltage in mV; returns -1 on read failure.
int bsp_battery_mv(void);
