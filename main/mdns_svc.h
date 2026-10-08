#pragma once

#include "esp_err.h"

esp_err_t mdns_svc_init(void);

// (Re)publish hostname, instance name and services from g_config.
void mdns_svc_update(void);
