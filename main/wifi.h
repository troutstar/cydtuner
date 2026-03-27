#pragma once
#include "esp_err.h"

/* Connect to the AP defined in wifi_credentials.h and block until IP assigned. */
esp_err_t wifi_sta_init(void);
