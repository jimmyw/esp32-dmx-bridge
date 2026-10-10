#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

// /api/scripts* and /scripts/<name>.js (GET source, PUT save). Register before the "/*" catch-all.
esp_err_t script_web_register(httpd_handle_t server);
