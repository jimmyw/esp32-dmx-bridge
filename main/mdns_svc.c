#include "mdns_svc.h"

#include <stdio.h>
#include "artnet.h"
#include "config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "sacn.h"

static const char *TAG = "mdns";
static bool s_ready;

esp_err_t mdns_svc_init(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    mdns_svc_update();
    return ESP_OK;
}

void mdns_svc_update(void)
{
    if (!s_ready) {
        return;
    }
    mdns_hostname_set(g_config.hostname);
    mdns_instance_name_set(g_config.name);
    mdns_service_remove_all();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_s[18], art_uni[8], sacn_uni[8], proto[12];
    snprintf(mac_s, sizeof(mac_s), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(art_uni, sizeof(art_uni), "%u", g_config.artnet_port_addr);
    snprintf(sacn_uni, sizeof(sacn_uni), "%u", g_config.sacn_universe);
    snprintf(proto, sizeof(proto), "%s",
             g_config.protocol == PROTO_BOTH ? "artnet,sacn" :
             g_config.protocol == PROTO_ARTNET ? "artnet" : "sacn");
    const char *fw = esp_app_get_description()->version;

    mdns_txt_item_t http_txt[] = {
        { "path", "/" },
        { "proto", proto },
        { "fw", fw },
        { "mac", mac_s },
    };
    mdns_service_add(g_config.name, "_http", "_tcp", 80, http_txt, 4);

    if (g_config.protocol & PROTO_ARTNET) {
        mdns_txt_item_t txt[] = {
            { "universe", art_uni },
            { "proto", "artnet" },
            { "fw", fw },
            { "mac", mac_s },
        };
        mdns_service_add(g_config.name, "_artnet", "_udp", ARTNET_PORT, txt, 4);
    }
    if (g_config.protocol & PROTO_SACN) {
        mdns_txt_item_t txt[] = {
            { "universe", sacn_uni },
            { "proto", "sacn" },
            { "fw", fw },
            { "mac", mac_s },
        };
        mdns_service_add(g_config.name, "_sacn", "_udp", SACN_PORT, txt, 4);
    }
    ESP_LOGI(TAG, "advertising %s.local (\"%s\")", g_config.hostname, g_config.name);
}
