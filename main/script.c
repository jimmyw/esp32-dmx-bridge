#include "script.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"
#include "dmx_buffer.h"
#include "duktape.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "script";

#define BASE             "/scripts"
#define LABEL            "scripts"
#define MAX_PARAMS       16
#define MAX_INCLUDES     8
#define PARAM_NAME_MAX   15
#define LOG_SIZE         4096
#define TASK_STACK       24576
#define STACK_RESERVE    4096                 // native stack check: throw below this much headroom
// Wall-clock limits that only need to catch endless loops: flash writes (saving a file) stall
// the CPU for up to a few hundred ms.
#define SETUP_TIMEOUT_US (3000 * 1000)        // top-level code
#define FRAME_TIMEOUT_US (1000 * 1000)        // one frame() call
#define SAVE_DELAY_US    (2 * 1000 * 1000)    // params are written this long after the last change
#define NVS_NS           "script"
#define GUARD_CLEAR_US   (10 * 1000 * 1000)   // an autorun script has proven itself after this long

// Bundled examples, written to a fresh partition (main/CMakeLists.txt embeds them).
#define EXAMPLE(sym) extern const char sym##_start[] asm("_binary_" #sym "_start"); \
                     extern const char sym##_end[] asm("_binary_" #sym "_end");
EXAMPLE(script_prelude_js)
EXAMPLE(fan_circle_js)
EXAMPLE(color_chase_js)
EXAMPLE(figure_eight_js)
EXAMPLE(setup_js)
// since: the seed version that added the file. A bridge seeded before gets just the newer files,
// once, so a file the user deleted or changed stays that way.
#define SEED_VERSION 2
#define EXAMPLE_ENTRY(n, sym, v) { n, sym##_start, sym##_end, v }
static const struct { const char *name; const char *start, *end; uint8_t since; } s_examples[] = {
    EXAMPLE_ENTRY("fan-circle", fan_circle_js, 1),
    EXAMPLE_ENTRY("color-chase", color_chase_js, 1),
    EXAMPLE_ENTRY("figure-eight", figure_eight_js, 1),
    EXAMPLE_ENTRY("setup", setup_js, 2),
};

typedef struct {
    char   name[PARAM_NAME_MAX + 1];
    double value, min, max, step;
} param_t;

typedef enum { CMD_RUN, CMD_STOP, CMD_RELOAD } cmd_op_t;
typedef struct {
    cmd_op_t op;
    char name[SCRIPT_NAME_MAX + 1];
    SemaphoreHandle_t done;   // given when handled (may be NULL)
} cmd_t;

// Shared with the API (guarded by s_lock)
static SemaphoreHandle_t s_lock;
static char    s_running[SCRIPT_NAME_MAX + 1];
static char    s_error[200];
static char    s_error_name[SCRIPT_NAME_MAX + 1];
static param_t s_params[MAX_PARAMS];
static int     s_nparams;
static int64_t s_params_dirty_us;            // 0 = saved
static char    s_log[LOG_SIZE];
static size_t  s_log_len, s_log_end;        // s_log holds bytes [s_log_end - s_log_len, s_log_end)

// Engine task only
static QueueHandle_t s_queue;
static TaskHandle_t  s_task;
static duk_context  *s_ctx;
static cJSON        *s_saved;               // saved param values of the running script
static uint8_t       s_values[DMX_SLOTS], s_input[DMX_SLOTS], s_fader[DMX_SLOTS];
static bool          s_owned[DMX_SLOTS];
static char          s_included[MAX_INCLUDES][SCRIPT_NAME_MAX + 1];   // include()d by the running script
static int           s_nincluded;
static int64_t       s_start_us, s_last_us;
static volatile int64_t s_deadline_us;      // exec timeout, 0 = none
static const char   *s_deadline_what;       // "frame()" or "top-level code"
static bool          s_timed_out;
static bool          s_notify;
static size_t        s_heap_used, s_heap_limit;
static uint32_t      s_heap_caps;
static bool          s_mounted;
static bool          s_guarded;              // started by autorun, NVS "trying" still set
static void (*s_change_cb)(void);

/* ---------- Duktape hooks ---------- */

int script_exec_timeout(void *udata)
{
    int64_t d = s_deadline_us;
    if (d && esp_timer_get_time() > d) {
        s_timed_out = true;
        return 1;
    }
    return 0;
}

