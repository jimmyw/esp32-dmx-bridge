#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "dmx_buffer.h"
#include "esp_log.h"
#include "names.h"
#include "scenes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "console";

/*
 * WebSocket protocol (binary):
 *   client -> server: repeated 3-byte records [ch_hi, ch_lo, value]; ch 0..511, 0xFFFF = master.
 *                     text "clear" zeroes all console faders.
 *   server -> client: every PUSH_MS: [0x01][master][manual x512][output x512]
 *                     text "names" / "scenes" when those changed (clients re-fetch the API).
 */
#define PUSH_MS     66
#define MASTER_CH   0xFFFF
#define MAX_CLIENTS 16
#define IDLE_PUSHES 50   // keep full-rate pushing ~3 s after the last client left
#define IDLE_SCAN   8    // idle: look for new clients every ~0.5 s
#define STALL_LIMIT 75   // close a client whose socket stays full this many pushes (~5 s)

extern const char console_html_start[] asm("_binary_console_html_start");
extern const char console_html_end[] asm("_binary_console_html_end");

static httpd_handle_t s_server;
static volatile int s_idle_left;
static struct { int fd; int stalled; } s_stall[MAX_CLIENTS];

// A sleeping phone stops ACKing; a blocking send would then stall the whole web server.
static bool writable(int fd)
{
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = { 0 };
    return select(fd + 1, NULL, &w, NULL, &tv) > 0;
}

static int *stall_counter(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_stall[i].fd == fd && s_stall[i].stalled > 0) {
            return &s_stall[i].stalled;
        }
    }
    // Slots with a zero count carry no state, so any of them can be (re)used.
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_stall[i].stalled == 0) {
            s_stall[i].fd = fd;
            return &s_stall[i].stalled;
        }
    }
    return NULL;
}

static esp_err_t console_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, console_html_start, console_html_end - console_html_start - 1);
}

static void apply_records(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i + 3 <= len; i += 3) {
        uint16_t ch = (p[i] << 8) | p[i + 1];
        if (ch == MASTER_CH) {
            dmx_buffer_set_manual_master(p[i + 2]);
        } else {
            scenes_release_channel(ch);   // a hand on the fader wins over a running fade
            dmx_buffer_set_manual(ch, p[i + 2]);
        }
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "console client connected (fd %d)", httpd_req_to_sockfd(req));
        s_idle_left = IDLE_PUSHES;
        return ESP_OK;
    }
    static uint8_t buf[1536];
    httpd_ws_frame_t f = { .payload = buf };
    esp_err_t err = httpd_ws_recv_frame(req, &f, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }
    if (f.type == HTTPD_WS_TYPE_BINARY) {
        apply_records(buf, f.len);
    } else if (f.type == HTTPD_WS_TYPE_TEXT && f.len == 5 && memcmp(buf, "clear", 5) == 0) {
        scenes_stop_fade();
        dmx_buffer_clear_manual();
    }
    return ESP_OK;
}

// Runs in the httpd task (queued), so socket access is serialized with request handling.
static void push_work(void *arg)
{
    static uint8_t frame[2 + 2 * DMX_SLOTS];
    size_t fds_n = MAX_CLIENTS;
    int fds[MAX_CLIENTS];
    if (httpd_get_client_list(s_server, &fds_n, fds) != ESP_OK) {
        return;
    }
    frame[0] = 0x01;
    dmx_buffer_get_manual(&frame[2], &frame[1]);
    dmx_buffer_peek(&frame[2 + DMX_SLOTS], DMX_SLOTS);
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_BINARY, .payload = frame, .len = sizeof(frame), .final = true };

    bool any = false;
    for (size_t i = 0; i < fds_n; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            any = true;
            int *stalled = stall_counter(fds[i]);
            if (writable(fds[i])) {
                if (stalled) *stalled = 0;
                httpd_ws_send_frame_async(s_server, fds[i], &f);
            } else if (stalled && ++*stalled >= STALL_LIMIT) {
                ESP_LOGW(TAG, "closing unresponsive console client (fd %d)", fds[i]);
                *stalled = 0;
                httpd_sess_trigger_close(s_server, fds[i]);
            }
        }
    }
    if (any) {
        s_idle_left = IDLE_PUSHES;
    } else if (s_idle_left > 0) {
        s_idle_left--;
    }
}

// arg: static string to broadcast as a text frame
static void broadcast_text_work(void *arg)
{
    const char *msg = arg;
    size_t fds_n = MAX_CLIENTS;
    int fds[MAX_CLIENTS];
    if (httpd_get_client_list(s_server, &fds_n, fds) != ESP_OK) {
        return;
    }
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)msg, .len = strlen(msg), .final = true };
    for (size_t i = 0; i < fds_n; i++) {
        if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET && writable(fds[i])) {
            httpd_ws_send_frame_async(s_server, fds[i], &f);
        }
    }
}

void console_names_changed(void)
{
    if (s_server) {
        httpd_queue_work(s_server, broadcast_text_work, (void *)"names");
    }
}

static void scenes_changed(void)
{
    if (s_server) {
        httpd_queue_work(s_server, broadcast_text_work, (void *)"scenes");
    }
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *js = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!js) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, js);
    free(js);
    return err;
}

static cJSON *read_json_body(httpd_req_t *req, int max_len)
{
    if (req->content_len <= 0 || req->content_len > max_len) {
        return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        return NULL;
    }
    int got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            free(body);
            return NULL;
        }
        got += n;
    }
    body[got] = '\0';
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static esp_err_t json_result(httpd_req_t *req, const char *err)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", err == NULL);
    if (err) {
        cJSON_AddStringToObject(r, "error", err);
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return send_json(req, r);
}

