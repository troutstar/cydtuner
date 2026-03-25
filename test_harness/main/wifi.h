#pragma once
#include "esp_err.h"

/**
 * Connect to the riotscanner WiFi network in station mode.
 * Blocks until an IP address is assigned (no timeout).
 * Starts mDNS as cydtuner-test.local after IP is assigned.
 * Must be called after nvs_flash_init().
 */
esp_err_t wifi_init(void);
