# Test Harness Plan 1 — Scaffold, WiFi, and HTTP /params

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create the `test_harness/` ESP-IDF project, connect it to the riotscanner WiFi network, and expose a working `GET /params` REST endpoint — device reachable at `http://cydtuner-test.local/params`.

**Architecture:** Minimal three-module app (wifi.c, httpd.c, test_harness.c) plus main.c in `test_harness/main/`. WiFi station mode blocks until IP is assigned, then mDNS registers `cydtuner-test.local`, then HTTP server starts. Audio/pitch tasks are not started in this plan.

**Tech Stack:** ESP-IDF 5.x, FreeRTOS, esp_wifi, esp_netif, esp_http_server, mdns, cJSON (IDF `json` component)

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `test_harness/CMakeLists.txt` | Create | Top-level project cmake |
| `test_harness/main/CMakeLists.txt` | Create | Component registration, linker wrap, compile define |
| `test_harness/sdkconfig.defaults` | Create | Non-default IDF options (WS support, WDT timeout) |
| `test_harness/main/test_harness.h` | Create | `tuner_params_t` definition + getter/setter API |
| `test_harness/main/test_harness.c` | Create | Params storage with mutex |
| `test_harness/main/wifi.h` | Create | `wifi_init()` declaration |
| `test_harness/main/wifi.c` | Create | WiFi station mode + mDNS |
| `test_harness/main/httpd.h` | Create | `httpd_start_server()` declaration |
| `test_harness/main/httpd.c` | Create | `GET /params`, `POST /params` handlers |
| `test_harness/main/main.c` | Create | `app_main` — init sequence |

---

### Task 1: Build system scaffold

**Files:**
- Create: `test_harness/CMakeLists.txt`
- Create: `test_harness/main/CMakeLists.txt`
- Create: `test_harness/sdkconfig.defaults`
- Create: `test_harness/main/test_harness.h` (stub)
- Create: `test_harness/main/test_harness.c` (stub)
- Create: `test_harness/main/wifi.h` (stub)
- Create: `test_harness/main/wifi.c` (stub)
- Create: `test_harness/main/httpd.h` (stub)
- Create: `test_harness/main/httpd.c` (stub)
- Create: `test_harness/main/main.c` (stub)

- [ ] **Step 1: Write `test_harness/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_harness)
```

`EXTRA_COMPONENT_DIRS` makes `../components/ili9341/` and `../components/xpt2046/` available. They are not compiled in Plan 1 but are required when display.c and touch.c are added in Plan 3.

- [ ] **Step 2: Write `test_harness/main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "main.c" "wifi.c" "httpd.c" "test_harness.c"
    # "../../main" headers needed once pitch.c / audio.c are added to SRCS in Plan 2.
    # Inert in Plan 1 but listed now so CMakeLists.txt does not need editing later.
    INCLUDE_DIRS "." "../../main"
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        mdns
        nvs_flash
        esp_http_server
        json
        esp_timer
)

# audio.c defines __wrap_sdmmc_init_spi_crc to suppress CMD59 on older SD cards.
# The __wrap_ symbol is only present when audio.c is in SRCS (Plan 2).
# Enabling the linker wrap without that symbol causes an "undefined reference" link
# error if anything in the binary calls sdmmc_init_spi_crc. Uncomment in Plan 2.
# target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdmmc_init_spi_crc")

# Activates pitch_detect_full() and audio_get_position_sec() in pitch.c / audio.c.
# Inert in Plan 1 (those files are not in SRCS yet). Uncomment in Plan 2 alongside
# the linker wrap when audio.c and pitch.c are added to SRCS.
# target_compile_definitions(${COMPONENT_LIB} PRIVATE PITCH_TEST_HARNESS=1)
```

Note: `json` is the cJSON component bundled with ESP-IDF. Include path is `cJSON.h`.
`INCLUDE_DIRS "../../main"` is inert in Plan 1 but avoids a CMakeLists edit in Plan 2.

- [ ] **Step 3: Write `test_harness/sdkconfig.defaults`**

```
CONFIG_ESP_WIFI_ENABLED=y
CONFIG_ESP_WIFI_SOFTAP_SUPPORT=n
CONFIG_MDNS_ENABLED=y
CONFIG_HTTPD_WS_SUPPORT=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
```

`CONFIG_HTTPD_WS_SUPPORT=y` is critical — without it `esp_http_server` compiles and
links but WebSocket upgrade requests silently fall through as plain HTTP with no error.
`CONFIG_ESP_TASK_WDT_TIMEOUT_S=10` prevents watchdog resets; `pitch_detect_full()` is
heavier than `pitch_detect()` and the default 5s timeout is marginal.
`CONFIG_MDNS_ENABLED=y` ensures the mdns component is included; without it
`mdns_init()` may be absent at runtime if a prior `sdkconfig` had it disabled.
WiFi options are defaults on ESP32 but listed explicitly to guarantee correctness.

- [ ] **Step 4: Write stub header and source files**

