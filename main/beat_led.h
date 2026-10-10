#pragma once

#include "esp_err.h"

// The board's addressable RGB LED (CONFIG_DMX_RGB_LED_PIN, a WS2812, usually on GPIO 48) flashes on every beat of the audio analysis, so the beat lock can be checked by eye:
// green = locked to a tempo, blue = onsets only (no tempo yet), purple tint = demo, off = silence.
esp_err_t beat_led_start(void);

// Diagnostics (serial `rgbled`): show a fixed colour for `ms` instead of the beat (0 = back to the
// beat), and move the LED to another GPIO until the next restart.
esp_err_t beat_led_test(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);
esp_err_t beat_led_set_pin(int pin);
int       beat_led_pin(void);
