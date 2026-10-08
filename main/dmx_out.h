#pragma once

#include "esp_err.h"

// Starts the DMX512 transmitter task using pins/UART from g_config.
esp_err_t dmx_out_start(void);
