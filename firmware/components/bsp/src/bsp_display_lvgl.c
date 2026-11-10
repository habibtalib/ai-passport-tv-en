// components/bsp/src/bsp_display_lvgl.c
// LVGL integration lives in its own file: developers not using LVGL can delete this file plus the two dependencies in idf_component.yml.
#include "bsp_display.h"
#include "bsp_display_rounding.h"
#include "bsp_pins.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "bsp_lvgl";

#define BSP_LVGL_DRAW_BUFFER_LINES 40

static lv_display_t *s_disp;
static bool s_port_initialized;
static bool s_port_init_failed;

// Same work as esp_lvgl_port's private flush-ready callback. Registered here so
// the raw (TV) path can restore LVGL's DMA-done handling after it borrows the panel.
static bool lvgl_flush_ready(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *ctx) {
    (void)io; (void)event;
    lv_display_flush_ready((lv_display_t *)ctx);
    return false;
}

static void rounded_flush_event(lv_event_t *event)
{
    lv_display_t *disp = lv_event_get_target(event);
    const lv_area_t *area = lv_event_get_param(event);
    lv_draw_buf_t *draw_buf = lv_display_get_buf_active(disp);
    if (!area || !draw_buf || !draw_buf->data ||
        lv_display_get_color_format(disp) != LV_COLOR_FORMAT_RGB565) {
        return;
    }

    const int32_t width = lv_area_get_width(area);
    if (draw_buf->header.stride < (uint32_t)width * sizeof(uint16_t)) return;

    for (int32_t y = area->y1; y <= area->y2; ++y) {
        uint16_t *row = (uint16_t *)(draw_buf->data +
                                     (y - area->y1) * draw_buf->header.stride);
        int32_t visible_x1;
        int32_t visible_x2;
        if (!bsp_display_rounded_row_span(y, BSP_LCD_W, BSP_LCD_H,
                                          BSP_LVGL_SCREEN_RADIUS, &visible_x1,
                                          &visible_x2)) {
            memset(row, 0, (size_t)width * sizeof(uint16_t));
            continue;
        }
        // Only clear pixels outside the visible span. The port swaps RGB565
        // bytes after this event; black is 0 in either byte order.
        const int32_t clear_left_end = visible_x1 > area->x2 ? area->x2 : visible_x1 - 1;
        const int32_t clear_right_start = visible_x2 < area->x1 ? area->x1 : visible_x2 + 1;
        for (int32_t x = area->x1; x <= clear_left_end; ++x) {
            row[x - area->x1] = 0;
        }
        for (int32_t x = clear_right_start; x <= area->x2; ++x) {
            row[x - area->x1] = 0;
        }
    }

}

lv_display_t *bsp_lvgl_init(void) {
    if (s_disp) return s_disp;
    if (!bsp_display_panel()) {
        ESP_LOGE(TAG, "Call bsp_display_init() successfully first");
        return NULL;
    }

    if (!s_port_initialized) {
        // Port 2.9.0 has no public completion handshake for asynchronous deinit.
        // Never overwrite a possibly live context after a partial port failure.
        if (s_port_init_failed) {
            ESP_LOGE(TAG, "LVGL port init incomplete, reboot and retry");
            return NULL;
        }
        const lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
        if (lvgl_port_init(&pc) != ESP_OK) {
            s_port_init_failed = true;
            ESP_LOGE(TAG, "lvgl_port_init failed, reboot and retry");
            return NULL;
        }
        s_port_initialized = true;
    }

    const lvgl_port_display_cfg_t dc = {
        .panel_handle = bsp_display_panel(),
        .io_handle    = bsp_display_io(),
        // WARNING: the C3 has no PSRAM and DMA can only use internal RAM. A 40-line single buffer is about 19.2KB,
        // which reduces window commands and queue submissions; still single-buffered, to avoid double buffering squeezing audio/Wi-Fi.
        .buffer_size   = (uint32_t)BSP_LCD_W * BSP_LVGL_DRAW_BUFFER_LINES,
        .double_buffer = false,
        .hres = BSP_LCD_W, .vres = BSP_LCD_H,
        // Rotation/mirroring must be configured here: esp_lvgl_port re-sends MADCTL when it registers the display,
        // overriding the esp_lcd_panel_mirror() setting in bsp_display.c.
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        // swap_bytes: LVGL outputs little-endian RGB565, but the ST7789 over SPI wants big-endian -> bytes must be swapped.
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    // The port mutex is recursive. Keep registration and the mask callback in
    // one critical section, before the new display can produce its first flush.
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "LVGL init lock failed");
        return NULL;
    }
    lv_display_t *disp = lvgl_port_add_disp(&dc);
    bool mask_registered = false;
    if (disp) {
        // LVGL 9.5 returns void here; check the list while still holding the lock.
        const uint32_t count = lv_display_get_event_count(disp);
        lv_display_add_event_cb(disp, rounded_flush_event, LV_EVENT_FLUSH_START, NULL);
        mask_registered = lv_display_get_event_count(disp) == count + 1;
    }
    if (!mask_registered) {
        ESP_LOGE(TAG, "LVGL display or rounded-corner callback registration failed");
        if (disp) lvgl_port_remove_disp(disp);
        lvgl_port_unlock();
        // Retain the initialized port for retry. Deinit is asynchronous and can
        // race the next init (or even run before the task sets running=true).
        return NULL;
    }

    // Mask the final RGB565 flush instead of using root-screen clip_corner.
    // Full-screen rounded clipping creates an ARGB layer that does not fit the
    // 24 KB LVGL pool reliably on this no-PSRAM target.
    s_disp = disp;
    const esp_lcd_panel_io_callbacks_t flush = { .on_color_trans_done = lvgl_flush_ready };
    bsp_display_lvgl_callbacks(&flush, disp);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL ready, global corner radius=%d, outer fill=black", BSP_LVGL_SCREEN_RADIUS);
    return s_disp;
}

bool bsp_lvgl_lock(int timeout_ms) {
    if (!s_disp) return false;
    return lvgl_port_lock(timeout_ms);
}
void bsp_lvgl_unlock(void) {
    if (s_disp) lvgl_port_unlock();
}
