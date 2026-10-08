#include "scenes.h"

#include <stdio.h>
#include <string.h>
#include "dmx_buffer.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "scenes";

/*
 * One flash sector per scene, after the channel-name slots in the "storage" partition.
 * A sector holds a header, the name and 512 values; the CRC makes a torn write read as empty.
 */
#define SCENES_BASE  0x10000
#define SECTOR       0x1000
#define SCENE_MAGIC  0x53434E31   // "SCN1"
#define FADE_STEP_MS 20

typedef struct {
    uint32_t magic;
    uint32_t crc;      // over name + values
    char     name[NAME_MAX_LEN + 1];
    uint8_t  values[DMX_SLOTS];
} scene_rec_t;

_Static_assert(sizeof(scene_rec_t) <= SECTOR, "scene record must fit one sector");

static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;
static bool s_used[SCENE_COUNT];
static char s_names[SCENE_COUNT][NAME_MAX_LEN + 1];
static int  s_active = -1;
static void (*s_change_cb)(void);

// Fade state (guarded by s_lock)
static uint8_t  s_from[DMX_SLOTS], s_to[DMX_SLOTS];
static bool     s_fade_mask[DMX_SLOTS];
static int64_t  s_fade_start_us;
static uint32_t s_fade_ms;
static volatile bool s_fading;
static TaskHandle_t s_fade_task;

static uint32_t rec_crc(const scene_rec_t *r)
{
    return esp_crc32_le(0, (const uint8_t *)r->name, sizeof(r->name) + sizeof(r->values));
}

static size_t sector_of(int id)
{
    return SCENES_BASE + (size_t)id * SECTOR;
}

static bool load(int id, scene_rec_t *r)
{
    return esp_partition_read(s_part, sector_of(id), r, sizeof(*r)) == ESP_OK &&
           r->magic == SCENE_MAGIC && r->crc == rec_crc(r);
}

static esp_err_t store(int id, scene_rec_t *r)
{
    r->magic = SCENE_MAGIC;
    r->crc = rec_crc(r);
    esp_err_t err = esp_partition_erase_range(s_part, sector_of(id), SECTOR);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, sector_of(id), r, sizeof(*r));
    }
    return err;
}

static void changed(void)
{
    if (s_change_cb) {
        s_change_cb();
    }
}

static bool valid_id(int id)
{
    return s_part && id >= 0 && id < SCENE_COUNT;
}

static void copy_name(char *dst, const char *src, int id)
{
    names_sanitize(dst, src);
    if (!dst[0]) {
        snprintf(dst, NAME_MAX_LEN + 1, "Scene %d", id + 1);
    }
}

esp_err_t scenes_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!s_part || s_part->size < SCENES_BASE + SCENE_COUNT * SECTOR) {
        s_part = NULL;
        ESP_LOGW(TAG, "no 'storage' partition: scenes disabled");
        return ESP_ERR_NOT_FOUND;
    }
    static scene_rec_t r;
    int n = 0;
    for (int i = 0; i < SCENE_COUNT; i++) {
        s_used[i] = load(i, &r);
        if (s_used[i]) {
            memcpy(s_names[i], r.name, sizeof(s_names[i]));
            s_names[i][NAME_MAX_LEN] = '\0';
            n++;
        }
    }
    ESP_LOGI(TAG, "%d stored scenes", n);
    return ESP_OK;
}

bool scenes_used(int id) { return valid_id(id) && s_used[id]; }
const char *scenes_name(int id) { return scenes_used(id) ? s_names[id] : ""; }
int scenes_active(void) { return s_active; }
bool scenes_fading(void) { return s_fading; }
void scenes_set_change_cb(void (*cb)(void)) { s_change_cb = cb; }

esp_err_t scenes_save(int id, const char *name)
{
    if (!valid_id(id)) return ESP_ERR_INVALID_ARG;
    static scene_rec_t r;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&r, 0, sizeof(r));
    uint8_t master;
    dmx_buffer_get_manual(r.values, &master);
    // A scene without a name keeps its old one when overwritten.
    copy_name(r.name, (name && name[0]) ? name : (s_used[id] ? s_names[id] : ""), id);
    esp_err_t err = store(id, &r);
    if (err == ESP_OK) {
        s_used[id] = true;
        memcpy(s_names[id], r.name, sizeof(s_names[id]));
        s_active = id;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "save %d \"%s\": %s", id + 1, r.name, esp_err_to_name(err));
    changed();
    return err;
}

esp_err_t scenes_rename(int id, const char *name)
{
    if (!scenes_used(id)) return ESP_ERR_NOT_FOUND;
    static scene_rec_t r;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_CRC;
    if (load(id, &r)) {
        copy_name(r.name, name ? name : "", id);
        err = store(id, &r);
        if (err == ESP_OK) {
            memcpy(s_names[id], r.name, sizeof(s_names[id]));
        }
    }
    xSemaphoreGive(s_lock);
    changed();
    return err;
}

esp_err_t scenes_delete(int id)
{
    if (!valid_id(id)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = esp_partition_erase_range(s_part, sector_of(id), SECTOR);
    if (err == ESP_OK) {
        s_used[id] = false;
        s_names[id][0] = '\0';
        if (s_active == id) s_active = -1;
    }
    xSemaphoreGive(s_lock);
    changed();
    return err;
}

static void fade_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        TickType_t wake = xTaskGetTickCount();
        while (s_fading) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            int64_t el = esp_timer_get_time() - s_fade_start_us;
            uint32_t t = s_fade_ms ? (uint32_t)((el * 1024) / ((int64_t)s_fade_ms * 1000)) : 1024;
            if (t >= 1024) {
                t = 1024;
                s_fading = false;
            }
            for (int i = 0; i < DMX_SLOTS; i++) {
                if (s_fade_mask[i]) {
                    // all-positive form so rounding is symmetric for up and down fades
                    int v = (s_from[i] * (1024 - (int)t) + s_to[i] * (int)t + 512) / 1024;
                    dmx_buffer_set_manual(i, (uint8_t)v);
                }
            }
            xSemaphoreGive(s_lock);
            if (s_fading) {
                xTaskDelayUntil(&wake, pdMS_TO_TICKS(FADE_STEP_MS));
            }
        }
        changed();   // fade finished
    }
}

esp_err_t scenes_recall(int id, uint32_t fade_ms)
{
    if (!scenes_used(id)) return ESP_ERR_NOT_FOUND;
    static scene_rec_t r;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!load(id, &r)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_CRC;
    }
    uint8_t master;
    dmx_buffer_get_manual(s_from, &master);
    memcpy(s_to, r.values, DMX_SLOTS);
    for (int i = 0; i < DMX_SLOTS; i++) {
        s_fade_mask[i] = true;
    }
    s_fade_ms = fade_ms > SCENE_FADE_MAX ? SCENE_FADE_MAX : fade_ms;
    s_fade_start_us = esp_timer_get_time();
    s_active = id;
    s_fading = true;
    if (!s_fade_task) {
        xTaskCreate(fade_task, "scene_fade", 3072, NULL, 6, &s_fade_task);
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_fade_task);
    ESP_LOGI(TAG, "recall %d \"%s\" fade %lu ms", id + 1, r.name, (unsigned long)s_fade_ms);
    changed();
    return ESP_OK;
}

void scenes_release_channel(int ch)
{
    if (s_fading && ch >= 0 && ch < DMX_SLOTS) {
        s_fade_mask[ch] = false;   // single byte store; the fade task skips it from now on
    }
}

void scenes_stop_fade(void)
{
    s_fading = false;
}
