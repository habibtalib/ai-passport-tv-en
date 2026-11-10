// components/bsp/src/bsp_display.c
// Ported from trae_card/components/platform/platform_esp32/src/disp_st7789.c
#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_disp";

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bool                      s_bus_ready;
static bool                      s_bl_ready;
static bool                      s_ready;

static const gpio_num_t s_deep_sleep_pins[] = {
    BSP_LCD_CS, BSP_LCD_SCLK, BSP_LCD_MOSI, BSP_LCD_DC, BSP_LCD_BL,
};

static const uint8_t s_deep_sleep_levels[] = {
    1, 0, 0, 0, 0,
};

static esp_err_t display_set_safe_levels(void) {
    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < sizeof(s_deep_sleep_pins) /
                           sizeof(s_deep_sleep_pins[0]); i++) {
        gpio_num_t pin = s_deep_sleep_pins[i];
        if ((int)pin < 0) continue;
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << (unsigned)pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&cfg);
        if (e == ESP_OK) e = gpio_set_level(pin, s_deep_sleep_levels[i]);
        if (e != ESP_OK && first_error == ESP_OK) first_error = e;
    }
    return first_error;
}

// The deep-sleep hold survives a reset. First disable the global deep hold; while the per-pin hold
// is still in effect write a safe value matching the sleep period, then unlock each pin, avoiding a glitch at the instant of wake.
static esp_err_t display_release_deep_sleep_holds(void) {
    gpio_deep_sleep_hold_dis();
    esp_err_t first_error = display_set_safe_levels();
    for (size_t i = 0; i < sizeof(s_deep_sleep_pins) /
                           sizeof(s_deep_sleep_pins[0]); i++) {
        gpio_num_t pin = s_deep_sleep_pins[i];
        if ((int)pin < 0) continue;
        esp_err_t e = gpio_hold_dis(pin);
        if (e != ESP_OK && first_error == ESP_OK) first_error = e;
    }
    return first_error;
}

// ---------------------------------------------------------------------------
// ST7789P3 vendor-specific init sequence (porch / power / gamma).
// These are [the values in the panel maker's reference example TFT_init()], not generic ST7789 defaults -
// when changing panels you must get a new set from the matching vendor; copying this one will likely give a faulty display.
//
// The following four are done by the built-in esp_lcd driver, so they are not repeated here:
//   0x11 SLPOUT / 0x3A COLMOD → esp_lcd_panel_init()
//   0x21 INVON                → esp_lcd_panel_invert_color()
//   0x29 DISPON               → esp_lcd_panel_disp_on_off()
//   0x36 MADCTL               → esp_lcd_panel_mirror() (WARNING: do not write 0x36 manually again, it would be overridden)
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  cmd;
    uint8_t  data[16];
    uint8_t  len;
    uint16_t delay_ms;
} st_init_cmd_t;

static const st_init_cmd_t ST7789P3_CMDS[] = {
    {0xB2, {0x05, 0x05, 0x00, 0x33, 0x33}, 5, 0},   // PORCTRL frame-rate porch
    {0xB7, {0x35}, 1, 0},                            // GCTRL gate
    {0xBB, {0x21}, 1, 0},                            // VCOMS
    {0xC0, {0x2C}, 1, 0},                            // LCMCTRL
    {0xC2, {0x01}, 1, 0},                            // VDVVRHEN
    {0xC3, {0x0B}, 1, 0},                            // VRHS
    {0xC4, {0x20}, 1, 0},                            // VDVSET
    {0xC6, {0x0F}, 1, 0},                            // FRCTRL2 60Hz dot inversion
    {0xD0, {0xA7, 0xA1}, 2, 0},                      // PWCTRL1
    {0xD0, {0xA4, 0xA1}, 2, 0},                      // PWCTRL1 (re-sent per the reference example, overrides the previous one)
    {0xD6, {0xA1}, 1, 0},
    {0xE0, {0xD0, 0x04, 0x08, 0x0A, 0x09, 0x05, 0x2D, 0x43,
            0x49, 0x09, 0x16, 0x15, 0x26, 0x2B}, 14, 0},   // PVGAMCTRL positive gamma
    {0xE1, {0xD0, 0x03, 0x09, 0x0A, 0x0A, 0x06, 0x2E, 0x44,
            0x40, 0x3A, 0x15, 0x15, 0x26, 0x2A}, 14, 10},  // NVGAMCTRL negative gamma
};

