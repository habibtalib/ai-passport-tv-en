// components/bsp/include/bsp_button.h
// Three buttons share one ADC pin, told apart by divider resistors. See bsp_pins.h for the voltage windows.
#pragma once

#include "esp_err.h"

// Button index. The count uses BSP_BTN_COUNT from bsp_pins.h (a hardware property, owned by the pin table);
// no trailing count entry is defined here, to avoid two near-identical names BSP_BTN_COUNT / BSP_BTN_COUNT_.
typedef enum {
    BSP_BTN_UP = 0,
    BSP_BTN_DOWN,
    BSP_BTN_OK,
} bsp_btn_t;

typedef enum {
    BSP_BTN_PRESS = 0,   // Instant of press (low latency, suited to immediate response in games)
    BSP_BTN_CLICK,       // Single click (press and release)
    BSP_BTN_DOUBLE,      // Double click
    BSP_BTN_LONG,        // Long press
    BSP_BTN_RELEASE,     // Instant of release. Added at the end to keep the values above unchanged, but existing callbacks now
                         // receive this extra event: code that dispatches button events with a switch must tolerate (or ignore) it.
                         // After a deep-sleep button wake, use it to tell whether "that key has been released".
    BSP_BTN_END,         // End of a whole click, multi-click or long press, after RELEASE and the delayed CLICK/DOUBLE.
} bsp_btn_ev_t;

// Button event callback. Runs on the shared esp_timer task used by the button component; it may only enqueue or do an
// equally bounded operation; do not block, touch LVGL or do heavy work in it.
typedef void (*bsp_btn_cb_t)(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// Safe to call repeatedly after success; updates the callback and user; on failure the buttons and ADC resources created by this call are rolled back.
// If ADC calibration fails it returns an error rather than decoding invalid voltages as buttons, and can be retried after the fault is fixed.
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user);

// Deep sleep only: stop the three button devices, release the shared ADC unit, then hand the button pin back to a plain digital input
// with pull-up enabled (in parallel with the board's 10k external pull-up). level is filled with the pin level at that time, 1 = released.
//
// Why this is required: after the ADC takes over, the pin's DIGITAL level reads back 0 - and the deep sleep low-level
// wake compares exactly this value. Without restoring the digital input, the wake condition is already true at the instant of sleeping and the device wakes straight back up.
//
// After the call the buttons are unusable for the rest of this run; the caller must immediately enter deep sleep or reboot. (To
// refuse sleeping while "a button is held", first check with bsp_button_read_mv() and BSP_BTN_MV_RELEASED_MIN,
// while the buttons are still alive.)
esp_err_t bsp_button_prepare_deep_sleep(int *level);

// Read the current raw ADC voltage (mV). About 3300 when released; about that key's divider value when a key is held.
// Use it after changing divider/pull-up values to measure your three levels, then edit BSP_BTN_MV_TABLE in bsp_pins.h.
// Returns -1 on read failure.
int bsp_button_read_mv(void);
