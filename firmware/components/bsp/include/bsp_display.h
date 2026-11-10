// components/bsp/include/bsp_display.h
// ST7789P3 240x320 display: SPI panel init + vendor-specific registers + LEDC backlight dimming.
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"
#include <stdbool.h>
#include <stdint.h>

// The final LVGL frame is masked to this radius; pixels outside are pure black.
#define BSP_LVGL_SCREEN_RADIUS 30

// Initialize the SPI bus, panel, vendor registers and backlight LEDC. Safe to call repeatedly after success; on failure the display resources
// created by this call are rolled back and it can be retried after the fault is fixed. On success the screen is powered but its content is undefined.
esp_err_t bsp_display_init(void);

// Get the underlying panel handle. Use for direct esp_lcd_panel_draw_bitmap drawing, or for a GUI other than LVGL.
// Returns NULL if not initialized.
esp_lcd_panel_handle_t bsp_display_panel(void);

// Get the underlying panel io handle (needed to attach LVGL). Returns NULL if not initialized.
esp_lcd_panel_io_handle_t bsp_display_io(void);

// Backlight brightness 0..100 (%). LEDC PWM, 0 = fully off.
void bsp_display_backlight(uint8_t percent);

// Deep sleep only: turn off the display, put the ST7789 into Sleep In, stop the backlight PWM,
// set CS/SCLK/MOSI/DC/backlight to safe levels and hold them through deep sleep. LVGL refresh must
// already be blocked when calling; the caller must immediately enter deep sleep or reboot afterwards.
esp_err_t bsp_display_prepare_deep_sleep(void);

// ---------------------------------------------------------------------------
// Raw landscape owner (TV mode). Stop LVGL rendering before claim(); release()
// restores portrait and LVGL's DMA callback. submit_nowait() queues a stripe;
// drain() waits for queued stripes. Do not reuse a buffer before it is drained.
#include "esp_lcd_panel_io.h"
esp_err_t bsp_display_raw_claim(void);
esp_err_t bsp_display_raw_submit_nowait(int y, int rows, const void *rgb565_be);
esp_err_t bsp_display_raw_drain(unsigned transfers, uint32_t timeout_ms);
esp_err_t bsp_display_raw_release(void);
// Recorded by the LVGL glue so release() can hand the panel back.
void bsp_display_lvgl_callbacks(const esp_lcd_panel_io_callbacks_t *callbacks, void *ctx);

// ---------------------------------------------------------------------------
// LVGL integration (optional layer). Call only after bsp_display_init() succeeded.
// Developers who do not want LVGL can ignore this section and draw directly with bsp_display_panel().
// ---------------------------------------------------------------------------
// Forward declaration: in LVGL, lv_display_t is `typedef struct _lv_display_t lv_display_t;`,
// so the struct form is enough here and avoids forcing this header to include lvgl.h.
struct _lv_display_t;

// Start LVGL and its render task; returns lv_display_t*. Returns NULL on failure; if display/callback registration fails
// the display is rolled back and the already-initialized port is kept, so it can be retried. A partial failure of the port itself needs a reboot,
// so as not to overwrite tasks in the dependency that may not have exited yet. The first init is called serially from a single task.
struct _lv_display_t *bsp_lvgl_init(void);

// LVGL is not thread-safe: from any NON-LVGL task, lock before and unlock after touching any lv_* object.
// lock returns false if LVGL is not ready or on timeout; call unlock only after lock succeeded.
bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);
