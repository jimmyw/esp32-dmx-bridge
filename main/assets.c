#include "assets.h"

#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"

/*
 * Embedded web files. Browsers may cache them but must revalidate (Cache-Control: no-cache);
 * the ETag is derived from the firmware build, so a firmware update invalidates every file
 * and an unchanged one is answered with 304 Not Modified.
 */
#define ASSET(sym)                                                          \
    extern const char sym##_start[] asm("_binary_" #sym "_start");          \
    extern const char sym##_end[] asm("_binary_" #sym "_end");

ASSET(index_html)
ASSET(index_css)
ASSET(index_js)
ASSET(console_html)
ASSET(console_css)
ASSET(console_js)

typedef struct {
    const char *uri;
    const char *type;
    const char *start;
    const char *end;
} asset_t;

static const asset_t s_assets[] = {
    { "/",            "text/html",              index_html_start,   index_html_end },
    { "/index.css",   "text/css",               index_css_start,    index_css_end },
    { "/index.js",    "text/javascript",        index_js_start,     index_js_end },
    { "/console",     "text/html",              console_html_start, console_html_end },
    { "/console.css", "text/css",               console_css_start,  console_css_end },
    { "/console.js",  "text/javascript",        console_js_start,   console_js_end },
};

static char s_etag[20];

static esp_err_t asset_get(httpd_req_t *req)
{
    const asset_t *a = req->user_ctx;
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", s_etag);
    char inm[sizeof(s_etag)];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        strcmp(inm, s_etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, a->type);
    // EMBED_TXTFILES appends a NUL terminator
    return httpd_resp_send(req, a->start, a->end - a->start - 1);
}

esp_err_t assets_register(httpd_handle_t server)
{
    char sha[9];
    esp_app_get_elf_sha256(sha, sizeof(sha));
    snprintf(s_etag, sizeof(s_etag), "\"%s\"", sha);
    for (size_t i = 0; i < sizeof(s_assets) / sizeof(s_assets[0]); i++) {
        const httpd_uri_t u = {
            .uri = s_assets[i].uri, .method = HTTP_GET, .handler = asset_get, .user_ctx = (void *)&s_assets[i],
        };
        esp_err_t err = httpd_register_uri_handler(server, &u);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
