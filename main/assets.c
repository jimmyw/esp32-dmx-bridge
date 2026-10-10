#include "assets.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_spiffs.h"

static const char *TAG = "assets";

/*
 * Web pages: each one gzipped HTML file with its CSS and JS inlined, built by webpack from
 * web/src (see web/webpack.config.js).
 *
 * They are served from the "www" SPIFFS partition, which `idf.py flash` fills and which can be
 * replaced on its own by uploading dmx_bridge_www.tar (POST /api/www) — no firmware update
 * needed. The firmware also embeds the copies it was built with. Whichever set was built more
 * recently wins (manifest "built" time): so a firmware update brings its own newer pages, and a
 * newer package overrides them. The built-in copies are also used when a file is missing from
 * the partition, and always with "?builtin" (e.g. /?builtin), so a broken web package can't lock
 * you out of the settings page.
 *
 * Browsers may cache pages but must revalidate (Cache-Control: no-cache). The ETag is a hash of
 * the installed package's manifest (or of the firmware build for the built-in copies), so a new
 * package or firmware is always picked up and an unchanged page gets 304 Not Modified.
 */
#define WWW_BASE      "/www"
#define WWW_LABEL     "www"
#define MANIFEST      WWW_BASE "/manifest.json"
#define MAX_FILES     16
#define MAX_NAME      24          // file name in a web package (SPIFFS limit is 32 with path)
#define CHUNK         4096

#define ASSET(sym)                                                          \
    extern const char sym##_start[] asm("_binary_" #sym "_start");          \
    extern const char sym##_end[] asm("_binary_" #sym "_end");

ASSET(index_html_gz)
ASSET(settings_html_gz)
ASSET(console_html_gz)
ASSET(scripts_html_gz)
ASSET(manifest_json)

typedef struct {
    const char *uri;
    const char *file;     // name in the www partition / web package
    const char *start;    // built-in copy
    const char *end;
} asset_t;

static const asset_t s_assets[] = {
    { "/",        "index.html.gz",   index_html_gz_start,   index_html_gz_end },
    { "/settings", "settings.html.gz", settings_html_gz_start, settings_html_gz_end },
    { "/console", "console.html.gz", console_html_gz_start, console_html_gz_end },
    { "/scripts", "scripts.html.gz", scripts_html_gz_start, scripts_html_gz_end },
};

static bool s_mounted;
static char s_builtin_etag[20];
static char s_fs_etag[20];       // "" when no package is installed
static cJSON *s_manifest;        // manifest.json of the installed package, if it is used
static const char *s_builtin_built = "";   // build time of the built-in pages

/* ---------- mount + manifest ---------- */

static void load_manifest(void)
{
    cJSON_Delete(s_manifest);
    s_manifest = NULL;
    s_fs_etag[0] = '\0';
    FILE *f = s_mounted ? fopen(MANIFEST, "rb") : NULL;
    if (!f) {
        return;
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    s_manifest = cJSON_Parse(buf);
    const cJSON *built = cJSON_GetObjectItem(s_manifest, "built");
    if (!cJSON_IsString(built) || strcmp(built->valuestring, s_builtin_built) < 0) {
        // Older than the pages in this firmware (ISO times compare as strings): use those.
        cJSON_Delete(s_manifest);
        s_manifest = NULL;
        return;
    }
    snprintf(s_fs_etag, sizeof(s_fs_etag), "\"w%08lx\"", (unsigned long)esp_crc32_le(0, (uint8_t *)buf, n));
}

esp_err_t assets_init(void)
{
    char sha[9];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    snprintf(s_builtin_etag, sizeof(s_builtin_etag), "\"%s\"", sha);
    static cJSON *builtin;
    builtin = cJSON_Parse(manifest_json_start);
    const cJSON *built = cJSON_GetObjectItem(builtin, "built");
    if (cJSON_IsString(built)) {
        s_builtin_built = built->valuestring;
    }

    const esp_vfs_spiffs_conf_t conf = {
        .base_path = WWW_BASE,
        .partition_label = WWW_LABEL,
        .max_files = 4,
        .format_if_mount_failed = true,   // blank partition: start empty, built-in pages serve
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no '%s' partition (%s): serving built-in web pages", WWW_LABEL, esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    load_manifest();
    size_t total = 0, used = 0;
    esp_spiffs_info(WWW_LABEL, &total, &used);
    const cJSON *ver = cJSON_GetObjectItem(s_manifest, "version");
    ESP_LOGI(TAG, "www: %u/%u KB used, %s", (unsigned)(used / 1024), (unsigned)(total / 1024),
             cJSON_IsString(ver) ? "serving the web package" : "serving the built-in pages (no newer package)");
    return ESP_OK;
}

void assets_add_status(cJSON *obj)
{
    cJSON *w = cJSON_AddObjectToObject(obj, "web");
    cJSON_AddStringToObject(w, "source", s_manifest ? "package" : "builtin");
    cJSON_AddStringToObject(w, "builtin_built", s_builtin_built);
    if (s_manifest) {
        static const char *keys[] = { "version", "git", "built" };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            const cJSON *v = cJSON_GetObjectItem(s_manifest, keys[i]);
            if (cJSON_IsString(v)) {
                cJSON_AddStringToObject(w, keys[i], v->valuestring);
            }
        }
    }
    if (s_mounted) {
        size_t total = 0, used = 0;
        esp_spiffs_info(WWW_LABEL, &total, &used);
        cJSON_AddNumberToObject(w, "fs_total", total);
        cJSON_AddNumberToObject(w, "fs_used", used);
    }
}

/* ---------- serving ---------- */

static bool not_modified(httpd_req_t *req, const char *etag)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", etag);
    char inm[24];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK && strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_send(req, NULL, 0);
        return true;
    }
    return false;
}