`test_harness/main/test_harness.h`:
```c
#pragma once
#include "esp_err.h"

typedef struct {
    float threshold_coeff;
    float pitch_min_hz;
    float pitch_max_hz;
    float smooth_alpha;
} tuner_params_t;

esp_err_t test_harness_init(void);
void      test_harness_get_params(tuner_params_t *out);
void      test_harness_set_params(const tuner_params_t *in);
```

`test_harness/main/test_harness.c`:
```c
#include "test_harness.h"

esp_err_t test_harness_init(void)                   { return ESP_OK; }
void      test_harness_get_params(tuner_params_t *o) { (void)o; }
void      test_harness_set_params(const tuner_params_t *i) { (void)i; }
```

`test_harness/main/wifi.h`:
```c
#pragma once
#include "esp_err.h"

esp_err_t wifi_init(void);
```

`test_harness/main/wifi.c`:
```c
#include "wifi.h"

esp_err_t wifi_init(void) { return ESP_OK; }
```

`test_harness/main/httpd.h`:
```c
#pragma once
#include "esp_err.h"

/* Named httpd_start_server() to avoid collision with IDF's httpd_start(). */
esp_err_t httpd_start_server(void);
```

`test_harness/main/httpd.c`:
```c
#include "httpd.h"

esp_err_t httpd_start_server(void) { return ESP_OK; }
```

`test_harness/main/main.c`:
```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void) {
    ESP_LOGI(TAG, "test harness starting");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
```

- [ ] **Step 5: Build to verify scaffold compiles**

From `test_harness/` directory:
```bash
idf.py build
```
Expected: Build completes, no errors. Unused-variable warnings on the stub files are acceptable.

If a component is not found (e.g., `mdns` or `json`), check it is spelled correctly in REQUIRES. In IDF 5.x, cJSON is `json`; mDNS is `mdns`.

- [ ] **Step 6: Commit**

```bash
git add test_harness/
git commit -m "feat: test_harness build scaffold (stubs only, not yet functional)"
```

---

### Task 2: WiFi station mode + mDNS

**Files:**
- Modify: `test_harness/main/wifi.h`
- Modify: `test_harness/main/wifi.c`
- Modify: `test_harness/main/main.c`

- [ ] **Step 1: Replace wifi.h with full interface**

```c
#pragma once
#include "esp_err.h"

/**
 * Connect to the riotscanner WiFi network in station mode.
 * Blocks until an IP address is assigned (no timeout).
 * Starts mDNS as cydtuner-test.local after IP is assigned.
 * Must be called after nvs_flash_init().
 */
esp_err_t wifi_init(void);
```

- [ ] **Step 2: Replace wifi.c with station mode + mDNS implementation**

```c
#include "wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mdns.h"
#include "esp_log.h"

#define WIFI_SSID          "riotscanner"
#define WIFI_PASS          "3213213213"
#define WIFI_CONNECTED_BIT BIT0

static const char        *TAG = "wifi";
static EventGroupHandle_t s_wifi_eg;

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static void mdns_setup(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("cydtuner-test"));
    ESP_ERROR_CHECK(mdns_instance_name_set("CydTuner Test Harness"));
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: cydtuner-test.local");
}

esp_err_t wifi_init(void)
{
    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_wifi, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &inst_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &inst_ip));

    wifi_config_t wifi_config = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Block until IP assigned */
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    mdns_setup();
    return ESP_OK;
}
```

- [ ] **Step 3: Replace main.c — call nvs_flash_init + wifi_init**

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "wifi.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "test harness starting");

    /* NVS required by WiFi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(wifi_init());
    /* httpd_start_server() added in Task 3 */

    ESP_LOGI(TAG, "WiFi up — HTTP server not yet started");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
```

- [ ] **Step 4: Build**

```bash
idf.py build
```
Expected: No errors.

- [ ] **Step 5: Flash + verify WiFi connects**

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Expected serial output within 10 seconds of boot:
```
I (xxx) wifi: IP: 192.168.x.x
I (xxx) wifi: mDNS: cydtuner-test.local
I (xxx) main: WiFi up — HTTP server not yet started
```

If IP never appears: check SSID/pass in wifi.c, verify the device is in range, and confirm NVS was erased (if flashing for the first time on this device, add `idf.py erase-flash` before `flash monitor`).

- [ ] **Step 6: Commit**

```bash
git add test_harness/main/wifi.h test_harness/main/wifi.c test_harness/main/main.c
git commit -m "feat: test_harness WiFi station mode + mDNS cydtuner-test.local"
```

---

### Task 3: /params REST endpoint + end-to-end verification

**Files:**
- Modify: `test_harness/main/test_harness.c` (full implementation)
- Modify: `test_harness/main/httpd.h` (full interface)
- Modify: `test_harness/main/httpd.c` (full implementation)
- Modify: `test_harness/main/main.c` (add httpd_start_server call)

- [ ] **Step 1: Replace test_harness.c with full params implementation**

```c
#include "test_harness.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char        *TAG = "test_harness";
static tuner_params_t     s_params;
static SemaphoreHandle_t  s_params_mutex;