static esp_err_t backlight_init(void) {
    if (BSP_LCD_BL < 0) { ESP_LOGW(TAG, "Backlight pin not wired to the MCU, brightness not adjustable"); return ESP_OK; }
    ledc_timer_config_t t = {
        .speed_mode      = BSP_BL_LEDC_MODE,
        .timer_num       = BSP_BL_LEDC_TIMER,
        .duty_resolution = BSP_BL_LEDC_RES,
        .freq_hz         = BSP_BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t e = ledc_timer_config(&t);
    if (e != ESP_OK) { ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(e)); return e; }

    ledc_channel_config_t ch = {
        .gpio_num   = BSP_LCD_BL,
        .speed_mode = BSP_BL_LEDC_MODE,
        .channel    = BSP_BL_LEDC_CHANNEL,
        .timer_sel  = BSP_BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    e = ledc_channel_config(&ch);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(e));
        ledc_timer_rst(BSP_BL_LEDC_MODE, BSP_BL_LEDC_TIMER);
        return e;
    }

    s_bl_ready = true;
    ESP_LOGI(TAG, "Backlight LEDC ready gpio=%d", BSP_LCD_BL);
    return ESP_OK;
}

esp_err_t bsp_display_init(void) {
    if (s_ready) return ESP_OK;
    esp_err_t e = display_release_deep_sleep_holds();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "LCD deep-sleep hold release failed: %s", esp_err_to_name(e));
        return e;
    }
    if (s_panel || s_io || s_bus_ready) {
        ESP_LOGE(TAG, "Previous display init rollback incomplete, refusing to overwrite the still-live SPI resources");
        return ESP_ERR_INVALID_STATE;
    }

    spi_bus_config_t bus = {
        .mosi_io_num = BSP_LCD_MOSI,
        .sclk_io_num = BSP_LCD_SCLK,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_W * 80 * 2,
    };
    e = spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed (%s) - check whether MOSI=GPIO%d / SCLK=GPIO%d conflict",
                 esp_err_to_name(e), BSP_LCD_MOSI, BSP_LCD_SCLK);
        return e;
    }
    s_bus_ready = true;

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_LCD_CS,
        .dc_gpio_num = BSP_LCD_DC,
        .pclk_hz = BSP_LCD_PCLK_HZ,
        .spi_mode = BSP_LCD_SPI_MODE,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io_cfg, &s_io);
    if (e != ESP_OK) { ESP_LOGE(TAG, "panel_io creation failed: %s", esp_err_to_name(e)); goto fail; }

    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = BSP_LCD_RST,          // -1 -> SWRESET soft reset
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    e = esp_lcd_new_panel_st7789(s_io, &dev, &s_panel);
    if (e != ESP_OK) { ESP_LOGE(TAG, "Panel creation failed: %s", esp_err_to_name(e)); goto fail; }

    e = esp_lcd_panel_reset(s_panel);   // SWRESET when rst=-1
    if (e != ESP_OK) { ESP_LOGE(TAG, "Panel reset failed: %s", esp_err_to_name(e)); goto fail; }
    e = esp_lcd_panel_init(s_panel);    // SLPOUT / COLMOD / RAMCTRL
    if (e != ESP_OK) { ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(e)); goto fail; }

    for (size_t i = 0; i < sizeof(ST7789P3_CMDS) / sizeof(ST7789P3_CMDS[0]); i++) {
        const st_init_cmd_t *c = &ST7789P3_CMDS[i];
        e = esp_lcd_panel_io_tx_param(s_io, c->cmd, c->data, c->len);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "Vendor init command 0x%02X failed: %s", c->cmd, esp_err_to_name(e));
            goto fail;
        }
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    e = esp_lcd_panel_invert_color(s_panel, BSP_LCD_INVERT_COLOR);   // 0x21 / 0x20
    if (e != ESP_OK) { ESP_LOGE(TAG, "Panel color inversion setting failed: %s", esp_err_to_name(e)); goto fail; }
    e = esp_lcd_panel_mirror(s_panel, false, false);                 // 0x36 MADCTL: no mirroring needed on this board (XY double mirror = image rotated 180 deg)
    if (e != ESP_OK) { ESP_LOGE(TAG, "Panel mirror setting failed: %s", esp_err_to_name(e)); goto fail; }
    e = esp_lcd_panel_set_gap(s_panel, 0, 0);
    if (e != ESP_OK) { ESP_LOGE(TAG, "Panel offset setting failed: %s", esp_err_to_name(e)); goto fail; }
    e = esp_lcd_panel_disp_on_off(s_panel, true);                    // 0x29 DISPON
    if (e != ESP_OK) { ESP_LOGE(TAG, "Panel display-on failed: %s", esp_err_to_name(e)); goto fail; }

    e = backlight_init();
    if (e != ESP_OK) goto fail;
    s_ready = true;
    ESP_LOGI(TAG, "Display ready %dx%d", BSP_LCD_W, BSP_LCD_H);
    return ESP_OK;

