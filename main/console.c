#include "console.h"

#include <string.h>
#include "dmx_buffer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "console";

/*
 * WebSocket protocol (binary):
 *   client -> server: repeated 3-byte records [ch_hi, ch_lo, value]; ch 0..511, 0xFFFF = master.
 *                     text "clear" zeroes all console faders.
 *   server -> client: every PUSH_MS: [0x01][master][manual x512][output x512]
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
    httpd_register_uri_handler(server, &page);
    httpd_register_uri_handler(server, &ws);
    xTaskCreate(push_task, "console_push", 2560, NULL, 4, NULL);
    return ESP_OK;
}