int script_native_stack_check(void)
{
    uint8_t *start = (uint8_t *)xTaskGetStackStart(NULL);
    uint8_t *sp = (uint8_t *)__builtin_frame_address(0);
    return sp - start < STACK_RESERVE;
}

// Heap: PSRAM when present, capped so a script can't starve the rest of the firmware.
// Each block carries its size in an 8-byte header.
static void *heap_alloc(void *ud, duk_size_t size)
{
    if (s_heap_used + size > s_heap_limit) {
        return NULL;
    }
    size_t *p = heap_caps_malloc(size + 8, s_heap_caps);
    if (!p) {
        return NULL;
    }
    p[0] = size;
    s_heap_used += size;
    return (uint8_t *)p + 8;
}

static void heap_free(void *ud, void *ptr)
{
    if (ptr) {
        size_t *p = (size_t *)((uint8_t *)ptr - 8);
        s_heap_used -= p[0];
        heap_caps_free(p);
    }
}

static void *heap_realloc(void *ud, void *ptr, duk_size_t size)
{
    if (!ptr) {
        return heap_alloc(ud, size);
    }
    if (size == 0) {
        heap_free(ud, ptr);
        return NULL;
    }
    size_t *p = (size_t *)((uint8_t *)ptr - 8);
    size_t old = p[0];
    if (size > old && s_heap_used + (size - old) > s_heap_limit) {
        return NULL;
    }
    size_t *np = heap_caps_realloc(p, size + 8, s_heap_caps);
    if (!np) {
        return NULL;
    }
    np[0] = size;
    s_heap_used = s_heap_used - old + size;
    return (uint8_t *)np + 8;
}

static void heap_fatal(void *ud, const char *msg)
{
    // Only reached for errors outside a protected call, which the engine never makes.
    ESP_LOGE(TAG, "duktape fatal: %s", msg ? msg : "?");
    abort();
}

/* ---------- log ---------- */

static void log_append(const char *text)
{
    size_t n = strlen(text);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (n > LOG_SIZE) {
        text += n - LOG_SIZE;
        s_log_end += n - LOG_SIZE;
        n = LOG_SIZE;
    }
    if (s_log_len + n > LOG_SIZE) {
        size_t drop = s_log_len + n - LOG_SIZE;
        memmove(s_log, s_log + drop, s_log_len - drop);
        s_log_len -= drop;
    }
    memcpy(s_log + s_log_len, text, n);
    s_log_len += n;
    s_log_end += n;
    xSemaphoreGive(s_lock);
}

size_t script_log_read(size_t since, char **text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t start = s_log_end - s_log_len;
    if (since < start || since > s_log_end) {
        since = start;   // fell behind, or the bridge restarted: everything that's kept
    }
    size_t n = s_log_end - since;
    *text = malloc(n + 1);
    if (*text) {
        memcpy(*text, s_log + (since - start), n);
        (*text)[n] = '\0';
    }
    size_t end = s_log_end;
    xSemaphoreGive(s_lock);
    return end;
}

/* ---------- files ---------- */

bool script_name_valid(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > SCRIPT_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

bool script_is_lib(const char *name)
{
    return strcmp(name, "setup") == 0 || name[0] == '_';
}

static void path_of(char *buf, size_t size, const char *name, const char *ext)
{
    snprintf(buf, size, BASE "/%.24s%s", name, ext);
}

char *script_read(const char *name, size_t *len)
{
    if (!s_mounted || !script_name_valid(name)) {
        return NULL;
    }
    char path[48];
    path_of(path, sizeof(path), name, ".js");
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (n >= 0 && n <= SCRIPT_MAX_SIZE) ? malloc(n + 1) : NULL;
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = '\0';
        if (len) *len = n;
    }
    return buf;
}

// Write `tmp` first so a full partition or a reset never leaves half a file. Callers in
// different tasks use different temp names.
static bool write_file(const char *path, const char *tmp, const char *data, size_t len)
{
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        unlink(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        unlink(tmp);
    }
    return ok;
}

static void changed(void)
{
    if (s_change_cb) {
        s_change_cb();
    }
}

