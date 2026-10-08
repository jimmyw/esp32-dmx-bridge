#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

// Serve the embedded web pages (gzipped, built by webpack from web/src). Register before the "/*" catch-all.
esp_err_t assets_register(httpd_handle_t server);
