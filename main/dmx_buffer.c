#include "dmx_buffer.h"

#include <string.h>
#include "config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_SOURCES 4

typedef struct {
    bool     used;
    uint8_t  type;
    uint8_t  id[16];
    uint32_t ip;
    uint8_t  priority;
    int64_t  last_seen_us;
    uint8_t  data[DMX_SLOTS];
} source_t;

static SemaphoreHandle_t s_lock;
static source_t s_sources[MAX_SOURCES];
static uint8_t s_output[DMX_SLOTS];   // network layer (kept for "hold last look")
static uint8_t s_final[DMX_SLOTS];    // network merged with the manual layer
static uint8_t s_manual[DMX_SLOTS];
static uint8_t s_manual_master = 255;
static dmx_stats_t s_stats;

void dmx_buffer_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(s_sources, 0, sizeof(s_sources));
    memset(s_output, 0, sizeof(s_output));
}

static source_t *find_or_alloc(const uint8_t id[16], int64_t now)
{
    source_t *free_slot = NULL;
    source_t *oldest = &s_sources[0];
    for (int i = 0; i < MAX_SOURCES; i++) {
        source_t *s = &s_sources[i];
        if (s->used && memcmp(s->id, id, 16) == 0) {
            return s;
        }
        if (!s->used && !free_slot) {
            free_slot = s;
        }
        if (s->last_seen_us < oldest->last_seen_us) {
            oldest = s;
        }
    }
    source_t *s = free_slot ? free_slot : oldest;
    memset(s, 0, sizeof(*s));
    s->used = true;
    memcpy(s->id, id, 16);
    s->last_seen_us = now;
    return s;
}

void dmx_buffer_submit(dmx_src_type_t type, const uint8_t id[16], uint32_t ip, uint8_t priority,
                       const uint8_t *data, uint16_t len)
{
    if (len > DMX_SLOTS) {
        len = DMX_SLOTS;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    source_t *s = find_or_alloc(id, now);
    s->type = type;
    s->ip = ip;
    s->priority = priority;
    s->last_seen_us = now;
    memcpy(s->data, data, len);
    if (len < DMX_SLOTS) {
        // Shorter packets: remaining slots are zero (E1.31 / Art-Net behaviour)
        memset(s->data + len, 0, DMX_SLOTS - len);
    }
    if (type == SRC_ARTNET) {
        s_stats.artnet_packets++;
    } else {
        s_stats.sacn_packets++;
    }
    xSemaphoreGive(s_lock);
}

void dmx_buffer_terminate(const uint8_t id[16])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SOURCES; i++) {
        if (s_sources[i].used && memcmp(s_sources[i].id, id, 16) == 0) {
            s_sources[i].used = false;
        }
    }
    xSemaphoreGive(s_lock);
}

void dmx_buffer_reset_sources(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SOURCES; i++) {
        s_sources[i].used = false;
    }
    xSemaphoreGive(s_lock);
}

void dmx_buffer_get_output(uint8_t out[DMX_SLOTS])
{
    int64_t now = esp_timer_get_time();
    int64_t timeout_us = (int64_t)g_config.loss_timeout_ms * 1000;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    source_t *best = NULL;
    uint8_t live = 0;
    for (int i = 0; i < MAX_SOURCES; i++) {
        source_t *s = &s_sources[i];
        if (!s->used) {
            continue;
        }
        if (now - s->last_seen_us > timeout_us) {
            s->used = false;
            continue;
        }
        live++;
        if (!best || s->priority > best->priority ||
            (s->priority == best->priority && s->last_seen_us > best->last_seen_us)) {
            best = s;
        }
    }

    if (best) {
        memcpy(s_output, best->data, DMX_SLOTS);
        s_stats.active_type = best->type;
        s_stats.active_ip = best->ip;
        s_stats.active_priority = best->priority;
    } else {
        if (g_config.on_loss == LOSS_BLACKOUT) {
            memset(s_output, 0, DMX_SLOTS);
        }
        s_stats.active_type = SRC_NONE;
        s_stats.active_ip = 0;
        s_stats.active_priority = 0;
    }
    s_stats.num_sources = live;
    s_stats.signal = best != NULL;
    for (int i = 0; i < DMX_SLOTS; i++) {
        uint8_t m = (uint8_t)((s_manual[i] * s_manual_master + 127) / 255);
        s_final[i] = m > s_output[i] ? m : s_output[i];
    }
    memcpy(out, s_final, DMX_SLOTS);
    xSemaphoreGive(s_lock);
}

void dmx_buffer_count_frame(void)
{
    s_stats.frames_sent++;
}

void dmx_buffer_get_stats(dmx_stats_t *stats)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *stats = s_stats;
    xSemaphoreGive(s_lock);
}

void dmx_buffer_peek(uint8_t *out, uint16_t n)
{
    if (n > DMX_SLOTS) {
        n = DMX_SLOTS;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_final, n);
    xSemaphoreGive(s_lock);
}

void dmx_buffer_set_manual(uint16_t ch, uint8_t value)
{
    if (ch < DMX_SLOTS) {
        s_manual[ch] = value;   // single byte store, read by the merge under the lock
    }
}

void dmx_buffer_set_manual_master(uint8_t master)
{
    s_manual_master = master;
}

void dmx_buffer_clear_manual(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_manual, 0, sizeof(s_manual));
    xSemaphoreGive(s_lock);
}

void dmx_buffer_get_manual(uint8_t out[DMX_SLOTS], uint8_t *master)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_manual, DMX_SLOTS);
    *master = s_manual_master;
    xSemaphoreGive(s_lock);
}
