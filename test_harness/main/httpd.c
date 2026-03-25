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
