#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "esp_err.h"

#define SCRIPT_NAME_MAX   24          // file name without ".js"
#define SCRIPT_MAX_SIZE   (32 * 1024)

/*
 * JavaScript effects (Duktape). Scripts are "<name>.js" files on the "scripts" SPIFFS
 * partition; one runs at a time, calling its frame(t, dt) at the DMX refresh rate. Channels it
 * sets replace the network input (see dmx_buffer_set_script). Live parameters declared with
 * param() are saved next to the script as "<name>.json". The running script restarts after a
 * reboot. The JS API is described in scripts/README.md.
 */
esp_err_t script_init(void);   // mount, seed the examples on a fresh partition, autorun

bool  script_name_valid(const char *name);
char *script_read(const char *name, size_t *len);                    // malloc'd, NULL if missing
const char *script_write(const char *name, const char *src, size_t len);   // NULL or error;
                                       // (re)starts it if it is running or just failed
const char *script_delete(const char *name);
const char *script_run(const char *name);    // (re)start; returns NULL or an error
void        script_stop(void);
const char *script_set_param(const char *name, double value);

// {"running":"fan-circle"|null,"failed":{"name":..,"error":"line 3: .."},
//  "params":[{name,value,min,max,step}],"log_end":..,"heap_used":..,"heap_limit":..,
//  "scripts":[{"name":..,"size":..}],"fs_total":..,"fs_used":..}
cJSON *script_status_json(void);

// Log text (print() output and errors) written after byte offset `since`. Returns the new end
// offset; *text is malloc'd (caller frees).
size_t script_log_read(size_t since, char **text);

// Called (from the engine task) when the running script, its params or the file list change.
void script_set_change_cb(void (*cb)(void));
