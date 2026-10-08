#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "config.h"
#include "dmx_buffer.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns_svc.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static void restart_cb(void *arg)
{
    esp_restart();
}

static void schedule_restart(int ms)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
        esp_timer_create(&args, &t);
    }
    esp_timer_stop(t);
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *msg)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "error", msg);
    httpd_resp_set_status(req, "400 Bad Request");
    return send_json(req, r);
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // EMBED_TXTFILES appends a NUL terminator
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static const char *src_name(uint8_t t)
{
    return t == SRC_ARTNET ? "artnet" : t == SRC_SACN ? "sacn" : "none";
}

static esp_err_t status_get(httpd_req_t *req)
{
    static int64_t last_us;
    static uint32_t last_art, last_sacn, last_frames;
    static float art_rate, sacn_rate, fps;

    dmx_stats_t st;
    dmx_buffer_get_stats(&st);
    int64_t now = esp_timer_get_time();
    float dt = (now - last_us) / 1e6f;
    if (last_us && dt > 0.2f) {
        art_rate = (st.artnet_packets - last_art) / dt;
        sacn_rate = (st.sacn_packets - last_sacn) / dt;
        fps = (st.frames_sent - last_frames) / dt;
    }
    if (!last_us || dt > 0.2f) {
        last_us = now;
        last_art = st.artnet_packets;
        last_sacn = st.sacn_packets;
        last_frames = st.frames_sent;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "fw", esp_app_get_description()->version);
    cJSON_AddStringToObject(r, "name", g_config.name);
    cJSON_AddStringToObject(r, "hostname", g_config.hostname);
    cJSON_AddNumberToObject(r, "uptime", (double)(now / 1000000));
    cJSON_AddNumberToObject(r, "heap", esp_get_free_heap_size());
    const esp_partition_t *running = esp_ota_get_running_partition();
    cJSON_AddStringToObject(r, "partition", running ? running->label : "");

    cJSON *w = cJSON_AddObjectToObject(r, "wifi");
    cJSON_AddBoolToObject(w, "sta", wifi_mgr_sta_connected());
    cJSON_AddBoolToObject(w, "ap", wifi_mgr_ap_active());
    cJSON_AddStringToObject(w, "ssid", g_config.wifi_ssid);
    cJSON_AddStringToObject(w, "ap_ssid", wifi_mgr_ap_ssid());
    cJSON_AddNumberToObject(w, "rssi", wifi_mgr_rssi());
    esp_netif_ip_info_t ip;
    char ipbuf[16] = "";
    if (wifi_mgr_get_sta_ip_info(&ip) == ESP_OK) {
        snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ip.ip));
    }
    cJSON_AddStringToObject(w, "ip", ipbuf);

    cJSON *d = cJSON_AddObjectToObject(r, "dmx");
    cJSON_AddBoolToObject(d, "signal", st.signal);
    cJSON_AddStringToObject(d, "source", src_name(st.active_type));
    char srcip[16] = "";
    if (st.active_ip) {
        esp_ip4_addr_t a = { .addr = st.active_ip };
        snprintf(srcip, sizeof(srcip), IPSTR, IP2STR(&a));
    }
    cJSON_AddStringToObject(d, "source_ip", srcip);
    cJSON_AddNumberToObject(d, "priority", st.active_priority);
    cJSON_AddNumberToObject(d, "sources", st.num_sources);
    cJSON_AddNumberToObject(d, "artnet_pps", (int)(art_rate + 0.5f));
    cJSON_AddNumberToObject(d, "sacn_pps", (int)(sacn_rate + 0.5f));
    cJSON_AddNumberToObject(d, "fps", (int)(fps + 0.5f));
    cJSON_AddNumberToObject(d, "artnet_total", st.artnet_packets);
    cJSON_AddNumberToObject(d, "sacn_total", st.sacn_packets);

    static uint8_t slots[DMX_SLOTS];
    static char hex[DMX_SLOTS * 2 + 1];
    dmx_buffer_peek(slots, DMX_SLOTS);
    for (int i = 0; i < DMX_SLOTS; i++) {
        snprintf(&hex[i * 2], 3, "%02x", slots[i]);
    }
    cJSON_AddStringToObject(d, "values", hex);
    return send_json(req, r);
}

