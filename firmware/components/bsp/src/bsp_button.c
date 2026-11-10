// components/bsp/src/bsp_button.c
// Ported from trae_card/components/platform/platform_esp32/src/btn_iot_button.c
#include "bsp_button.h"
#include "bsp_pins.h"
#include "iot_button.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/task.h"

static const char *TAG = "bsp_btn";

static const uint16_t BTN_MV[BSP_BTN_COUNT][2] = BSP_BTN_MV_TABLE;

static button_handle_t s_btn[BSP_BTN_COUNT];
static bsp_btn_cb_t    s_cb;
static void           *s_user;
static volatile bool   s_ready;

// ADC1 is a unit-level exclusive resource: iot_button and bsp_button_read_mv() must share the same oneshot
// handle. Whoever calls adc_oneshot_new_unit() second gets "adc1 is already in use".
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

#define BSP_BTN_ATTEN  ADC_ATTEN_DB_12              // Range about 0~3100mV, covers the idle state

// Use the public button-driver interface, not button_adc's global registry:
// button 4.2.0 leaves an occupied index behind when its core allocation fails.
// These static drivers and the single ADC/calibration pair are owned by BSP.
typedef struct {
    button_driver_t base;
    unsigned index;
} bsp_adc_button_t;
static bsp_adc_button_t s_drivers[BSP_BTN_COUNT];
static int64_t s_sample_time;
static int s_sample_mv = -1;
static bool s_sample_valid;

static uint8_t button_level(button_driver_t *driver) {
    if (!s_ready) return BUTTON_INACTIVE;
    const bsp_adc_button_t *button = (const bsp_adc_button_t *)driver;
    const int64_t now = esp_timer_get_time();
    // Share one averaged reading across the three keys in a polling cycle.
    if (!s_sample_valid || now - s_sample_time >= 1000) {
        int sum = 0;
        s_sample_time = now;
        s_sample_valid = true;
        s_sample_mv = -1;
        for (int i = 0; i < CONFIG_ADC_BUTTON_SAMPLE_TIMES; ++i) {
            int raw;
            if (adc_oneshot_read(s_adc, BSP_BTN_ADC_CHANNEL, &raw) != ESP_OK) {
                return BUTTON_INACTIVE;
            }
            sum += raw;
        }
        if (adc_cali_raw_to_voltage(s_cali, sum / CONFIG_ADC_BUTTON_SAMPLE_TIMES,
                                   &s_sample_mv) != ESP_OK) {
            s_sample_mv = -1;
        }
    }
    // Half-open windows prevent two keys from matching a shared boundary.
    return s_sample_mv >= BTN_MV[button->index][0] &&
           s_sample_mv < BTN_MV[button->index][1] ? BUTTON_ACTIVE : BUTTON_INACTIVE;
}

static esp_err_t button_driver_delete(button_driver_t *driver) {
    (void)driver; // Static storage; shared ADC is released after all buttons.
    return ESP_OK;
}

// Each button carries "which key" back with its callback. The button component's callback signature is fixed, so usr_data carries the index.
static void on_event(void *arg, void *usr_data, bsp_btn_ev_t ev) {
    (void)arg;
    if (!s_ready || !s_cb) return;
    s_cb((bsp_btn_t)(intptr_t)usr_data, ev, s_user);
}
static void cb_press (void *a, void *u) { on_event(a, u, BSP_BTN_PRESS);  }
static void cb_click (void *a, void *u) { on_event(a, u, BSP_BTN_CLICK);  }
static void cb_double(void *a, void *u) { on_event(a, u, BSP_BTN_DOUBLE); }
static void cb_long  (void *a, void *u) { on_event(a, u, BSP_BTN_LONG);   }
static void cb_release(void *a, void *u) { on_event(a, u, BSP_BTN_RELEASE); }
static void cb_end(void *a, void *u) { on_event(a, u, BSP_BTN_END); }

// If init fails midway, first stop all button drivers, then release the calibration and ADC unit held by this file.
// The ADC must not be deleted while a button driver is still polling, otherwise the timer callback would touch a dead handle.
// Returns ESP_OK when buttons, calibration and ADC unit are all fully reclaimed; otherwise returns the first error code,
// for prepare_deep_sleep to decide whether it is safe to switch back to digital input.
static esp_err_t button_cleanup(void) {
    esp_err_t first_err = ESP_OK;
    s_cb = NULL;
    s_user = NULL;
    s_ready = false;
    s_sample_valid = false;

    for (int i = BSP_BTN_COUNT - 1; i >= 0; i--) {
        if (!s_btn[i]) continue;
        esp_err_t e = iot_button_delete(s_btn[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "Button %d rollback failed: %s", i, esp_err_to_name(e));
            if (first_err == ESP_OK) first_err = e;
            continue;
        }
        s_btn[i] = NULL;
    }

    // Never free the ADC beneath a driver whose deletion failed.
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (s_btn[i]) {
            if (first_err == ESP_OK) first_err = ESP_ERR_INVALID_STATE;
            return first_err;
        }
    }

    if (s_cali) {
        esp_err_t e = adc_cali_delete_scheme_curve_fitting(s_cali);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ADC calibration rollback failed: %s", esp_err_to_name(e));
            if (first_err == ESP_OK) first_err = e;
        } else {
            s_cali = NULL;
        }
    }
    if (s_adc) {
        esp_err_t e = adc_oneshot_del_unit(s_adc);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ADC unit rollback failed: %s", esp_err_to_name(e));
            if (first_err == ESP_OK) first_err = e;
        } else {
            s_adc = NULL;
        }
    }
    return first_err;
}