static void autorun_set(const char *name)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (name) {
            nvs_set_str(h, "run", name);
        } else {
            nvs_erase_key(h, "run");
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

// NVS "trying": set while an autorun script is in its first seconds. Still set after a crash,
// watchdog or brown-out reset means it may have taken the bridge down, so it isn't started again.
static void guard_set(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (on) {
            nvs_set_u8(h, "trying", 1);
        } else {
            nvs_erase_key(h, "trying");
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

static void guard_clear(void)
{
    if (s_guarded) {
        s_guarded = false;
        guard_set(false);
    }
}

/* ---------- params ---------- */

static void params_save(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_params_dirty_us || !s_running[0]) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_params_dirty_us = 0;
    cJSON *o = cJSON_CreateObject();
    for (int i = 0; i < s_nparams; i++) {
        cJSON_AddNumberToObject(o, s_params[i].name, s_params[i].value);
    }
    char path[48];
    path_of(path, sizeof(path), s_running, ".json");
    xSemaphoreGive(s_lock);

    char *js = cJSON_PrintUnformatted(o);
    if (js) {
        write_file(path, BASE "/params.tmp", js, strlen(js));
        free(js);
    }
    // Keep the saved values in step, so a restart of the script picks them up.
    cJSON_Delete(s_saved);
    s_saved = o;
}

static cJSON *params_load(const char *name)
{
    char path[48];
    path_of(path, sizeof(path), name, ".json");
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *o = cJSON_Parse(buf);
    if (!cJSON_IsObject(o)) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

const char *script_set_param(const char *name, double value)
{
    const char *err = "no such parameter";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_nparams; i++) {
        param_t *p = &s_params[i];
        if (strcmp(p->name, name) == 0) {
            p->value = isnan(value) ? p->min : value < p->min ? p->min : value > p->max ? p->max : value;
            s_params_dirty_us = esp_timer_get_time();
            err = NULL;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* ---------- native functions ---------- */

static int chan_arg(duk_context *ctx, duk_idx_t idx)
{
    duk_double_t d = duk_to_number(ctx, idx);
    if (!(d >= 1 && d <= DMX_SLOTS)) {
        (void)duk_range_error(ctx, "channel must be 1-512");
    }
    return (int)d - 1;
}

static double level_arg(duk_context *ctx, duk_idx_t idx)
{
    double v = duk_to_number(ctx, idx);
    return isnan(v) ? 0 : v < 0 ? 0 : v > 255 ? 255 : v;
}

// set(ch, value 0..255)
static duk_ret_t js_set(duk_context *ctx)
{
    int ch = chan_arg(ctx, 0);
    s_values[ch] = (uint8_t)(level_arg(ctx, 1) + 0.5);
    s_owned[ch] = true;
    return 0;
}

// setFine(ch, value 0..255 with fraction, [fineCh = ch + 1])
static duk_ret_t js_set_fine(duk_context *ctx)
{
    int ch = chan_arg(ctx, 0);
    int fine = duk_is_undefined(ctx, 2) ? ch + 1 : chan_arg(ctx, 2);
    if (fine >= DMX_SLOTS) {
        return duk_range_error(ctx, "fine channel must be 1-512");
    }
    uint32_t x = (uint32_t)(level_arg(ctx, 1) * 257 + 0.5);
    s_values[ch] = x >> 8;
    s_values[fine] = x & 0xFF;
    s_owned[ch] = s_owned[fine] = true;
    return 0;
}

static duk_ret_t js_get(duk_context *ctx)
{
    duk_push_int(ctx, s_values[chan_arg(ctx, 0)]);
    return 1;
}

static duk_ret_t js_input(duk_context *ctx)
{
    duk_push_int(ctx, s_input[chan_arg(ctx, 0)]);
    return 1;
}

static duk_ret_t js_fader(duk_context *ctx)
{
    duk_push_int(ctx, s_fader[chan_arg(ctx, 0)]);
    return 1;
}

// release(ch): hand the channel back to the network input. release() releases all.
static duk_ret_t js_release(duk_context *ctx)
{
    if (duk_is_undefined(ctx, 0)) {
        memset(s_owned, 0, sizeof(s_owned));
    } else {
        s_owned[chan_arg(ctx, 0)] = false;
    }
    return 0;
}

static duk_ret_t js_print(duk_context *ctx)
{
    duk_idx_t n = duk_get_top(ctx);
    duk_push_string(ctx, " ");
    duk_insert(ctx, 0);
    duk_join(ctx, n);
    const char *s = duk_safe_to_string(ctx, -1);
    ESP_LOGI(TAG, "print: %s", s);
    duk_push_string(ctx, "\n");
    duk_concat(ctx, 2);
    log_append(duk_get_string(ctx, -1));
    return 0;
}

// param(name, default, [min = 0], [max = max(1, 2 * default)], [step]): declares a live
// parameter on first use and returns its current value.
static duk_ret_t js_param(duk_context *ctx)
{
    const char *name = duk_require_string(ctx, 0);
    if (strlen(name) == 0 || strlen(name) > PARAM_NAME_MAX) {
        return duk_range_error(ctx, "param name must be 1-%d characters", PARAM_NAME_MAX);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_nparams; i++) {
        if (strcmp(s_params[i].name, name) == 0) {
            double v = s_params[i].value;
            xSemaphoreGive(s_lock);
            duk_push_number(ctx, v);
            return 1;
        }
    }
    int count = s_nparams;
    xSemaphoreGive(s_lock);

    if (duk_is_undefined(ctx, 1)) {
        return 0;   // never declared: undefined
    }
    if (count >= MAX_PARAMS) {
        return duk_range_error(ctx, "too many params (max %d)", MAX_PARAMS);
    }
    param_t p = { 0 };
    strcpy(p.name, name);
    double def = duk_to_number(ctx, 1);
    p.min = duk_is_undefined(ctx, 2) ? 0 : duk_to_number(ctx, 2);
    p.max = duk_is_undefined(ctx, 3) ? fmax(1, 2 * def) : duk_to_number(ctx, 3);
    p.step = duk_is_undefined(ctx, 4) ? 0 : duk_to_number(ctx, 4);
    if (isnan(def) || isnan(p.min) || isnan(p.max) || p.max < p.min) {
        return duk_range_error(ctx, "param %s: needs a number default and min <= max", name);
    }
    const cJSON *saved = cJSON_GetObjectItemCaseSensitive(s_saved, name);
    p.value = cJSON_IsNumber(saved) ? saved->valuedouble : def;
    p.value = p.value < p.min ? p.min : p.value > p.max ? p.max : p.value;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_params[s_nparams++] = p;
    xSemaphoreGive(s_lock);
    s_notify = true;
    duk_push_number(ctx, p.value);
    return 1;
}

static bool included(const char *name)
{
    for (int i = 0; i < s_nincluded; i++) {
        if (strcmp(s_included[i], name) == 0) return true;
    }
    return false;
}

// include(name): run <name>.js in the global scope, once per script start (later calls, and
// include cycles, do nothing). For shared setup such as fixture types and the rig.
static duk_ret_t js_include(duk_context *ctx)
{
    const char *name = duk_require_string(ctx, 0);
    if (!script_name_valid(name)) {
        return duk_range_error(ctx, "include: bad script name '%s'", name);
    }
    if (included(name)) {
        return 0;
    }
    if (s_nincluded >= MAX_INCLUDES) {
        return duk_range_error(ctx, "include: too many files (max %d)", MAX_INCLUDES);
    }
    size_t len;
    char *src = script_read(name, &len);
    if (!src) {
        return duk_error(ctx, DUK_ERR_REFERENCE_ERROR, "include: no script '%s'", name);
    }
    strlcpy(s_included[s_nincluded++], name, sizeof(s_included[0]));
    duk_push_string(ctx, name);
    // Copy into the heap first, so the malloc'd buffer is freed even if compiling throws.
    duk_push_lstring(ctx, src, len);
    free(src);
    duk_swap(ctx, -1, -2);   // [source filename]
    duk_compile(ctx, 0);     // throws SyntaxError (reported with the include's name and line)
    duk_call(ctx, 0);
    return 0;
}

static const duk_function_list_entry s_natives[] = {
    { "include", js_include, 1 },
    { "set", js_set, 2 },
    { "setFine", js_set_fine, 3 },
    { "get", js_get, 1 },
    { "input", js_input, 1 },
    { "fader", js_fader, 1 },
    { "release", js_release, 1 },
    { "print", js_print, DUK_VARARGS },
    { "param", js_param, 5 },
    { NULL, NULL, 0 },
};

/* ---------- engine ---------- */

// Error at the top of the stack -> "line 12: TypeError: ..." in s_error and the log.
static void fail(const char *name)
{
    s_deadline_us = 0;
    char msg[sizeof(s_error)];
    int line = 0;
    char file[SCRIPT_NAME_MAX + 1] = "";
    if (duk_is_error(s_ctx, -1)) {
        // The innermost frame in the script or a file it included ("at frame (fan-circle:12)"):
        // errors thrown by natives or prelude helpers otherwise point into C code or the prelude.
        duk_get_prop_string(s_ctx, -1, "stack");
        const char *p = duk_get_string_default(s_ctx, -1, "");
        while (!line && (p = strchr(p, '('))) {
            const char *colon = strchr(++p, ':');
            size_t n = colon ? (size_t)(colon - p) : 0;
            if (n && n <= SCRIPT_NAME_MAX) {
                char f[SCRIPT_NAME_MAX + 1];
                memcpy(f, p, n);
                f[n] = '\0';
                if (strcmp(f, name) == 0 || included(f)) {
                    line = atoi(colon + 1);
                    strcpy(file, f);
                }
            }
        }
        duk_pop(s_ctx);
        if (!line) {   // syntax errors carry no stack frame of their file yet
            duk_get_prop_string(s_ctx, -1, "fileName");
            const char *f = duk_get_string_default(s_ctx, -1, "");
            if (strcmp(f, name) == 0 || included(f)) {
                strlcpy(file, f, sizeof(file));
                duk_get_prop_string(s_ctx, -2, "lineNumber");
                line = duk_get_int_default(s_ctx, -1, 0);
                duk_pop(s_ctx);
            }
            duk_pop(s_ctx);
        }
    }
    const char *e = duk_safe_to_string(s_ctx, -1);
    char timeout[64];
    if (s_timed_out) {
        s_timed_out = false;
        snprintf(timeout, sizeof(timeout), "%s ran longer than %d s (endless loop?)", s_deadline_what,
                 (int)((strcmp(s_deadline_what, "frame()") == 0 ? FRAME_TIMEOUT_US : SETUP_TIMEOUT_US) / 1000000));
        e = timeout;
    }
    if (line > 0) {
        // SyntaxErrors already end in " (line N)" or " (line N, end of input)"
        char suffix[24];
        snprintf(suffix, sizeof(suffix), " (line %d", line);
        const char *sfx = strstr(e, suffix);
        size_t n = strlen(e);
        int keep = (sfx && n > 0 && e[n - 1] == ')' && !strchr(sfx + 2, '(')) ? (int)(sfx - e) : (int)n;
        if (strcmp(file, name) == 0) {
            snprintf(msg, sizeof(msg), "line %d: %.*s", line, keep, e);
        } else {   // in an included file: "setup.js line 7: ..."
            snprintf(msg, sizeof(msg), "%s.js line %d: %.*s", file, line, keep, e);
        }
    } else {
        snprintf(msg, sizeof(msg), "%s", e);
    }
    ESP_LOGW(TAG, "%s: %s", name, msg);
    char logline[sizeof(msg) + 40];
    snprintf(logline, sizeof(logline), "[%s] %s\n", name, msg);
    log_append(logline);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_error, msg, sizeof(s_error));
    strlcpy(s_error_name, name, sizeof(s_error_name));
    xSemaphoreGive(s_lock);
}

static void engine_stop(void)
{
    guard_clear();   // stopped or failed without taking the bridge down
    params_save();
    if (s_ctx) {
        duk_destroy_heap(s_ctx);
        s_ctx = NULL;
    }
    cJSON_Delete(s_saved);
    s_saved = NULL;
    dmx_buffer_set_script(NULL, NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_running[0] = '\0';
    s_nparams = 0;
    xSemaphoreGive(s_lock);
}

static void engine_start(const char *name, bool guarded)
{
    engine_stop();
    s_guarded = guarded;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_error[0] = s_error_name[0] = '\0';
    xSemaphoreGive(s_lock);

    size_t len;
    char *src = script_read(name, &len);
    if (!src) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_error, "script not found", sizeof(s_error));
        strlcpy(s_error_name, name, sizeof(s_error_name));
        xSemaphoreGive(s_lock);
        return;
    }
    s_heap_used = 0;
    s_ctx = duk_create_heap(heap_alloc, heap_realloc, heap_free, NULL, heap_fatal);
    if (!s_ctx) {
        free(src);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_error, "out of memory", sizeof(s_error));
        strlcpy(s_error_name, name, sizeof(s_error_name));
        xSemaphoreGive(s_lock);
        return;
    }
    memset(s_values, 0, sizeof(s_values));
    memset(s_owned, 0, sizeof(s_owned));
    s_nincluded = 0;
    s_saved = params_load(name);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_running, name, sizeof(s_running));
    xSemaphoreGive(s_lock);

    duk_push_global_object(s_ctx);
    duk_put_function_list(s_ctx, -1, s_natives);
    duk_pop(s_ctx);

    s_deadline_what = "top-level code";
    s_timed_out = false;
    s_deadline_us = esp_timer_get_time() + SETUP_TIMEOUT_US;
    // TEXT embeds end in a NUL, which isn't part of the source
    bool ok = duk_peval_lstring(s_ctx, script_prelude_js_start,
                                script_prelude_js_end - script_prelude_js_start - 1) == 0;
    if (!ok) {
        fail(name);
    } else {
        duk_pop(s_ctx);
        dmx_buffer_get_input(s_input);
        uint8_t master;
        dmx_buffer_get_manual(s_fader, &master);
        duk_push_string(s_ctx, name);
        ok = duk_pcompile_lstring_filename(s_ctx, 0, src, len) == 0 && duk_pcall(s_ctx, 0) == 0;
        if (!ok) {
            fail(name);
        } else {
            duk_pop(s_ctx);
            ok = duk_get_global_string(s_ctx, "frame") && duk_is_function(s_ctx, -1);
            duk_pop(s_ctx);
            if (!ok) {
                duk_push_string(s_ctx, "script has no frame(t, dt) function");
                fail(name);
            }
        }
    }
    s_deadline_us = 0;
    free(src);
    if (!ok) {
        // Keep the param values the user set, but don't write the half-declared list.
        s_params_dirty_us = 0;
        engine_stop();
        return;
    }
    s_start_us = s_last_us = esp_timer_get_time();
    ESP_LOGI(TAG, "running %s (%u KB heap)", name, (unsigned)(s_heap_used / 1024));
}

static void engine_frame(void)
{
    int64_t now = esp_timer_get_time();
    dmx_buffer_get_input(s_input);
    uint8_t master;
    dmx_buffer_get_manual(s_fader, &master);

    duk_get_global_string(s_ctx, "frame");
    duk_push_number(s_ctx, (now - s_start_us) / 1e6);
    duk_push_number(s_ctx, (now - s_last_us) / 1e6);
    s_last_us = now;
    s_deadline_what = "frame()";
    s_deadline_us = now + FRAME_TIMEOUT_US;
    if (duk_pcall(s_ctx, 2) != 0) {
        char name[SCRIPT_NAME_MAX + 1];
        strlcpy(name, s_running, sizeof(name));
        fail(name);
        engine_stop();
        s_notify = true;
        return;
    }
    s_deadline_us = 0;
    duk_pop(s_ctx);
    dmx_buffer_set_script(s_values, s_owned);
    if (s_guarded && now - s_start_us > GUARD_CLEAR_US) {
        guard_clear();
    }
}

static void engine_task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    while (1) {
        cmd_t c;
        TickType_t wait = s_ctx ? 0 : portMAX_DELAY;   // idle: sleep until a command arrives
        while (xQueueReceive(s_queue, &c, wait) == pdTRUE) {
            wait = 0;
            // Saving the running script, or the one that just failed, (re)starts it.
            // ... and so does saving a file the running script include()d.
            bool reload = c.op == CMD_RELOAD &&
                          (strcmp(c.name, s_running) == 0 || strcmp(c.name, s_error_name) == 0);
            char restart[SCRIPT_NAME_MAX + 1] = "";
            if (c.op == CMD_RELOAD && s_ctx && included(c.name)) {
                strlcpy(restart, s_running, sizeof(restart));
            } else if (c.op == CMD_RELOAD && !s_ctx && s_error_name[0] && included(c.name)) {
                // the list is still that of the script that failed: fixing its setup.js retries it
                strlcpy(restart, s_error_name, sizeof(restart));
            }
            if (restart[0]) {
                engine_start(restart, false);
            } else if (c.op == CMD_RUN || reload) {
                engine_start(c.name, c.done == NULL && c.op == CMD_RUN);   // only autorun doesn't wait
            } else if (c.op == CMD_STOP) {
                engine_stop();
            }
            s_notify = true;
            if (c.done) {
                xSemaphoreGive(c.done);
            }
            wake = xTaskGetTickCount();
        }
        if (s_ctx) {
            engine_frame();
            if (s_params_dirty_us && esp_timer_get_time() - s_params_dirty_us > SAVE_DELAY_US) {
                params_save();
            }
        }
        if (s_notify) {
            s_notify = false;
            changed();
        }
        if (s_ctx) {
            int hz = g_config.refresh_hz ? g_config.refresh_hz : 40;
            TickType_t period = pdMS_TO_TICKS(1000 / hz);
            if (xTaskGetTickCount() - wake > 4 * period) {
                wake = xTaskGetTickCount();   // fell far behind (slow frame): don't burst
            }
            xTaskDelayUntil(&wake, period);
        }
    }
}