static esp_err_t config_get(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "wifi_ssid", g_config.wifi_ssid);
    cJSON_AddBoolToObject(r, "wifi_pass_set", g_config.wifi_pass[0] != '\0');
    cJSON_AddStringToObject(r, "hostname", g_config.hostname);
    cJSON_AddStringToObject(r, "name", g_config.name);
    cJSON_AddNumberToObject(r, "protocol", g_config.protocol);
    cJSON_AddNumberToObject(r, "artnet_universe", g_config.artnet_port_addr);
    cJSON_AddNumberToObject(r, "sacn_universe", g_config.sacn_universe);
    cJSON_AddNumberToObject(r, "tx_pin", g_config.tx_pin);
    cJSON_AddNumberToObject(r, "de_pin", g_config.de_pin);
    cJSON_AddNumberToObject(r, "led_pin", g_config.led_pin);
    cJSON_AddNumberToObject(r, "uart", g_config.uart_num);
    cJSON_AddNumberToObject(r, "refresh_hz", g_config.refresh_hz);
    cJSON_AddNumberToObject(r, "on_loss", g_config.on_loss);
    cJSON_AddNumberToObject(r, "loss_timeout_ms", g_config.loss_timeout_ms);
    return send_json(req, r);
}

static bool get_int(cJSON *root, const char *key, int *out)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(it)) {
        *out = it->valueint;
        return true;
    }
    if (cJSON_IsString(it) && it->valuestring[0]) {
        *out = atoi(it->valuestring);
        return true;
    }
    return false;
}

static const char *get_str(cJSON *root, const char *key)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
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

