#include "audio.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "audio_rec";

/*
 * Recording: a client that connects to TCP port AUDIO_REC_PORT gets the raw microphone signal
 * until it disconnects, as 32-bit float samples (little endian, -1..1, mono, AUDIO_REC_RATE Hz):
 *     nc <bridge> 7001 > opus.f32
 * One client at a time. Blocks the network can't keep up with are dropped (logged); the audio
 * analysis never waits for it.
 */
#define BUF_BYTES (64 * 1024)   // ~0.7 s

static StreamBufferHandle_t s_buf;
static volatile bool s_on;
static volatile uint32_t s_dropped;

static void tap(const float *samples, int n)
{
    if (!s_on) return;
    size_t bytes = n * sizeof(float);
    if (xStreamBufferSpacesAvailable(s_buf) < bytes) {
        s_dropped++;
        return;
    }
    xStreamBufferSend(s_buf, samples, bytes, 0);
}

static void rec_task(void *arg)
{
    static uint8_t chunk[4096];
    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(AUDIO_REC_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (srv < 0 || bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(srv, 1) < 0) {
        ESP_LOGE(TAG, "can't listen on port %d", AUDIO_REC_PORT);
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        struct timeval tv = { .tv_sec = 5 };   // a client that stops reading is dropped
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        xStreamBufferReset(s_buf);
        s_dropped = 0;
        s_on = true;
        ESP_LOGI(TAG, "recording to a client");
        uint64_t sent = 0;
        while (1) {
            size_t n = xStreamBufferReceive(s_buf, chunk, sizeof(chunk), pdMS_TO_TICKS(500));
            if (n == 0) continue;   // no microphone data right now
            if (send(fd, chunk, n, 0) != (ssize_t)n) break;
            sent += n;
        }
        s_on = false;
        close(fd);
        ESP_LOGI(TAG, "recording ended: %.1f s, %lu blocks dropped", (double)sent / sizeof(float) / AUDIO_REC_RATE,
                 (unsigned long)s_dropped);
        vTaskDelay(pdMS_TO_TICKS(50));   // let a tap call in progress finish before the reset
    }
}

esp_err_t audio_rec_start(void)
{
    // Sent in 4 KB pieces: small ones crawl (delayed ACKs).
    s_buf = xStreamBufferCreateWithCaps(BUF_BYTES, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) return ESP_ERR_NO_MEM;
    audio_set_tap(tap);
    return xTaskCreate(rec_task, "audio_rec", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
