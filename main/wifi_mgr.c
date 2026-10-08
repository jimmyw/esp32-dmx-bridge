#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>
#include "captive_dns.h"
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns_svc.h"
#include "sacn.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define AP_FALLBACK_S     20   // start provisioning AP after this long without STA link
#define STA_RETRY_S       5    // retry interval while the AP is off
#define STA_RETRY_AP_S    30   // retry interval while the AP is on (retries disturb AP clients)
#define AP_LINGER_S       15   // keep AP this long after STA got an IP (and while clients remain)

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static SemaphoreHandle_t s_scan_lock;
static char s_ap_ssid[33];

static volatile bool s_has_creds;
static volatile bool s_sta_connected;
static volatile bool s_connecting;
static volatile bool s_ap_active;
static volatile bool s_scanning;
static volatile int  s_ap_clients;

bool wifi_mgr_sta_connected(void) { return s_sta_connected; }
bool wifi_mgr_ap_active(void) { return s_ap_active; }
const char *wifi_mgr_ap_ssid(void) { return s_ap_ssid; }

int wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_sta_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

esp_err_t wifi_mgr_get_sta_ip_info(esp_netif_ip_info_t *info)
{
    if (!s_sta_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_get_ip_info(s_sta_netif, info);
}

esp_err_t wifi_mgr_get_ip_info(esp_netif_ip_info_t *info)
{
    if (s_sta_connected) {
        return esp_netif_get_ip_info(s_sta_netif, info);
    }
    if (s_ap_active) {
        return esp_netif_get_ip_info(s_ap_netif, info);
    }
    return ESP_ERR_INVALID_STATE;
}

static void sta_connect(void)
{
    if (!s_has_creds || s_connecting || s_sta_connected || s_scanning) {
        return;
    }
    ESP_LOGI(TAG, "connecting to \"%s\"", g_config.wifi_ssid);
    s_connecting = true;
    if (esp_wifi_connect() != ESP_OK) {
        s_connecting = false;
    }
}

static void set_ap(bool on)
{
    if (on == s_ap_active) {
        return;
    }
    if (on) {
        ESP_LOGI(TAG, "starting provisioning AP \"%s\"", s_ap_ssid);
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        s_ap_active = true;
        captive_dns_start();
    } else {
        ESP_LOGI(TAG, "stopping provisioning AP");
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_ap_active = false;
        s_ap_clients = 0;
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            sta_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = data;
            if (s_sta_connected || s_connecting) {
                ESP_LOGW(TAG, "STA disconnected (reason %d)", e->reason);
            }
            s_sta_connected = false;
            s_connecting = false;
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            s_ap_clients++;
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_clients > 0) {
                s_ap_clients--;
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_sta_connected = true;
        s_connecting = false;
        sacn_rejoin();
    }
}

static void supervisor_task(void *arg)
{
    int disconnected_s = 0;
    int connected_s = 0;
    int since_attempt_s = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!s_has_creds) {
            set_ap(true);
            continue;
        }

        if (s_sta_connected) {
            disconnected_s = 0;
            connected_s++;
            if (s_ap_active && connected_s >= AP_LINGER_S && s_ap_clients == 0) {
                set_ap(false);
            }
            continue;
        }

        connected_s = 0;
        disconnected_s++;
        since_attempt_s++;
        if (!s_ap_active && disconnected_s >= AP_FALLBACK_S) {
            set_ap(true);
        }
        int interval = s_ap_active ? STA_RETRY_AP_S : STA_RETRY_S;
        // Connection attempts hop channels, which kicks phones off the AP: wait while one is on it.
        bool ap_busy = s_ap_active && s_ap_clients > 0;
        if (!s_connecting && !ap_busy && since_attempt_s >= interval) {
            since_attempt_s = 0;
            sta_connect();
        }
    }
}

esp_err_t wifi_mgr_scan(wifi_ap_record_t *records, uint16_t *count)
{
    if (xSemaphoreTake(s_scan_lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_scanning = true;
    if (s_connecting) {
        // A pending connect attempt blocks scanning; abort it, the supervisor retries later.
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        s_connecting = false;
    }
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(count, records);
    } else {
        *count = 0;
    }
    s_scanning = false;
    xSemaphoreGive(s_scan_lock);
    return err;
}

esp_err_t wifi_mgr_start(void)
{
    s_scan_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, g_config.hostname);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   // creds live in our own config
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));

    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "DMX-Bridge-%s", config_mac_suffix());
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    const char *ap_pass = CONFIG_DMX_AP_PASSWORD;
    if (strlen(ap_pass) >= 8) {
        strlcpy((char *)ap.ap.password, ap_pass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    s_has_creds = g_config.wifi_ssid[0] != '\0';
    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, g_config.wifi_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, g_config.wifi_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = g_config.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    // Configure both interfaces up front; mode decides which is live.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    if (s_has_creds) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    } else {
        s_ap_active = true;
        captive_dns_start();
        ESP_LOGI(TAG, "no Wi-Fi configured: provisioning AP \"%s\" (pass \"%s\") at 192.168.4.1",
                 s_ap_ssid, ap.ap.authmode == WIFI_AUTH_OPEN ? "" : ap_pass);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep adds tens of ms of latency and jitter to incoming DMX: keep the radio awake.
    esp_wifi_set_ps(WIFI_PS_NONE);

    mdns_svc_init();
    xTaskCreate(supervisor_task, "wifi_sup", 3072, NULL, 5, NULL);
    return ESP_OK;
}