static esp_err_t config_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048) {
        return send_error(req, "bad body length");
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return send_error(req, "invalid JSON");
    }

    bridge_config_t c = g_config;
    const char *err = NULL;
    const char *s;
    int v;

    if ((s = get_str(root, "wifi_ssid"))) {
        if (strlen(s) >= sizeof(c.wifi_ssid)) {
            err = "SSID too long";
        } else if (strcmp(s, c.wifi_ssid) != 0) {
            strlcpy(c.wifi_ssid, s, sizeof(c.wifi_ssid));
            c.wifi_pass[0] = '\0';   // new network: password must be given again
        }
    }
    if ((s = get_str(root, "wifi_pass")) && s[0]) {
        size_t n = strlen(s);
        if (n < 8 || n >= sizeof(c.wifi_pass)) {
            err = "Wi-Fi password must be 8-63 characters";
        } else {
            strlcpy(c.wifi_pass, s, sizeof(c.wifi_pass));
        }
    }
    if ((s = get_str(root, "hostname"))) {
        if (!valid_hostname(s)) {
            err = "hostname: 1-31 chars, letters, digits and '-'";
        } else {
            strlcpy(c.hostname, s, sizeof(c.hostname));
        }
    }
    if ((s = get_str(root, "name")) && s[0]) {
        strlcpy(c.name, s, sizeof(c.name));
    }
    if (get_int(root, "protocol", &v)) {
        if (v < PROTO_ARTNET || v > PROTO_BOTH) err = "invalid protocol";
        else c.protocol = v;
    }
    if (get_int(root, "artnet_universe", &v)) {
        if (v < 0 || v > 32767) err = "Art-Net universe must be 0-32767";
        else c.artnet_port_addr = v;
    }
    if (get_int(root, "sacn_universe", &v)) {
        if (v < 1 || v > 63999) err = "sACN universe must be 1-63999";
        else c.sacn_universe = v;
    }
    if (get_int(root, "tx_pin", &v)) {
        if (!config_pin_valid(v, false)) err = "invalid TX pin";
        else c.tx_pin = v;
    }
    if (get_int(root, "de_pin", &v)) {
        if (!config_pin_valid(v, true)) err = "invalid DE pin";
        else c.de_pin = v;
    }
    if (get_int(root, "led_pin", &v)) {
        if (!config_pin_valid(v, true)) err = "invalid LED pin";
        else c.led_pin = v;
    }
    if (get_int(root, "uart", &v)) {
        if (v < 1 || v > 2) err = "UART must be 1 or 2";
        else c.uart_num = v;
    }
    if (get_int(root, "refresh_hz", &v)) {
        if (v < 1 || v > 44) err = "refresh must be 1-44 Hz";
        else c.refresh_hz = v;
    }
    if (get_int(root, "on_loss", &v)) {
        c.on_loss = v ? LOSS_BLACKOUT : LOSS_HOLD;
    }
    if (get_int(root, "loss_timeout_ms", &v)) {
        if (v < 500 || v > 60000) err = "loss timeout must be 500-60000 ms";
        else c.loss_timeout_ms = v;
    }
    cJSON_Delete(root);

    if (!err && (c.tx_pin == c.de_pin || (c.led_pin >= 0 && (c.led_pin == c.tx_pin || c.led_pin == c.de_pin)))) {
        err = "pins must be different";
    }
    if (err) {
        return send_error(req, err);
    }

    bool reboot = strcmp(c.wifi_ssid, g_config.wifi_ssid) != 0 ||
                  strcmp(c.wifi_pass, g_config.wifi_pass) != 0 ||
                  strcmp(c.hostname, g_config.hostname) != 0 ||
                  c.tx_pin != g_config.tx_pin || c.de_pin != g_config.de_pin ||
                  c.led_pin != g_config.led_pin || c.uart_num != g_config.uart_num;
    bool universe_changed = c.artnet_port_addr != g_config.artnet_port_addr ||
                            c.sacn_universe != g_config.sacn_universe ||
                            c.protocol != g_config.protocol;

    g_config = c;
    esp_err_t serr = config_save();
    if (serr != ESP_OK) {
        return send_error(req, "failed to save to flash");
    }
    if (universe_changed) {
        dmx_buffer_reset_sources();   // sACN task re-joins multicast on its own
    }
    mdns_svc_update();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "reboot", reboot);
    esp_err_t res = send_json(req, r);
    if (reboot) {
        ESP_LOGI(TAG, "config needs reboot, restarting");
        schedule_restart(1500);
    }
    return res;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    uint16_t n = 24;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    if (!recs) {
        return httpd_resp_send_500(req);
    }
    esp_err_t err = wifi_mgr_scan(recs, &n);
    cJSON *r = cJSON_CreateArray();
    if (err == ESP_OK) {
        for (int i = 0; i < n; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            if (!ssid[0]) {
                continue;
            }
            bool dup = false;   // records are sorted by RSSI: keep the strongest per SSID
            for (int j = 0; j < i; j++) {
                if (strcmp(ssid, (const char *)recs[j].ssid) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", ssid);
            cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
            cJSON_AddNumberToObject(o, "ch", recs[i].primary);
            cJSON_AddBoolToObject(o, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(r, o);
        }
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }
    free(recs);
    return send_json(req, r);
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    esp_err_t res = send_json(req, r);
    schedule_restart(1000);
    return res;
}

static esp_err_t factory_reset_post(httpd_req_t *req)
{
    config_factory_reset();
    return reboot_post(req);
}

// Firmware upload: the request body is the raw dmx_bridge.bin.
static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return send_error(req, "no OTA partition");
    }
    if (req->content_len <= 0 || req->content_len > part->size) {
        return send_error(req, "firmware size invalid or larger than the app partition");
    }
    ESP_LOGI(TAG, "OTA: %d bytes -> %s", req->content_len, part->label);

    char *buf = malloc(4096);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    esp_ota_handle_t ota = 0;
    const char *err = NULL;
    int remaining = req->content_len;
    bool started = false;

    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            err = "upload interrupted";
            break;
        }
        if (!started) {
            // First chunk: check this is an app image for this chip before erasing anything.
            const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            if (n < desc_off + sizeof(esp_app_desc_t)) {
                err = "file too small";
                break;
            }
            const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
            const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + desc_off);
            if (hdr->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                err = "not an ESP32 application image (use build/dmx_bridge.bin)";
                break;
            }
            if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
                err = "firmware is built for a different chip";
                break;
            }
            ESP_LOGI(TAG, "OTA: image \"%s\" version %s", desc->project_name, desc->version);
            if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
                err = "esp_ota_begin failed";
                break;
            }
            started = true;
        }
        if (esp_ota_write(ota, buf, n) != ESP_OK) {
            err = "flash write failed";
            break;
        }
        remaining -= n;
    }
    free(buf);

    if (started) {
        if (err) {
            esp_ota_abort(ota);
        } else if (esp_ota_end(ota) != ESP_OK) {
            err = "image verification failed";
        } else if (esp_ota_set_boot_partition(part) != ESP_OK) {
            err = "could not set boot partition";
        }
    }
    if (err) {
        ESP_LOGW(TAG, "OTA failed: %s", err);
        return send_error(req, err);
    }
    ESP_LOGI(TAG, "OTA done, restarting into %s", part->label);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    esp_err_t res = send_json(req, r);
    schedule_restart(1000);
    return res;
}

// Anything else (captive-portal probes such as /generate_204, /hotspot-detect.html) -> portal.
static esp_err_t redirect_get(httpd_req_t *req)
{
    char loc[48] = "/";
    esp_netif_ip_info_t ip;
    if (wifi_mgr_ap_active() && !wifi_mgr_sta_connected() && wifi_mgr_get_ip_info(&ip) == ESP_OK) {
        snprintf(loc, sizeof(loc), "http://" IPSTR "/", IP2STR(&ip.ip));
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.core_id = 0;
    cfg.recv_wait_timeout = 15;

    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",                  .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/status",        .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/config",        .method = HTTP_GET,  .handler = config_get },
        { .uri = "/api/config",        .method = HTTP_POST, .handler = config_post },
        { .uri = "/api/scan",          .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/reboot",        .method = HTTP_POST, .handler = reboot_post },
        { .uri = "/api/factory_reset", .method = HTTP_POST, .handler = factory_reset_post },
        { .uri = "/api/ota",           .method = HTTP_POST, .handler = ota_post },
        { .uri = "/*",                 .method = HTTP_GET,  .handler = redirect_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    ESP_LOGI(TAG, "web UI on port 80");
    return ESP_OK;
}