// Hand a command to the engine task; with wait, block until it's done.
static void send_cmd(cmd_op_t op, const char *name, bool wait)
{
    cmd_t c = { .op = op, .done = wait ? xSemaphoreCreateBinary() : NULL };
    if (name) {
        strlcpy(c.name, name, sizeof(c.name));
    }
    if (xQueueSend(s_queue, &c, pdMS_TO_TICKS(1000)) != pdTRUE) {
        if (c.done) vSemaphoreDelete(c.done);
        return;
    }
    if (c.done) {
        xSemaphoreTake(c.done, portMAX_DELAY);   // the engine bounds each command (timeouts)
        vSemaphoreDelete(c.done);
    }
}

/* ---------- API ---------- */

static bool exists(const char *name)
{
    char path[48];
    struct stat st;
    path_of(path, sizeof(path), name, ".js");
    return stat(path, &st) == 0;
}

const char *script_run(const char *name)
{
    if (!s_mounted) return "no scripts partition";
    if (!script_name_valid(name) || !exists(name)) return "no such script";
    if (script_is_lib(name)) return "a shared file for include(), not an effect";
    send_cmd(CMD_RUN, name, true);
    autorun_set(name);
    return NULL;
}

void script_stop(void)
{
    send_cmd(CMD_STOP, NULL, true);
    autorun_set(NULL);
}