fail:
    s_ready = false;
    if (s_panel) {
        esp_err_t cleanup = esp_lcd_panel_del(s_panel);
        if (cleanup == ESP_OK) s_panel = NULL;
        else ESP_LOGE(TAG, "Panel rollback failed: %s", esp_err_to_name(cleanup));
    }
    if (!s_panel && s_io) {
        esp_err_t cleanup = esp_lcd_panel_io_del(s_io);
        if (cleanup == ESP_OK) s_io = NULL;
        else ESP_LOGE(TAG, "Panel IO rollback failed: %s", esp_err_to_name(cleanup));
    }
    if (!s_io && s_bus_ready) {
        esp_err_t cleanup = spi_bus_free(BSP_LCD_SPI_HOST);
        if (cleanup == ESP_OK) s_bus_ready = false;
        else ESP_LOGE(TAG, "SPI bus rollback failed: %s", esp_err_to_name(cleanup));
    }
    s_bl_ready = false;
    return e;
}

esp_lcd_panel_handle_t bsp_display_panel(void) { return s_panel; }

esp_lcd_panel_io_handle_t bsp_display_io(void) { return s_io; }

void bsp_display_backlight(uint8_t percent) {
    if (!s_bl_ready) return;
    if (percent > 100) percent = 100;
    uint32_t max_duty = (1u << BSP_BL_LEDC_RES) - 1u;
    uint32_t duty = (max_duty * percent) / 100u;
    ledc_set_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL);
}

esp_err_t bsp_display_prepare_deep_sleep(void) {
    esp_err_t first_error = ESP_OK;
    if (!s_ready || !s_panel) {
        first_error = ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG, "LCD not ready, cannot send deep-sleep commands");
    } else {
        esp_err_t e = esp_lcd_panel_disp_on_off(s_panel, false);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ST7789 display-off failed: %s", esp_err_to_name(e));
            first_error = e;
        }
        e = esp_lcd_panel_disp_sleep(s_panel, true);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ST7789 Sleep In failed: %s", esp_err_to_name(e));
            if (first_error == ESP_OK) first_error = e;
        }
    }

    bsp_display_backlight(0);
    if (s_bl_ready && BSP_LCD_BL >= 0) {
        esp_err_t e = ledc_stop(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, 0);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "LCD backlight PWM stop failed: %s", esp_err_to_name(e));
            if (first_error == ESP_OK) first_error = e;
        }
    }

    esp_err_t e = display_set_safe_levels();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "LCD SPI safe-level configuration failed: %s", esp_err_to_name(e));
        if (first_error == ESP_OK) first_error = e;
    }
    for (size_t i = 0; i < sizeof(s_deep_sleep_pins) /
                           sizeof(s_deep_sleep_pins[0]); i++) {
        gpio_num_t pin = s_deep_sleep_pins[i];
        if ((int)pin < 0) continue;
        e = gpio_hold_en(pin);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "LCD GPIO%d hold failed: %s", (int)pin, esp_err_to_name(e));
            if (first_error == ESP_OK) first_error = e;
        }
    }
    gpio_deep_sleep_hold_en();
    ESP_LOGI(TAG, "ST7789 asleep, LCD SPI and backlight safe levels held");
    return first_error;
}

