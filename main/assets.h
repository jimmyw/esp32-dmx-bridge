#pragma once

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"

// Mount the "www" SPIFFS partition holding the web pages (falls back to built-in copies).
esp_err_t assets_init(void);
// Serve the web pages and POST /api/www (web package upload). Register before the "/*" catch-all.
esp_err_t assets_register(httpd_handle_t server);
// Add {"web":{source, version, git, built, fs_total, fs_used}} to a status object.
void      assets_add_status(cJSON *obj);