const char *script_write(const char *name, const char *src, size_t len)
{
    if (!s_mounted) return "no scripts partition";
    if (!script_name_valid(name)) return "name: 1-24 letters, digits, - or _";
    if (len > SCRIPT_MAX_SIZE) return "script too large (max 32 KB)";
    char path[48];
    path_of(path, sizeof(path), name, ".js");
    if (!write_file(path, BASE "/upload.tmp", src, len)) return "write failed (partition full?)";
    send_cmd(CMD_RELOAD, name, true);   // live edit: a running script restarts with the new code
    return NULL;
}

const char *script_delete(const char *name)
{
    if (!s_mounted) return "no scripts partition";
    if (!script_name_valid(name) || !exists(name)) return "no such script";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool running = strcmp(s_running, name) == 0;
    xSemaphoreGive(s_lock);
    if (running) {
        script_stop();
    }
    char path[48];
    path_of(path, sizeof(path), name, ".js");
    unlink(path);
    path_of(path, sizeof(path), name, ".json");
    unlink(path);
    changed();
    return NULL;
}

cJSON *script_status_json(void)
{
    cJSON *r = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running[0]) {
        cJSON_AddStringToObject(r, "running", s_running);
    } else {
        cJSON_AddNullToObject(r, "running");
    }
    if (s_error[0]) {
        cJSON *f = cJSON_AddObjectToObject(r, "failed");
        cJSON_AddStringToObject(f, "name", s_error_name);
        cJSON_AddStringToObject(f, "error", s_error);
    }
    cJSON *pa = cJSON_AddArrayToObject(r, "params");
    for (int i = 0; i < s_nparams; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s_params[i].name);
        cJSON_AddNumberToObject(o, "value", s_params[i].value);
        cJSON_AddNumberToObject(o, "min", s_params[i].min);
        cJSON_AddNumberToObject(o, "max", s_params[i].max);
        cJSON_AddNumberToObject(o, "step", s_params[i].step);
        cJSON_AddItemToArray(pa, o);
    }
    cJSON_AddNumberToObject(r, "log_end", s_log_end);
    xSemaphoreGive(s_lock);
    cJSON_AddNumberToObject(r, "heap_used", s_heap_used);
    cJSON_AddNumberToObject(r, "heap_limit", s_heap_limit);

    cJSON *arr = cJSON_AddArrayToObject(r, "scripts");
    DIR *d = s_mounted ? opendir(BASE) : NULL;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t n = strlen(e->d_name);
            if (n <= 3 || strcmp(e->d_name + n - 3, ".js") != 0) {
                continue;
            }
            char name[SCRIPT_NAME_MAX + 1];
            if (n - 3 > SCRIPT_NAME_MAX) continue;
            memcpy(name, e->d_name, n - 3);
            name[n - 3] = '\0';
            char path[48];
            struct stat st;
            path_of(path, sizeof(path), name, ".js");
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", name);
            cJSON_AddNumberToObject(o, "size", stat(path, &st) == 0 ? st.st_size : 0);
            if (script_is_lib(name)) {
                cJSON_AddBoolToObject(o, "lib", true);
            }
            cJSON_AddItemToArray(arr, o);
        }
        closedir(d);
    }
    size_t total = 0, used = 0;
    if (s_mounted && esp_spiffs_info(LABEL, &total, &used) == ESP_OK) {
        cJSON_AddNumberToObject(r, "fs_total", total);
        cJSON_AddNumberToObject(r, "fs_used", used);
    }
    return r;
}

