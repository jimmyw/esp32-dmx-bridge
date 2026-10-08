#include "artnet.h"
#include "cli.h"
#include "config.h"
#include "dmx_buffer.h"
#include "dmx_out.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "names.h"
#include "scenes.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sacn.h"
#include "sdkconfig.h"
#include "web.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

#define RESET_HOLD_MS 5000

// Factory-reset button + status LED.
// LED: AP mode = fast blink, connecting = slow blink, connected = on, DMX received = flicker.
static void ui_task(void *arg)
{
    const int btn = CONFIG_DMX_RESET_GPIO;
    const int led = g_config.led_pin;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << btn,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    if (led >= 0) {
        gpio_reset_pin(led);
        gpio_set_direction(led, GPIO_MODE_OUTPUT);
    }

    int held_ms = 0;
    uint32_t tick = 0;
    uint32_t last_pkts = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(50));
        tick++;

        if (gpio_get_level(btn) == 0) {
            held_ms += 50;
            if (held_ms >= RESET_HOLD_MS) {
                ESP_LOGW(TAG, "factory reset");
                config_factory_reset();
                esp_restart();
            }
        } else {
            held_ms = 0;
        }

        if (led < 0) {
            continue;
        }
        int level;
        dmx_stats_t st;
        dmx_buffer_get_stats(&st);
        uint32_t pkts = st.artnet_packets + st.sacn_packets;
        if (held_ms > 0) {
            level = (tick / 2) & 1;                  // button held: rapid blink
        } else if (wifi_mgr_sta_connected()) {
            level = (pkts != last_pkts) ? (tick & 1) : 1;
        } else if (wifi_mgr_ap_active()) {
            level = (tick / 3) & 1;
        } else {
            level = (tick / 10) & 1;
        }
        last_pkts = pkts;
        gpio_set_level(led, level);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "DMX bridge %s", esp_app_get_description()->version);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    config_init();
    dmx_buffer_init();
    names_init();
    scenes_init();
    ESP_ERROR_CHECK(dmx_out_start());
    ESP_ERROR_CHECK(wifi_mgr_start());
    ESP_ERROR_CHECK(artnet_start());
    ESP_ERROR_CHECK(sacn_start());
    ESP_ERROR_CHECK(web_start());
    xTaskCreate(ui_task, "ui", 3072, NULL, 3, NULL);
    ESP_ERROR_CHECK(cli_start());

    // Everything came up: confirm a freshly uploaded firmware so the bootloader keeps it.
    // If it crashes before reaching this point, the next reset rolls back to the old one.
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "new firmware booted OK, cancelling rollback");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}