static esp_err_t register_callbacks(button_handle_t button, void *index) {
    esp_err_t e = iot_button_register_cb(button, BUTTON_PRESS_DOWN, NULL, cb_press, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_SINGLE_CLICK, NULL, cb_click, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_DOUBLE_CLICK, NULL, cb_double, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_LONG_PRESS_START, NULL, cb_long, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_PRESS_UP, NULL, cb_release, index);
    if (e == ESP_OK) e = iot_button_register_cb(button, BUTTON_PRESS_END, NULL, cb_end, index);
    return e;
}

esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user) {
    if (s_ready) {
        s_cb = cb;
        s_user = user;
        return ESP_OK;
    }
    if (s_adc || s_cali) {
        ESP_LOGE(TAG, "Previous button init rollback incomplete, refusing to overwrite the still-live ADC handle");
        return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        if (s_btn[i]) {
            ESP_LOGE(TAG, "Previous button %d rollback incomplete, refusing to allocate resources again", i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_cb = cb; s_user = user;

    // BSP owns one ADC unit and one calibration handle for polling and reads.
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BSP_BTN_ADC_UNIT };
    esp_err_t ae = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit creation failed (%s)", esp_err_to_name(ae));
        s_adc = NULL;
        (void)button_cleanup();
        return ae;
    }

    const adc_oneshot_chan_cfg_t channel = {
        .atten = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ae = adc_oneshot_config_channel(s_adc, BSP_BTN_ADC_CHANNEL, &channel);
    if (ae != ESP_OK) { (void)button_cleanup(); return ae; }
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = BSP_BTN_ADC_UNIT,
        .chan = BSP_BTN_ADC_CHANNEL,
        .atten = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ae = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if (ae != ESP_OK) {
        ESP_LOGE(TAG, "ADC calibration creation failed: %s", esp_err_to_name(ae));
        (void)button_cleanup();
        return ae; // No guessed mV: an unavailable reading is not an UP press.
    }

    for (int i = 0; i < BSP_BTN_COUNT; i++) {
        s_drivers[i] = (bsp_adc_button_t){
            .base = { .get_key_level = button_level, .del = button_driver_delete },
            .index = (unsigned)i,
        };
        // Detection thresholds are passed explicitly by the BSP (see bsp_pins.h): the component's Kconfig default long press of 1500ms is sluggish.
        const button_config_t bc = {
            .short_press_time = BSP_BTN_SHORT_PRESS_MS,
            .long_press_time  = BSP_BTN_LONG_PRESS_MS,
        };
        esp_err_t e = iot_button_create(&bc, &s_drivers[i].base, &s_btn[i]);
        if (e != ESP_OK || !s_btn[i]) {
            ESP_LOGE(TAG, "Button %d creation failed (%s) - check the ADC config and divider resistor on GPIO%d",
                     i, esp_err_to_name(e), BSP_BTN_ADC_CHANNEL);
            e = e == ESP_OK ? ESP_FAIL : e;
            (void)button_cleanup();
            return e;
        }
        void *idx = (void *)(intptr_t)i;
        e = register_callbacks(s_btn[i], idx);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "Button %d callback registration failed: %s", i, esp_err_to_name(e));
            (void)button_cleanup();
            return e;
        }
    }

    s_sample_valid = false;
    s_ready = true;
    ESP_LOGI(TAG, "Buttons ready: ADC1_CH%d three-key divider, short press %dms long press %dms",
             BSP_BTN_ADC_CHANNEL, BSP_BTN_SHORT_PRESS_MS, BSP_BTN_LONG_PRESS_MS);
    return ESP_OK;
}

int bsp_button_read_mv(void) {
    // Reads the ADC channel that bsp_button_init() set up and shares with iot_button.
    // A single sample does not interfere with the component's button polling (oneshot has its own lock).
    if (!s_adc || !s_cali) return -1;

    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, BSP_BTN_ADC_CHANNEL, &raw) != ESP_OK) return -1;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    return mv;
}

esp_err_t bsp_button_prepare_deep_sleep(int *level) {
    if (level) *level = 0;

    // (1) Stop the button driver first, then release the ADC - the reverse would let the timer callback touch a released handle.
    // If the rollback is incomplete (a live button handle or ADC unit remains) it must not switch back to digital input: otherwise
    // a still-polling driver is left behind and the caller wrongly thinks it is safe to sleep.
    esp_err_t err = button_cleanup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Button/ADC rollback incomplete, refusing to switch back to digital input: %s", esp_err_to_name(err));
        return err;
    }

    // (2) Hand the button pin back to plain digital input + pull-up (in parallel with the board's 10k external pull-up).
    const gpio_num_t pin = (gpio_num_t)BSP_BTN_GPIO;
    err = gpio_set_direction(pin, GPIO_MODE_INPUT);
    if (err == ESP_OK) err = gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch button pin back to digital input: %s", esp_err_to_name(err));
        return err;
    }

    // The pull-up needs a moment to settle; read back afterwards.
    vTaskDelay(pdMS_TO_TICKS(20));
    if (level) *level = gpio_get_level(pin);
    ESP_LOGI(TAG, "Buttons released, deep-sleep wake pin GPIO%d level=%d", (int)pin, level ? *level : -1);
    return ESP_OK;
}
