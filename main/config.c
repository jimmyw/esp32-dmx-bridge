#include "config.h"

#include <stddef.h>
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
#define CONFIG_VERSION 3
// Each version appended fields: v1 ended at loss_timeout_ms, v2 added the microphone, v3 the beat
// offset. An older blob is a prefix (plus padding) of the current struct.
static const size_t s_version_size[CONFIG_VERSION + 1] = {
    0, offsetof(bridge_config_t, mic_sck), offsetof(bridge_config_t, beat_offset_ms), sizeof(bridge_config_t),
};

typedef struct {
    uint32_t        version;
    bridge_config_t cfg;
} stored_config_t;

static bool blob_is_version(size_t len, uint32_t version)
{
    if (version < 1 || version > CONFIG_VERSION) return false;
    size_t want = offsetof(stored_config_t, cfg) + s_version_size[version];
    return len >= want && len <= ((want + 3) & ~(size_t)3);   // allow the struct's tail padding
}

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
    g_config.mic_sck          = CONFIG_DMX_MIC_SCK_PIN;
    g_config.mic_ws           = CONFIG_DMX_MIC_WS_PIN;
    g_config.mic_sd           = CONFIG_DMX_MIC_SD_PIN;
    g_config.mic_gate         = 75;
    g_config.beat_sens        = 14;
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
    if (!config_pin_valid(g_config.mic_sck, true)) g_config.mic_sck = -1;
    if (!config_pin_valid(g_config.mic_ws, true))  g_config.mic_ws = -1;
    if (!config_pin_valid(g_config.mic_sd, true))  g_config.mic_sd = -1;
    if (g_config.mic_gate < 20 || g_config.mic_gate > 100) g_config.mic_gate = 75;
    if (g_config.beat_sens < 10 || g_config.beat_sens > 40) g_config.beat_sens = 14;
    if (g_config.beat_offset_ms < -300 || g_config.beat_offset_ms > 300) g_config.beat_offset_ms = 0;
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
        // Prefilled with defaults: an older (shorter) version only overwrites its own fields.
        stored_config_t stored = { .cfg = g_config };
        size_t len = sizeof(stored);
        err = nvs_get_blob(h, NVS_KEY, &stored, &len);
        nvs_close(h);
        if (err == ESP_OK && blob_is_version(len, stored.version)) {
            if (stored.version != CONFIG_VERSION) {
                ESP_LOGI(TAG, "config v%lu -> v%d (new settings get defaults)", (unsigned long)stored.version,
                         CONFIG_VERSION);
            }
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
    "loss_timeout_ms", "mic_sck", "mic_ws", "mic_sd", "mic_gate", "beat_sens", "beat_offset_ms", NULL,
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
    } else if (strcmp(key, "mic_sck") == 0 || strcmp(key, "mic_ws") == 0 || strcmp(key, "mic_sd") == 0) {
        if (!parse_int(value, -1, 48, &v) || !config_pin_valid(v, true)) return "invalid microphone pin (-1 = none)";
        int8_t *pin = strcmp(key, "mic_sck") == 0 ? &c->mic_sck : strcmp(key, "mic_ws") == 0 ? &c->mic_ws : &c->mic_sd;
        *pin = v;
    } else if (strcmp(key, "mic_gate") == 0) {
        if (!parse_int(value, 20, 100, &v)) return "noise gate must be 20-100 (dB below full scale)";
        c->mic_gate = v;
    } else if (strcmp(key, "beat_sens") == 0) {
        if (!parse_int(value, 10, 40, &v)) return "beat sensitivity must be 10-40";
        c->beat_sens = v;
    } else if (strcmp(key, "beat_offset_ms") == 0) {
        if (!parse_int(value, -300, 300, &v)) return "beat offset must be -300..300 ms";
        c->beat_offset_ms = v;
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
    // Every assigned pin (DE/LED/mic may be -1 = none) must be distinct.
    const int pins[] = { c->tx_pin, c->de_pin, c->led_pin, c->mic_sck, c->mic_ws, c->mic_sd };
    const int n = sizeof(pins) / sizeof(pins[0]);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (pins[i] >= 0 && pins[i] == pins[j]) {
                return "pins must be different";
            }
        }
    }
    int mics = (c->mic_sck >= 0) + (c->mic_ws >= 0) + (c->mic_sd >= 0);
    if (mics != 0 && mics != 3) {
        return "microphone needs all three pins (or all -1)";
    }
    bool need_reboot = strcmp(c->wifi_ssid, g_config.wifi_ssid) != 0 ||
                       strcmp(c->wifi_pass, g_config.wifi_pass) != 0 ||
                       strcmp(c->hostname, g_config.hostname) != 0 ||
                       c->tx_pin != g_config.tx_pin || c->de_pin != g_config.de_pin ||
                       c->led_pin != g_config.led_pin || c->uart_num != g_config.uart_num ||
                       c->mic_sck != g_config.mic_sck || c->mic_ws != g_config.mic_ws ||
                       c->mic_sd != g_config.mic_sd;
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
