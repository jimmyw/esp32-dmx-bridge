#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "names.h"

#define SCENE_COUNT     64
#define SCENE_FADE_MAX  60000   // ms

// Scenes are snapshots of the console (manual) fader layer, stored in the "storage" partition.
esp_err_t   scenes_init(void);
bool        scenes_used(int id);                     // id 0..SCENE_COUNT-1
const char *scenes_name(int id);
esp_err_t   scenes_save(int id, const char *name);   // capture current console faders
esp_err_t   scenes_rename(int id, const char *name);
esp_err_t   scenes_delete(int id);
esp_err_t   scenes_recall(int id, uint32_t fade_ms);

// A console fader was moved by hand: stop fading that channel.
void scenes_release_channel(int ch);
void scenes_stop_fade(void);
int  scenes_active(void);         // last recalled scene, -1 if none
bool scenes_fading(void);

// Called whenever the scene list or active scene changes (used to notify web clients).
void scenes_set_change_cb(void (*cb)(void));