static esp_err_t asset_get(httpd_req_t *req)
{
    const asset_t *a = req->user_ctx;
    char query[32];
    bool builtin = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                   strncmp(query, "builtin", 7) == 0;
    char path[48];
    snprintf(path, sizeof(path), WWW_BASE "/%s", a->file);
    FILE *f = (!builtin && s_mounted && s_fs_etag[0]) ? fopen(path, "rb") : NULL;

    if (not_modified(req, f ? s_fs_etag : s_builtin_etag)) {
        if (f) fclose(f);
        return ESP_OK;
    }
    // Every browser accepts gzip, so the pages are only stored compressed.
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    if (!f) {
        return httpd_resp_send(req, a->start, a->end - a->start);
    }
    static char buf[CHUNK];   // httpd runs one request at a time
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

/* ---------- web package upload: POST /api/www (ustar archive) ---------- */

typedef struct {
    char names[MAX_FILES][MAX_NAME + 1];
    int count;
} file_list_t;

static bool valid_name(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > MAX_NAME || n[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool has_suffix(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

static uint64_t octal(const uint8_t *p, size_t len)
{
    uint64_t v = 0;
    for (size_t i = 0; i < len && p[i] >= '0' && p[i] <= '7'; i++) {
        v = v * 8 + (p[i] - '0');
    }
    return v;
}

// Delete the files for which keep() is false. Names are collected first: deleting while
// iterating a SPIFFS directory can skip entries.
static void remove_files(bool (*keep)(const char *name, const file_list_t *list), const file_list_t *list)
{
    static char doomed[MAX_FILES * 2][40];
    int n = 0;
    DIR *d = opendir(WWW_BASE);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < MAX_FILES * 2) {
        if (!keep(e->d_name, list) && strlen(e->d_name) < sizeof(doomed[0])) {
            strcpy(doomed[n++], e->d_name);
        }
    }
    closedir(d);
    char path[48];
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof(path), WWW_BASE "/%.39s", doomed[i]);   // doomed names are < 40
        unlink(path);
    }
}

static bool keep_unless_new(const char *name, const file_list_t *list)
{
    return !has_suffix(name, ".new");
}

static bool keep_if_listed(const char *name, const file_list_t *list)
{
    for (int i = 0; i < list->count; i++) {
        if (strcmp(name, list->names[i]) == 0) return true;
    }
    return false;
}

// Leftovers of an interrupted upload.
static void remove_new_files(void)
{
    remove_files(keep_unless_new, NULL);
}

static esp_err_t www_error(httpd_req_t *req, const char *msg)
{
    ESP_LOGW(TAG, "web package rejected: %s", msg);
    remove_new_files();
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t www_post(httpd_req_t *req)
{
    if (!s_mounted) {
        return www_error(req, "no www partition on this device");
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(WWW_LABEL, &total, &used);
    if (req->content_len == 0 || req->content_len > total / 2) {
        return www_error(req, req->content_len ? "too large for a web package (firmware .bin files go to the firmware upload)" : "empty upload");
    }
    remove_new_files();

    static file_list_t list;
    list.count = 0;
    static uint8_t buf[CHUNK];
    static uint8_t hdr[512];
    size_t hdr_fill = 0;
    uint64_t data_left = 0, pad_left = 0;   // of the current entry
    FILE *out = NULL;
    int zero_blocks = 0;
    bool done = false;
    size_t received = 0;
    const char *err = NULL;

    while (received < req->content_len && !err) {
        int r = httpd_req_recv(req, (char *)buf, sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { err = "upload interrupted"; break; }
        received += r;
        size_t pos = 0;
        while (pos < (size_t)r && !err && !done) {
            if (data_left > 0) {                       // file contents
                size_t n = (size_t)r - pos < data_left ? (size_t)r - pos : (size_t)data_left;
                if (out && fwrite(buf + pos, 1, n, out) != n) err = "write failed (www partition full?)";
                pos += n;
                data_left -= n;
                if (data_left == 0 && out) {
                    if (fclose(out) != 0) err = "write failed";
                    out = NULL;
                }
            } else if (pad_left > 0) {                 // padding to the next 512-byte block
                size_t n = (size_t)r - pos < pad_left ? (size_t)r - pos : (size_t)pad_left;
                pos += n;
                pad_left -= n;
            } else {                                   // header block
                size_t n = (size_t)r - pos < 512 - hdr_fill ? (size_t)r - pos : 512 - hdr_fill;
                memcpy(hdr + hdr_fill, buf + pos, n);
                pos += n;
                hdr_fill += n;
                if (hdr_fill < 512) break;
                hdr_fill = 0;

                bool zero = true;
                for (int i = 0; i < 512 && zero; i++) zero = hdr[i] == 0;
                if (zero) {
                    if (++zero_blocks == 2) done = true;
                    continue;
                }
                zero_blocks = 0;
                uint32_t sum = 0;
                for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : hdr[i];
                if (sum != octal(hdr + 148, 8) || memcmp(hdr + 257, "ustar", 5) != 0) {
                    err = "not a tar archive (upload dmx_bridge_www.tar)";
                    break;
                }
                uint64_t size = octal(hdr + 124, 12);
                data_left = size;
                pad_left = (512 - size % 512) % 512;
                char type = hdr[156];
                if (type != '0' && type != '\0') continue;   // directories, pax headers: skip

                char name[101];
                memcpy(name, hdr, 100);
                name[100] = '\0';
                const char *base = strncmp(name, "./", 2) == 0 ? name + 2 : name;
                if (hdr[345] != '\0' || !valid_name(base)) { err = "bad file name in package"; break; }
                if (list.count == MAX_FILES) { err = "too many files in package"; break; }
                for (int i = 0; i < list.count; i++) {
                    if (strcmp(list.names[i], base) == 0) { err = "duplicate file in package"; break; }
                }
                if (err) break;
                strcpy(list.names[list.count++], base);
                char path[48];
                snprintf(path, sizeof(path), WWW_BASE "/%s.new", base);
                out = fopen(path, "wb");
                if (!out) { err = "cannot create file on www partition"; break; }
                if (size == 0) { fclose(out); out = NULL; }
            }
        }
    }
    if (out) fclose(out);
    // Drain anything after the end of the archive so the connection stays usable.
    while (!err && received < req->content_len) {
        int r = httpd_req_recv(req, (char *)buf, sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        received += r;
    }
    if (!err && (!done || data_left > 0)) err = "package is truncated";
    bool has_index = false, has_manifest = false;
    for (int i = 0; i < list.count; i++) {
        has_index |= strcmp(list.names[i], "index.html.gz") == 0;
        has_manifest |= strcmp(list.names[i], "manifest.json") == 0;
    }
    if (!err && (!has_index || !has_manifest)) err = "package must contain index.html.gz and manifest.json";
    if (!err) {
        // A package older than the firmware's own pages would never be served.
        FILE *m = fopen(WWW_BASE "/manifest.json.new", "rb");
        char mbuf[512];
        size_t n = m ? fread(mbuf, 1, sizeof(mbuf) - 1, m) : 0;
        if (m) fclose(m);
        mbuf[n] = '\0';
        cJSON *man = cJSON_Parse(mbuf);
        const cJSON *built = cJSON_GetObjectItem(man, "built");
        if (!cJSON_IsString(built)) err = "manifest.json has no build time";
        else if (strcmp(built->valuestring, s_builtin_built) < 0) err = "package is older than the web UI built into this firmware";
        cJSON_Delete(man);
    }
    if (err) {
        return www_error(req, err);
    }

    // Everything arrived intact: swap the new files in, then drop files the package no longer has.
    char path[48], tmp[48];
    for (int i = 0; i < list.count; i++) {
        snprintf(path, sizeof(path), WWW_BASE "/%s", list.names[i]);
        snprintf(tmp, sizeof(tmp), WWW_BASE "/%s.new", list.names[i]);
        unlink(path);
        if (rename(tmp, path) != 0) {
            ESP_LOGE(TAG, "rename %s failed", tmp);
        }
    }
    remove_files(keep_if_listed, &list);
    load_manifest();
    const cJSON *ver = cJSON_GetObjectItem(s_manifest, "version");
    ESP_LOGI(TAG, "web package installed: %d files, version %s", list.count,
             cJSON_IsString(ver) ? ver->valuestring : "?");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "files", list.count);
    assets_add_status(r);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, s);
    free(s);
    return e;
}

esp_err_t assets_register(httpd_handle_t server)
{
    for (size_t i = 0; i < sizeof(s_assets) / sizeof(s_assets[0]); i++) {
        const httpd_uri_t u = {
            .uri = s_assets[i].uri, .method = HTTP_GET, .handler = asset_get, .user_ctx = (void *)&s_assets[i],
        };
        esp_err_t err = httpd_register_uri_handler(server, &u);
        if (err != ESP_OK) {
            return err;
        }
    }
    const httpd_uri_t up = { .uri = "/api/www", .method = HTTP_POST, .handler = www_post };
    return httpd_register_uri_handler(server, &up);
}
