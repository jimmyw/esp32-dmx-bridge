#include "names.h"

#include <string.h>
#include "dmx_buffer.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "names";

/*
 * Two alternating slots in the "storage" partition; each holds a header plus the whole table.
 * On load the valid slot with the highest sequence number wins, so a power cut while saving
 * only ever loses the change being written.
 */
#define SLOT_SIZE  0x4000
#define NAMES_MAGIC 0x4E414D31   // "NAM1"
#define SAVE_DELAY_MS 2000

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;
    uint32_t crc;
} slot_header_t;

static char s_names[DMX_SLOTS][NAME_MAX_LEN + 1];
static SemaphoreHandle_t s_lock;
static const esp_partition_t *s_part;
static uint32_t s_seq;
static int s_slot;           // slot written most recently
static TaskHandle_t s_task;

_Static_assert(sizeof(slot_header_t) + sizeof(s_names) <= SLOT_SIZE, "names table too big for slot");

static bool read_slot(int slot, slot_header_t *hdr)
{
    if (esp_partition_read(s_part, slot * SLOT_SIZE, hdr, sizeof(*hdr)) != ESP_OK ||
        hdr->magic != NAMES_MAGIC || hdr->len != sizeof(s_names)) {
        return false;
    }
    static char tmp[DMX_SLOTS][NAME_MAX_LEN + 1];
    if (esp_partition_read(s_part, slot * SLOT_SIZE + sizeof(*hdr), tmp, sizeof(tmp)) != ESP_OK ||
        esp_crc32_le(0, (const uint8_t *)tmp, sizeof(tmp)) != hdr->crc) {
        return false;
    }
    memcpy(s_names, tmp, sizeof(s_names));
    return true;
}

static void save_now(void)
{
    static char snap[DMX_SLOTS][NAME_MAX_LEN + 1];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(snap, s_names, sizeof(snap));
    xSemaphoreGive(s_lock);

    int slot = s_slot ^ 1;
    slot_header_t hdr = {
        .magic = NAMES_MAGIC,
        .seq = s_seq + 1,
        .len = sizeof(snap),
        .crc = esp_crc32_le(0, (const uint8_t *)snap, sizeof(snap)),
    };
    esp_err_t err = esp_partition_erase_range(s_part, slot * SLOT_SIZE, SLOT_SIZE);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, slot * SLOT_SIZE + sizeof(hdr), snap, sizeof(snap));
    }
    if (err == ESP_OK) {
        // Header last: the slot only becomes valid once everything else is on flash.
        err = esp_partition_write(s_part, slot * SLOT_SIZE, &hdr, sizeof(hdr));
    }
    if (err == ESP_OK) {
        s_seq = hdr.seq;
        s_slot = slot;
        ESP_LOGI(TAG, "saved (slot %d, seq %lu)", slot, (unsigned long)s_seq);
    } else {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
}

static void names_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Debounce: keep waiting while more edits arrive.
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SAVE_DELAY_MS)) > 0) {
        }
        save_now();
    }
}

esp_err_t names_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(s_names, 0, sizeof(s_names));
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!s_part || s_part->size < 2 * SLOT_SIZE) {
        ESP_LOGW(TAG, "no 'storage' partition: channel names will not be saved");
        return ESP_ERR_NOT_FOUND;
    }
    slot_header_t h0, h1;
    bool v0 = read_slot(0, &h0);
    bool v1 = read_slot(1, &h1);
    // read_slot leaves the last valid slot loaded; reload the newer one when both are valid.
    if (v0 && v1) {
        int newer = (int32_t)(h1.seq - h0.seq) > 0 ? 1 : 0;
        read_slot(newer, newer ? &h1 : &h0);
        s_slot = newer;
        s_seq = newer ? h1.seq : h0.seq;
    } else if (v0 || v1) {
        s_slot = v1 ? 1 : 0;
        s_seq = v1 ? h1.seq : h0.seq;
    } else {
        memset(s_names, 0, sizeof(s_names));
        s_slot = 1;   // first save goes to slot 0
    }
    int named = 0;
    for (int i = 0; i < DMX_SLOTS; i++) {
        named += s_names[i][0] != '\0';
    }
    ESP_LOGI(TAG, "%d named channels", named);
    xTaskCreate(names_task, "names", 3072, NULL, 2, &s_task);
    return ESP_OK;
}

const char *names_get(int ch)
{
    return (ch >= 0 && ch < DMX_SLOTS) ? s_names[ch] : "";
}

void names_sanitize(char dst[NAME_MAX_LEN + 1], const char *src)
{
    while (*src == ' ') {
        src++;
    }
    size_t o = 0;
    for (size_t i = 0; src[i] && o < NAME_MAX_LEN;) {
        unsigned char c = src[i];
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (o + n > NAME_MAX_LEN) {
            break;
        }
        if (c >= 0x20 && c != 0x7F) {
            memcpy(&dst[o], &src[i], n);
            o += n;
        }
        i += n;
    }
    // trim trailing spaces
    while (o > 0 && dst[o - 1] == ' ') {
        o--;
    }
    dst[o] = '\0';
}

bool names_set(int ch, const char *name)
{
    if (ch < 0 || ch >= DMX_SLOTS) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    names_sanitize(s_names[ch], name);
    xSemaphoreGive(s_lock);
    return true;
}

void names_clear_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_names, 0, sizeof(s_names));
    xSemaphoreGive(s_lock);
}

void names_save_soon(void)
{
    if (s_task && s_part) {
        xTaskNotifyGive(s_task);
    }
}