esp_err_t test_harness_init(void)
{
    s_params = (tuner_params_t){
        .threshold_coeff = 0.8f,
        .pitch_min_hz    = 40.0f,
        .pitch_max_hz    = 1200.0f,
        .smooth_alpha    = 0.0f,
    };
    s_params_mutex = xSemaphoreCreateMutex();
    if (!s_params_mutex) {
        ESP_LOGE(TAG, "params mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void test_harness_get_params(tuner_params_t *out)
{
    xSemaphoreTake(s_params_mutex, portMAX_DELAY);
    *out = s_params;
    xSemaphoreGive(s_params_mutex);
}

void test_harness_set_params(const tuner_params_t *in)
{
    xSemaphoreTake(s_params_mutex, portMAX_DELAY);
    s_params = *in;
    xSemaphoreGive(s_params_mutex);
}
```

- [ ] **Step 2: Replace httpd.h with full interface**

```c
#pragma once
#include "esp_err.h"

/**
 * Start the HTTP server and register all URI handlers.
 * Must be called after wifi_init().
 * Registers: GET /params, POST /params
 */
esp_err_t httpd_start_server(void);
```

- [ ] **Step 3: Replace httpd.c with /params GET + POST handlers**

```c
#include "httpd.h"
#include "test_harness.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>

static const char *TAG = "httpd";

/* GET /params — return current tuner_params_t as JSON */
static esp_err_t params_get_handler(httpd_req_t *req)
{
    tuner_params_t p;
    test_harness_get_params(&p);

    char buf[160];
    int len = snprintf(buf, sizeof(buf),
        "{\"threshold_coeff\":%.4f,\"pitch_min_hz\":%.1f,"
        "\"pitch_max_hz\":%.1f,\"smooth_alpha\":%.4f}\n",
        (double)p.threshold_coeff, (double)p.pitch_min_hz,
        (double)p.pitch_max_hz,    (double)p.smooth_alpha);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* POST /params — accept partial JSON, update only provided fields */
static esp_err_t params_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    tuner_params_t p;
    test_harness_get_params(&p);

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "threshold_coeff")) && cJSON_IsNumber(item))
        p.threshold_coeff = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "pitch_min_hz")) && cJSON_IsNumber(item))
        p.pitch_min_hz = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "pitch_max_hz")) && cJSON_IsNumber(item))
        p.pitch_max_hz = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "smooth_alpha")) && cJSON_IsNumber(item))
        p.smooth_alpha = (float)item->valuedouble;

    cJSON_Delete(root);
    test_harness_set_params(&p);

    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t httpd_start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t get_params = {
        .uri = "/params", .method = HTTP_GET,
        .handler = params_get_handler, .user_ctx = NULL,
    };
    httpd_uri_t post_params = {
        .uri = "/params", .method = HTTP_POST,
        .handler = params_post_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &get_params);
    httpd_register_uri_handler(server, &post_params);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
```

- [ ] **Step 4: Replace main.c — add test_harness_init + httpd_start_server**

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "wifi.h"
#include "httpd.h"
#include "test_harness.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "test harness starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(test_harness_init());
    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(httpd_start_server());

    ESP_LOGI(TAG, "ready at http://cydtuner-test.local");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
```

- [ ] **Step 5: Build**

```bash
idf.py build
```
Expected: No errors.

- [ ] **Step 6: Flash + verify serial**

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Expected:
```
I (xxx) wifi: IP: 192.168.x.x
I (xxx) wifi: mDNS: cydtuner-test.local
I (xxx) httpd: HTTP server started
I (xxx) main: ready at http://cydtuner-test.local
```

- [ ] **Step 7: Verify /params endpoint**

From a machine on the same WiFi network:
```bash
curl http://cydtuner-test.local/params
# If mDNS doesn't resolve, use the IP from serial monitor:
curl http://192.168.x.x/params
```

Expected response (HTTP 200, Content-Type application/json):
```json
{"threshold_coeff":0.8000,"pitch_min_hz":40.0,"pitch_max_hz":1200.0,"smooth_alpha":0.0000}
```

- [ ] **Step 8: Commit + push**

```bash
git add test_harness/
git commit -m "feat: test_harness HTTP /params endpoint — reachable at cydtuner-test.local"
git push
```

If `test_harness/sdkconfig` was generated by the build, stage it in a follow-up commit:
```bash
git add test_harness/sdkconfig
git commit -m "chore: track test_harness sdkconfig (private repo)"
git push
```

---

## End State

After Plan 1, the second ESP32:
- Boots, connects to riotscanner WiFi
- Registers as `cydtuner-test.local`
- Responds to `GET http://cydtuner-test.local/params` with the default tuner params JSON
- Accepts `POST /params` with a partial JSON body to update params

Audio/pitch tasks and the full instrumented pipeline are added in Plan 2.
