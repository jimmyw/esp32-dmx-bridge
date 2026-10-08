#pragma once

#include "esp_err.h"

#define SACN_PORT 5568

esp_err_t sacn_start(void);

// Re-join the multicast group (after Wi-Fi reconnect or universe/protocol change).
void sacn_rejoin(void);