void script_set_change_cb(void (*cb)(void))
{
    s_change_cb = cb;
}

static void seed_examples(void)
{
    nvs_handle_t h;
    uint8_t seeded = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_get_u8(h, "seeded", &seeded);
    if (seeded < SEED_VERSION) {
        int n = 0;
        for (size_t i = 0; i < sizeof(s_examples) / sizeof(s_examples[0]); i++) {
            if (s_examples[i].since <= seeded || exists(s_examples[i].name)) {
                continue;
            }
            char path[48];
            path_of(path, sizeof(path), s_examples[i].name, ".js");
            // TEXT embeds carry a trailing NUL
            write_file(path, BASE "/upload.tmp", s_examples[i].start, s_examples[i].end - s_examples[i].start - 1);
            n++;
        }
        nvs_set_u8(h, "seeded", SEED_VERSION);
        nvs_commit(h);
        ESP_LOGI(TAG, "wrote %d example scripts", n);
    }
    nvs_close(h);
}

esp_err_t script_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(4, sizeof(cmd_t));

    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        s_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        s_heap_limit = 768 * 1024;
    } else {
        s_heap_caps = MALLOC_CAP_8BIT;
        s_heap_limit = 64 * 1024;
    }

    const esp_vfs_spiffs_conf_t conf = {
        .base_path = BASE,
        .partition_label = LABEL,
        .max_files = 3,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no '%s' partition (%s): scripts disabled", LABEL, esp_err_to_name(err));
    } else {
        s_mounted = true;
        seed_examples();
        size_t total = 0, used = 0;
        esp_spiffs_info(LABEL, &total, &used);
        ESP_LOGI(TAG, "scripts: %u/%u KB used, %u KB JS heap (%s)", (unsigned)(used / 1024),
                 (unsigned)(total / 1024), (unsigned)(s_heap_limit / 1024),
                 (s_heap_caps & MALLOC_CAP_SPIRAM) ? "PSRAM" : "internal");
    }

    // Core 0 with the network stack; DMX output runs on core 1.
    if (xTaskCreatePinnedToCore(engine_task, "script", TASK_STACK, NULL, 4, &s_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    char name[SCRIPT_NAME_MAX + 1];
    size_t len = sizeof(name);
    uint8_t trying = 0;
    nvs_handle_t h;
    bool autorun = false;
    if (s_mounted && nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        autorun = nvs_get_str(h, "run", name, &len) == ESP_OK && script_name_valid(name) && exists(name);
        nvs_get_u8(h, "trying", &trying);
        nvs_close(h);
    }
    esp_reset_reason_t why = esp_reset_reason();
    bool crash = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT ||
                 why == ESP_RST_WDT || why == ESP_RST_BROWNOUT || why == ESP_RST_CPU_LOCKUP ||
                 why == ESP_RST_UNKNOWN;
    if (autorun && trying && crash) {
        // It may be what took the bridge down: don't restart it into a boot loop.
        ESP_LOGW(TAG, "not restarting %s: the bridge crashed while it was starting (reset reason %d)",
                 name, why);
        guard_set(false);
        strlcpy(s_error, "not restarted: the bridge crashed while it was starting (run it to retry)",
                sizeof(s_error));
        strlcpy(s_error_name, name, sizeof(s_error_name));
    } else if (autorun) {
        ESP_LOGI(TAG, "autorun %s", name);
        guard_set(true);
        send_cmd(CMD_RUN, name, false);
    }
    return ESP_OK;
}
