// components/bsp/include/bsp_pins.h
// FoloToy AI Passport: SINGLE SOURCE OF TRUTH for hardware pins and parameters. Changing boards/hardware only requires editing this file.
// Each item says "why this value", so a developer can judge whether it may be changed.
#pragma once

#include "driver/spi_master.h"
#include "driver/i2c_types.h"
#include "hal/adc_types.h"

// ============================================================================
// Display: ST7789P3 240x320, 4-line SPI
// ============================================================================
#define BSP_LCD_W            240
#define BSP_LCD_H            320
#define BSP_LCD_SPI_HOST     SPI2_HOST
#define BSP_LCD_MOSI         9
#define BSP_LCD_SCLK         8
#define BSP_LCD_CS           1
#define BSP_LCD_DC           20
// -1 = reset pin not wired to the MCU (hard-tied to 3.3V); esp_lcd_panel_reset() does a SWRESET soft reset.
#define BSP_LCD_RST          (-1)
#define BSP_LCD_BL           21          // Backlight, LEDC PWM dimming
#define BSP_LCD_PCLK_HZ      (80 * 1000 * 1000)
// ST7789 SCK idles low, samples on the rising edge -> SPI mode 0.
#define BSP_LCD_SPI_MODE     0
// This panel needs color inversion out of the box (the reference example TFT_init() unconditionally sends 0x21 INVON at the end).
// If the image looks like a negative after changing panels, set this to 0.
#define BSP_LCD_INVERT_COLOR 1

// Backlight LEDC parameters
#define BSP_BL_LEDC_TIMER    LEDC_TIMER_0
#define BSP_BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BSP_BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BSP_BL_LEDC_RES      LEDC_TIMER_10_BIT
#define BSP_BL_LEDC_FREQ_HZ  5000

// ============================================================================
// Buttons: three keys share one ADC pin, told apart by divider resistors
//
// Circuit: 3.3V -- external pull-up 10k --+-- ADC node (GPIO0 / ADC1_CH0)
//                                         +-- key -- divider resistor -- GND
//
//   Up    : 0 ohm    -> 3.3 x 0/10k       =   0 mV
//   Down  : 1k       -> 3.3 x 1k/11k      ~ 300 mV
//   OK    : 2.2k     -> 3.3 x 2.2k/12.2k  ~ 595 mV
//   Idle  : no path  -> pulled up to 3300 mV
//
// WARNING: do NOT switch to the INTERNAL pull-up: about 45k ohm and imprecise, it would squeeze all three levels into 0~154mV and let them overlap with temperature drift.
//
// NOTE, if you change the divider/pull-up values: open the demo's Button page, which shows the live ADC voltage;
//   hold each of the three keys and note the readings, take the midpoint of adjacent levels as the window boundary, and edit BSP_BTN_MV below.
// ============================================================================
#define BSP_BTN_ADC_UNIT     ADC_UNIT_1
#define BSP_BTN_ADC_CHANNEL  ADC_CHANNEL_0    // GPIO0
#define BSP_BTN_COUNT        3
// The one GPIO shared by the three keys. Besides ADC reads, deep sleep button wake also uses it: any key pulls
// the pin low, so the wake triggers on low level.
#define BSP_BTN_GPIO         0

// Button timing (ms): passed explicitly by the BSP to the button component, not relying on its Kconfig defaults.
// The short-press window of 180ms matches the component default; it is written here so it can be tuned together with the app feel.
// The component's default long press is 1500ms, which must be held 1.5s to trigger and feels sluggish on this three-key device, so it is tightened to 500ms.
// (The component's Kconfig lower bound for BUTTON_LONG_PRESS_TIME_MS is also 500ms; going shorter can only be done by passing it in code.)
#define BSP_BTN_SHORT_PRESS_MS  180   // Short press (single click) window
#define BSP_BTN_LONG_PRESS_MS   500   // Long press trigger time

// Voltage window per key {min_mV, max_mV}; boundaries are the midpoints of adjacent levels.
// The OK key's upper bound is widened to 1900 to keep distance from the 3300mV idle state.
#define BSP_BTN_MV_TABLE  { {0, 150}, {150, 447}, {447, 1900} }
// Above the highest key window counts as idle (idle is about 3300mV). Before sleeping this is used to refuse sleeping
// while a key is held - otherwise the low-level wake condition is already true at the instant of sleeping (see bsp_button.h).
#define BSP_BTN_MV_RELEASED_MIN  1900

// ============================================================================
// I2C: ES8311 (audio codec) and CW2017 (fuel gauge) share one bus
// ============================================================================
#define BSP_I2C_PORT         I2C_NUM_0
#define BSP_I2C_SDA          10
#define BSP_I2C_SCL          7
#define BSP_I2C_ES8311_ADDR  0x18    // 7-bit address (8-bit form is 0x30)
#define BSP_I2C_CW2017_ADDR  0x63    // 7-bit address

// ============================================================================
// Audio: ES8311, I2S full-duplex (one tx and one rx on the same port, sharing MCLK/BCLK/WS)
// ============================================================================
#define BSP_I2S_PORT         I2S_NUM_0
#define BSP_I2S_MCLK         6
#define BSP_I2S_BCLK         5
#define BSP_I2S_WS           3
#define BSP_I2S_DOUT         2       // Playback: MCU -> codec
#define BSP_I2S_DIN          4       // Record: codec -> MCU
// -1 = amplifier enable pin not wired to the MCU (always on). If your board wires it, fill in the GPIO.
#define BSP_I2S_PA_CTRL      (-1)
