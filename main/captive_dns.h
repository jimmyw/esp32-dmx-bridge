#pragma once

#include "esp_err.h"

// Minimal DNS server: answers every A query with the soft-AP address so phones open the portal.
esp_err_t captive_dns_start(void);
