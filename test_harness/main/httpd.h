#pragma once
#include "esp_err.h"

/**
 * Start the HTTP server and register all URI handlers.
 * Must be called after wifi_init().
 * Registers: GET /params, POST /params
 */
esp_err_t httpd_start_server(void);