// GET /api/names -> {"max_len":24,"names":{"1":"Front wash",...}}  (1-based channel numbers)
static esp_err_t names_get_handler(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "max_len", NAME_MAX_LEN);
    cJSON *n = cJSON_AddObjectToObject(r, "names");
    char key[8];
    for (int i = 0; i < DMX_SLOTS; i++) {
        const char *nm = names_get(i);
        if (nm[0]) {
            snprintf(key, sizeof(key), "%d", i + 1);
            cJSON_AddStringToObject(n, key, nm);
        }
    }
    return send_json(req, r);
}

// POST /api/names {"1":"Front wash","2":""}  ("" removes a name), or {"clear":true}
static esp_err_t names_post_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req, 24 * 1024);
    if (!root) {
        return json_result(req, "invalid JSON body");
    }
    int changed = 0;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "clear"))) {
        names_clear_all();
        changed++;
    }
    cJSON *it;
    cJSON_ArrayForEach(it, root) {
        char *end;
        long ch = strtol(it->string, &end, 10);
        if (*end == '\0' && ch >= 1 && ch <= DMX_SLOTS && cJSON_IsString(it)) {
            names_set(ch - 1, it->valuestring);
            changed++;
        }
    }
    cJSON_Delete(root);
    if (changed) {
        names_save_soon();
        console_names_changed();
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "changed", changed);
    return send_json(req, r);
}

// GET /api/scenes -> {"count":64,"active":3,"fading":false,"scenes":[{"id":1,"name":"Intro"},...]}
static esp_err_t scenes_get_handler(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "count", SCENE_COUNT);
    cJSON_AddNumberToObject(r, "active", scenes_active() + 1);   // 0 = none
    cJSON_AddBoolToObject(r, "fading", scenes_fading());
    cJSON *arr = cJSON_AddArrayToObject(r, "scenes");
    for (int i = 0; i < SCENE_COUNT; i++) {
        if (scenes_used(i)) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "id", i + 1);
            cJSON_AddStringToObject(o, "name", scenes_name(i));
            cJSON_AddItemToArray(arr, o);
        }
    }
    return send_json(req, r);
}

// POST /api/scenes {"action":"recall|save|rename|delete","id":1..64,"name":"..","fade_ms":2000}
static esp_err_t scenes_post_handler(httpd_req_t *req)
{
    cJSON *root = read_json_body(req, 1024);
    if (!root) {
        return json_result(req, "invalid JSON body");
    }
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON *idj = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *namej = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *fadej = cJSON_GetObjectItemCaseSensitive(root, "fade_ms");
    const char *name = cJSON_IsString(namej) ? namej->valuestring : "";
    int id = cJSON_IsNumber(idj) ? idj->valueint - 1 : -1;
    const char *err = NULL;

    if (!cJSON_IsString(action)) {
        err = "missing action";
    } else if (id < 0 || id >= SCENE_COUNT) {
        err = "id must be 1-64";
    } else if (strcmp(action->valuestring, "recall") == 0) {
        int fade = cJSON_IsNumber(fadej) ? fadej->valueint : 0;
        if (fade < 0) fade = 0;
        if (scenes_recall(id, fade) != ESP_OK) err = "scene is empty";
    } else if (strcmp(action->valuestring, "save") == 0) {
        if (scenes_save(id, name) != ESP_OK) err = "save failed";
    } else if (strcmp(action->valuestring, "rename") == 0) {
        if (scenes_rename(id, name) != ESP_OK) err = "scene is empty";
    } else if (strcmp(action->valuestring, "delete") == 0) {
        if (scenes_delete(id) != ESP_OK) err = "delete failed";
    } else {
        err = "unknown action";
    }
    cJSON_Delete(root);
    return json_result(req, err);
}

static void push_task(void *arg)
{
    // Newer IDF versions don't call the URI handler for the handshake, so new clients are
    // found by scanning: every push while clients exist, every IDLE_SCAN pushes otherwise.
    uint32_t tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(PUSH_MS));
        if (s_idle_left > 0 || (++tick % IDLE_SCAN) == 0) {
            httpd_queue_work(s_server, push_work, NULL);
        }
    }
}

esp_err_t console_register(httpd_handle_t server)
{
    s_server = server;
    const httpd_uri_t page = { .uri = "/console", .method = HTTP_GET, .handler = console_get };
    const httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
    const httpd_uri_t names_get_uri = { .uri = "/api/names", .method = HTTP_GET, .handler = names_get_handler };
    const httpd_uri_t names_post_uri = { .uri = "/api/names", .method = HTTP_POST, .handler = names_post_handler };
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &ws);
    httpd_register_uri_handler(server, &names_get_uri);
    httpd_register_uri_handler(server, &names_post_uri);
    const httpd_uri_t scenes_get_uri = { .uri = "/api/scenes", .method = HTTP_GET, .handler = scenes_get_handler };
    const httpd_uri_t scenes_post_uri = { .uri = "/api/scenes", .method = HTTP_POST, .handler = scenes_post_handler };
    httpd_register_uri_handler(server, &scenes_get_uri);
    httpd_register_uri_handler(server, &scenes_post_uri);
    scenes_set_change_cb(scenes_changed);
    xTaskCreate(push_task, "console_push", 2560, NULL, 4, NULL);
    return ESP_OK;
}
