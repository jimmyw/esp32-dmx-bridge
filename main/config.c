#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmx_buffer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "mdns_svc.h"
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

const char *const CONFIG_KEYS[] = {
    "wifi_ssid", "wifi_pass", "hostname", "name", "protocol", "artnet_universe",
    "sacn_universe", "tx_pin", "de_pin", "led_pin", "uart", "refresh_hz", "on_loss",
    "loss_timeout_ms", NULL,
};

static bool parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool valid_hostname(const char *h)
{
    size_t n = strlen(h);
    if (n < 1 || n > 31 || h[0] == '-' || h[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

const char *config_set_field(bridge_config_t *c, const char *key, const char *value)
{
    int v;
    if (strcmp(key, "wifi_ssid") == 0) {
        if (strlen(value) >= sizeof(c->wifi_ssid)) return "SSID too long (max 32)";
        if (strcmp(value, c->wifi_ssid) != 0) {
            strlcpy(c->wifi_ssid, value, sizeof(c->wifi_ssid));
            c->wifi_pass[0] = '\0';   // new network: password must be given again
        }
    } else if (strcmp(key, "wifi_pass") == 0) {
        size_t n = strlen(value);
        if (n != 0 && (n < 8 || n >= sizeof(c->wifi_pass))) return "Wi-Fi password must be 8-63 characters";
        strlcpy(c->wifi_pass, value, sizeof(c->wifi_pass));
    } else if (strcmp(key, "hostname") == 0) {
        if (!valid_hostname(value)) return "hostname: 1-31 chars, letters, digits and '-'";
        strlcpy(c->hostname, value, sizeof(c->hostname));
    } else if (strcmp(key, "name") == 0) {
        if (!value[0]) return "name must not be empty";
        strlcpy(c->name, value, sizeof(c->name));
    } else if (strcmp(key, "protocol") == 0) {
        if (strcmp(value, "artnet") == 0) c->protocol = PROTO_ARTNET;
        else if (strcmp(value, "sacn") == 0) c->protocol = PROTO_SACN;
        else if (strcmp(value, "both") == 0) c->protocol = PROTO_BOTH;
        else if (parse_int(value, PROTO_ARTNET, PROTO_BOTH, &v)) c->protocol = v;
        else return "protocol: artnet, sacn or both";
    } else if (strcmp(key, "artnet_universe") == 0) {
        if (!parse_int(value, 0, 32767, &v)) return "Art-Net universe must be 0-32767";
        c->artnet_port_addr = v;
    } else if (strcmp(key, "sacn_universe") == 0) {
        if (!parse_int(value, 1, 63999, &v)) return "sACN universe must be 1-63999";
        c->sacn_universe = v;
    } else if (strcmp(key, "tx_pin") == 0) {
        if (!parse_int(value, 0, 48, &v) || !config_pin_valid(v, false)) return "invalid TX pin";
        c->tx_pin = v;
    } else if (strcmp(key, "de_pin") == 0) {
        if (!parse_int(value, -1, 48, &v) || !config_pin_valid(v, true)) return "invalid DE pin (-1 = none)";
        c->de_pin = v;
    } else if (strcmp(key, "led_pin") == 0) {
        if (!parse_int(value, -1, 48, &v) || !config_pin_valid(v, true)) return "invalid LED pin (-1 = none)";
        c->led_pin = v;
    } else if (strcmp(key, "uart") == 0) {
        if (!parse_int(value, 1, 2, &v)) return "UART must be 1 or 2";
        c->uart_num = v;
    } else if (strcmp(key, "refresh_hz") == 0) {
        if (!parse_int(value, 1, 44, &v)) return "refresh must be 1-44 Hz";
        c->refresh_hz = v;
    } else if (strcmp(key, "on_loss") == 0) {
        if (strcmp(value, "hold") == 0 || strcmp(value, "0") == 0) c->on_loss = LOSS_HOLD;
        else if (strcmp(value, "blackout") == 0 || strcmp(value, "1") == 0) c->on_loss = LOSS_BLACKOUT;
        else return "on_loss: hold or blackout";
    } else if (strcmp(key, "loss_timeout_ms") == 0) {
        if (!parse_int(value, 500, 60000, &v)) return "loss timeout must be 500-60000 ms";
        c->loss_timeout_ms = v;
    } else {
        return "unknown setting";
    }
    return NULL;
}

const char *config_commit(const bridge_config_t *c, bool *reboot)
{
    if (c->tx_pin == c->de_pin ||
        (c->led_pin >= 0 && (c->led_pin == c->tx_pin || c->led_pin == c->de_pin))) {
        return "pins must be different";
    }
    bool need_reboot = strcmp(c->wifi_ssid, g_config.wifi_ssid) != 0 ||
                       strcmp(c->wifi_pass, g_config.wifi_pass) != 0 ||
                       strcmp(c->hostname, g_config.hostname) != 0 ||
                       c->tx_pin != g_config.tx_pin || c->de_pin != g_config.de_pin ||
                       c->led_pin != g_config.led_pin || c->uart_num != g_config.uart_num;
    bool universe_changed = c->artnet_port_addr != g_config.artnet_port_addr ||
                            c->sacn_universe != g_config.sacn_universe ||
                            c->protocol != g_config.protocol;

    g_config = *c;
    if (config_save() != ESP_OK) {
        return "failed to save to flash";
    }
    if (universe_changed) {
        dmx_buffer_reset_sources();   // sACN task re-joins multicast on its own
    }
    mdns_svc_update();
    if (reboot) {
        *reboot = need_reboot;
    }
    return NULL;
}
