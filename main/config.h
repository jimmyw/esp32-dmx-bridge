#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    PROTO_ARTNET = 1,
    PROTO_SACN   = 2,
    PROTO_BOTH   = 3,
} dmx_protocol_t;

typedef enum {
    LOSS_HOLD     = 0,
    LOSS_BLACKOUT = 1,
} dmx_loss_t;

typedef struct {
    char     wifi_ssid[33];
    char     wifi_pass[65];
    char     hostname[32];       // mDNS / DHCP hostname
    char     name[64];           // friendly name (Art-Net long name, mDNS instance)
    uint8_t  protocol;           // dmx_protocol_t bitmask
    uint16_t artnet_port_addr;   // 15-bit: net(7) | subnet(4) | universe(4)
    uint16_t sacn_universe;      // 1..63999
    int8_t   tx_pin;
    int8_t   de_pin;             // -1 = none (auto-direction transceiver)
    int8_t   led_pin;            // -1 = none
    uint8_t  uart_num;
    uint8_t  refresh_hz;         // DMX frames per second
    uint8_t  on_loss;            // dmx_loss_t
    uint16_t loss_timeout_ms;
} bridge_config_t;

// Global, live configuration. Readers may access fields directly; writers use config_save().
extern bridge_config_t g_config;

esp_err_t config_init(void);           // load from NVS (or defaults)
esp_err_t config_save(void);           // persist g_config
esp_err_t config_factory_reset(void);  // erase NVS namespace
bool      config_pin_valid(int pin, bool allow_none);
const char *config_mac_suffix(void);   // "A1B2" (last 2 MAC bytes)

// Set one field of `c` from its text form (shared by the web API and the serial CLI).
// Keys: wifi_ssid wifi_pass hostname name protocol artnet_universe sacn_universe tx_pin
// de_pin led_pin uart refresh_hz on_loss loss_timeout_ms. Returns NULL or an error message.
// Changing wifi_ssid clears wifi_pass, so set the SSID first.
const char *config_set_field(bridge_config_t *c, const char *key, const char *value);

// Validate `c`, make it the live config, persist it and apply what can change at runtime.
// *reboot is set when Wi-Fi, hostname or hardware settings changed. Returns NULL or an error.
const char *config_commit(const bridge_config_t *c, bool *reboot);

extern const char *const CONFIG_KEYS[];
