#include "config.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "config";
static const char *NVS_NS = "bridge";
static const char *NVS_KEY = "cfg";
#define CONFIG_VERSION 1

typedef struct {
    uint32_t        version;
    bridge_config_t cfg;
} stored_config_t;

bridge_config_t g_config;
static char s_mac_suffix[5];

const char *config_mac_suffix(void)
{
    return s_mac_suffix;
}

bool config_pin_valid(int pin, bool allow_none)
{
    if (pin == -1) {
        return allow_none;
    }
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) {
        return false;
    }
    // ESP32-S3: 19/20 = USB D-/D+ (console), 26..32 = SPI flash/PSRAM, 22..25 don't exist.
    if (pin == 19 || pin == 20 || (pin >= 22 && pin <= 32)) {
        return false;
    }
    return true;
}

static void set_defaults(void)
{
    memset(&g_config, 0, sizeof(g_config));
    snprintf(g_config.hostname, sizeof(g_config.hostname), "%s-%s",
             CONFIG_DMX_HOSTNAME_PREFIX, s_mac_suffix);
    snprintf(g_config.name, sizeof(g_config.name), "DMX Bridge %s", s_mac_suffix);
    g_config.protocol         = PROTO_BOTH;
    g_config.artnet_port_addr = 0;
    g_config.sacn_universe    = 1;
    g_config.tx_pin           = CONFIG_DMX_TX_PIN;
    g_config.de_pin           = CONFIG_DMX_DE_PIN;
    g_config.led_pin          = CONFIG_DMX_LED_PIN;
    g_config.uart_num         = CONFIG_DMX_UART_NUM;
    g_config.refresh_hz       = 40;
    g_config.on_loss          = LOSS_HOLD;
    g_config.loss_timeout_ms  = 3000;
}

static void sanitize(void)
{
    if (!config_pin_valid(g_config.tx_pin, false)) g_config.tx_pin = CONFIG_DMX_TX_PIN;
    if (!config_pin_valid(g_config.de_pin, true))  g_config.de_pin = -1;
    if (!config_pin_valid(g_config.led_pin, true)) g_config.led_pin = -1;
    if (g_config.uart_num < 1 || g_config.uart_num > 2) g_config.uart_num = 1;
    if (g_config.refresh_hz < 1 || g_config.refresh_hz > 44) g_config.refresh_hz = 40;
    if (g_config.protocol < PROTO_ARTNET || g_config.protocol > PROTO_BOTH) g_config.protocol = PROTO_BOTH;
    g_config.artnet_port_addr &= 0x7FFF;
    if (g_config.sacn_universe < 1 || g_config.sacn_universe > 63999) g_config.sacn_universe = 1;
    if (g_config.loss_timeout_ms < 500) g_config.loss_timeout_ms = 500;
    if (g_config.hostname[0] == '\0') {
        snprintf(g_config.hostname, sizeof(g_config.hostname), "%s-%s",
                 CONFIG_DMX_HOSTNAME_PREFIX, s_mac_suffix);
    }
    if (g_config.name[0] == '\0') {
        snprintf(g_config.name, sizeof(g_config.name), "DMX Bridge %s", s_mac_suffix);
    }
}

esp_err_t config_init(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac_suffix, sizeof(s_mac_suffix), "%02X%02X", mac[4], mac[5]);

    set_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        stored_config_t stored;
        size_t len = sizeof(stored);
        err = nvs_get_blob(h, NVS_KEY, &stored, &len);
        nvs_close(h);
        if (err == ESP_OK && len == sizeof(stored) && stored.version == CONFIG_VERSION) {
            g_config = stored.cfg;
            g_config.wifi_ssid[sizeof(g_config.wifi_ssid) - 1] = '\0';
            g_config.wifi_pass[sizeof(g_config.wifi_pass) - 1] = '\0';
            g_config.hostname[sizeof(g_config.hostname) - 1] = '\0';
            g_config.name[sizeof(g_config.name) - 1] = '\0';
            ESP_LOGI(TAG, "loaded config from NVS");
        } else {
            ESP_LOGW(TAG, "no valid stored config, using defaults");
        }
    } else {
        ESP_LOGI(TAG, "no config namespace yet, using defaults");
    }
    sanitize();
    return ESP_OK;
}

esp_err_t config_save(void)
{
    sanitize();
    stored_config_t stored = { .version = CONFIG_VERSION, .cfg = g_config };
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "config saved: %s", esp_err_to_name(err));
    return err;
}

esp_err_t config_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