// ---------------------------------------------------------------------------
// Raw landscape ownership, used by TV mode while LVGL rendering is stopped.
// Ported from ai-passport-tv (MIT). The caller must stop LVGL first: claim()
// replaces LVGL's DMA-done callback and release() puts it back.
static SemaphoreHandle_t s_raw_done;
static bool s_raw;
static unsigned s_raw_outstanding;
static esp_lcd_panel_io_callbacks_t s_lvgl_callbacks;
static void *s_lvgl_callback_ctx;

static bool raw_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *ctx) {
    (void)io; (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    return wake == pdTRUE;
}

void bsp_display_lvgl_callbacks(const esp_lcd_panel_io_callbacks_t *callbacks, void *ctx) {
    if (callbacks) s_lvgl_callbacks = *callbacks;
    s_lvgl_callback_ctx = ctx;
}

esp_err_t bsp_display_raw_claim(void) {
    if (!s_panel || s_raw) return ESP_ERR_INVALID_STATE;
    s_raw_done = xSemaphoreCreateCounting(32, 0);
    if (!s_raw_done) return ESP_ERR_NO_MEM;
    // esp_lcd logs one "overwritten" warning per handover; it is expected here.
    esp_lcd_panel_io_callbacks_t cb = { .on_color_trans_done = raw_done };
    esp_err_t e = esp_lcd_panel_io_register_event_callbacks(s_io, &cb, s_raw_done);
    if (e == ESP_OK) e = esp_lcd_panel_swap_xy(s_panel, true);
    if (e == ESP_OK) e = esp_lcd_panel_mirror(s_panel, true, false);
    if (e != ESP_OK) {
        esp_lcd_panel_io_register_event_callbacks(s_io, &s_lvgl_callbacks, s_lvgl_callback_ctx);
        vSemaphoreDelete(s_raw_done); s_raw_done = NULL;
        return e;
    }
    s_raw = true;
    s_raw_outstanding = 0;
    return ESP_OK;
}

esp_err_t bsp_display_raw_submit_nowait(int y, int rows, const void *pixels) {
    if (!s_raw) return ESP_ERR_INVALID_STATE;
    if (!pixels || y < 0 || rows <= 0 || y + rows > BSP_LCD_W) return ESP_ERR_INVALID_ARG;
    esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, 0, y, BSP_LCD_H, y + rows, pixels);
    if (e == ESP_OK) s_raw_outstanding++;
    return e;
}

esp_err_t bsp_display_raw_drain(unsigned transfers, uint32_t timeout_ms) {
    if (!s_raw) return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < transfers; i++) {
        if (!s_raw_outstanding) break;
        if (!xSemaphoreTake(s_raw_done, pdMS_TO_TICKS(timeout_ms))) return ESP_ERR_TIMEOUT;
        s_raw_outstanding--;
    }
    return ESP_OK;
}

esp_err_t bsp_display_raw_release(void) {
    if (!s_raw || s_raw_outstanding) return ESP_ERR_INVALID_STATE;
    esp_err_t e = esp_lcd_panel_io_register_event_callbacks(s_io, &s_lvgl_callbacks, s_lvgl_callback_ctx);
    if (e != ESP_OK) return e;
    esp_lcd_panel_swap_xy(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);
    vSemaphoreDelete(s_raw_done); s_raw_done = NULL;
    s_raw = false;
    return ESP_OK;
}
