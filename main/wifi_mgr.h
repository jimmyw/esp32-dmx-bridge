#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"

esp_err_t wifi_mgr_start(void);

bool        wifi_mgr_sta_connected(void);
bool        wifi_mgr_ap_active(void);
const char *wifi_mgr_ap_ssid(void);
int         wifi_mgr_rssi(void);

// IP of the STA interface when connected, else of the soft-AP.
esp_err_t wifi_mgr_get_ip_info(esp_netif_ip_info_t *info);
esp_err_t wifi_mgr_get_sta_ip_info(esp_netif_ip_info_t *info);

// Blocking scan; *count is in/out (capacity/result).
esp_err_t wifi_mgr_scan(wifi_ap_record_t *records, uint16_t *count);
