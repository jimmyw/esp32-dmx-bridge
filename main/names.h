#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define NAME_MAX_LEN 24   // bytes of UTF-8, excluding the terminator

// Channel names and console "hidden" flags (0-based channel index), kept in RAM and persisted to the "storage" partition.
esp_err_t   names_init(void);
const char *names_get(int ch);                 // "" when unnamed
bool        names_set(int ch, const char *name);   // false if ch is out of range
void        names_clear_all(void);            // names only; hidden flags stay
bool        names_hidden(int ch);              // channel hidden on the console
bool        names_set_hidden(int ch, bool hidden);
void        names_show_all(void);
void        names_save_soon(void);             // debounced write to flash

// Trim, drop control chars and cut to NAME_MAX_LEN bytes on a UTF-8 boundary.
void        names_sanitize(char dst[NAME_MAX_LEN + 1], const char *src);
