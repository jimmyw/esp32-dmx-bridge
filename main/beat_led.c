#include "beat_led.h"

#include <math.h>
#include "audio.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"

static const char *TAG = "beat_led";

#define UPDATE_MS 20
#define MAX_LEVEL 60.0f   // of 255: the DevKit LED is blinding at full brightness

static led_strip_handle_t s_led;
static int s_pin = -1;
static volatile int64_t s_test_until_us;
static volatile uint8_t s_test[3];
static bool s_err_logged;

static void beat_led_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    uint8_t last[3] = { 1, 1, 1 };
    while (1) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(UPDATE_MS));
        audio_state_t a;
        audio_get(&a);
        int64_t now = esp_timer_get_time();
        float r = 0, g = 0, b = 0;
        if (now < s_test_until_us) {
            r = s_test[0] / MAX_LEVEL;
            g = s_test[1] / MAX_LEVEL;
            b = s_test[2] / MAX_LEVEL;
        } else if (a.period_s > 0 || a.signal || a.demo) {
            float flash;
            if (a.period_s > 0) {
                // Brightest right on the beat, gone before the next one. Green: locked to the
                // sound; amber: running on the demo tempo (sound without a beat).
                float p = audio_phase_at(&a, now);
                flash = powf(1 - p, 4);
                if (a.locked) {
                    g = flash;
                } else {
                    r = flash;
                    g = 0.45f * flash;
                }
            } else {
                flash = a.last_beat_us ? expf(-(now - a.last_beat_us) / 80000.0f) : 0;
                b = flash;
            }
            if (a.demo) {
                r += 0.25f * flash + 0.04f;
                b += 0.25f * flash + 0.04f;
            }
        }
        uint8_t px[3] = {
            (uint8_t)fminf(255, r * MAX_LEVEL), (uint8_t)fminf(255, g * MAX_LEVEL), (uint8_t)fminf(255, b * MAX_LEVEL),
        };
        if (px[0] != last[0] || px[1] != last[1] || px[2] != last[2]) {
            led_strip_set_pixel(s_led, 0, px[0], px[1], px[2]);
            esp_err_t err = led_strip_refresh(s_led);
            if (err != ESP_OK && !s_err_logged) {
                ESP_LOGW(TAG, "refresh: %s", esp_err_to_name(err));
                s_err_logged = true;
            }
            last[0] = px[0];
            last[1] = px[1];
            last[2] = px[2];
        }
    }
}

esp_err_t beat_led_test(uint8_t r, uint8_t g, uint8_t b, uint32_t ms)
{
    if (!s_led) return ESP_ERR_INVALID_STATE;
    s_test[0] = r;
    s_test[1] = g;
    s_test[2] = b;
    s_test_until_us = ms ? esp_timer_get_time() + (int64_t)ms * 1000 : 0;
    return ESP_OK;
}

esp_err_t beat_led_set_pin(int pin)
{
    if (!s_led) return ESP_ERR_INVALID_STATE;
    led_strip_clear(s_led);
    esp_err_t err = led_strip_switch_gpio(s_led, pin, false);
    if (err == ESP_OK) {
        s_pin = pin;
        ESP_LOGI(TAG, "beat LED moved to GPIO %d", pin);
    }
    return err;
}

int beat_led_pin(void)
{
    return s_pin;
}

esp_err_t beat_led_start(void)
{
    int pin = CONFIG_DMX_RGB_LED_PIN;
    if (pin < 0) {
        return ESP_OK;
    }
    if (pin == g_config.tx_pin || pin == g_config.de_pin || pin == g_config.led_pin ||
        pin == g_config.mic_sck || pin == g_config.mic_ws || pin == g_config.mic_sd) {
        ESP_LOGW(TAG, "GPIO %d is in use, beat LED off", pin);
        return ESP_ERR_INVALID_STATE;
    }
    const led_strip_config_t cfg = {
        .strip_gpio_num = pin,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &s_led);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED on GPIO %d: %s", pin, esp_err_to_name(err));
        return err;
    }
    led_strip_clear(s_led);
    s_pin = pin;
    ESP_LOGI(TAG, "beat LED on GPIO %d", pin);
    return xTaskCreatePinnedToCore(beat_led_task, "beat_led", 3072, NULL, 2, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
