#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

// Web fader console: serves /console and the /ws WebSocket that drives the manual DMX layer.
esp_err_t console_register(httpd_handle_t server);

// Tell connected console pages that channel names changed (they re-fetch /api/names).
void console_names_changed(void);
