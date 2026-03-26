#include "httpd.h"
#include "test_harness.h"
#include "audio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

extern void main_get_diag(int *q_waiting, int *sem_count,
                           uint32_t *audio_hwm, uint32_t *pitch_hwm, uint32_t *display_hwm);

static const char *TAG = "httpd";

static httpd_handle_t s_server = NULL;

/* ---- Snapshot buffer — allocated in httpd_start_server() before tasks ---- */
#define SNAP_BUFSZ 12288   /* reduced from 20480 — saves 8 KB heap */
static char *s_snap_buf = NULL;

/* ---- WebSocket client list ----------------------------------------------- */
#define MAX_WS_CLIENTS 4
static int               s_ws_fds[MAX_WS_CLIENTS];
static int               s_ws_n = 0;
static SemaphoreHandle_t s_ws_mutex;

static void ws_add_client(int fd)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    if (s_ws_n < MAX_WS_CLIENTS) s_ws_fds[s_ws_n++] = fd;
    xSemaphoreGive(s_ws_mutex);
}

/* ---- Compact frame JSON helper ------------------------------------------- */
static int compact_to_json(const compact_frame_t *f, char *buf, size_t sz)
{
    return snprintf(buf, sz,
        "{\"ts_us\":%llu,\"detected_hz\":%.4f,\"smooth_hz\":%.4f,"
        "\"note\":\"%s\",\"cents\":%.3f,\"ground_truth_hz\":%.4f,"
        "\"cents_error\":%.3f,\"nsdf_peak_val\":%.4f,"
        "\"nsdf_global_max\":%.4f,\"threshold_used\":%.4f,"
        "\"tau_detected\":%u,\"tau_interpolated\":%.4f,\"nsdf_len\":%u}",
        (unsigned long long)f->timestamp_us,
        (double)f->detected_hz, (double)f->smooth_hz,
        f->note,
        (double)f->cents, (double)f->ground_truth_hz,
        (double)f->cents_error, (double)f->nsdf_peak_val,
        (double)f->nsdf_global_max, (double)f->threshold_used,
        (unsigned)f->tau_detected, (double)f->tau_interpolated,
        (unsigned)f->nsdf_len);
}

/* ---- WebSocket broadcast (called from httpd task via httpd_queue_work) ---- */
static void ws_broadcast_cb(void *arg)
{
    compact_frame_t f;
    test_harness_get_latest_compact(&f);

    char buf[512];
    int len = compact_to_json(&f, buf, sizeof(buf));
    if (len <= 0 || len >= (int)sizeof(buf)) return;

    httpd_ws_frame_t pkt = {
        .final = true, .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len = (size_t)len,
    };

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < s_ws_n; ) {
        esp_err_t ret = httpd_ws_send_frame_async(s_server, s_ws_fds[i], &pkt);
        if (ret != ESP_OK) {
            /* Stale fd — remove by swapping with last */
            s_ws_fds[i] = s_ws_fds[--s_ws_n];
        } else {
            i++;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

/* ---- ws_sender_task ------------------------------------------------------ */
static void ws_sender_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)test_harness_get_ws_notify_q();
    int64_t last_send_us = 0;
    uint8_t sig;

    for (;;) {
        if (xQueueReceive(q, &sig, pdMS_TO_TICKS(100)) != pdTRUE) continue;

        /* Drain any extra pending signals */
        while (xQueueReceive(q, &sig, 0) == pdTRUE) {}

        int64_t now = esp_timer_get_time();
        if (now - last_send_us < 100000) continue;  /* rate limit ~10Hz */
        last_send_us = now;

        if (s_server && s_ws_n > 0)
            httpd_queue_work(s_server, ws_broadcast_cb, NULL);
    }
}

/* ---- GET /params --------------------------------------------------------- */
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

/* ---- POST /params -------------------------------------------------------- */
static esp_err_t params_post_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    body[received] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    tuner_params_t p;
    test_harness_get_params(&p);
    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "threshold_coeff")) && cJSON_IsNumber(item)) p.threshold_coeff = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "pitch_min_hz"))    && cJSON_IsNumber(item)) p.pitch_min_hz    = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "pitch_max_hz"))    && cJSON_IsNumber(item)) p.pitch_max_hz    = (float)item->valuedouble;
    if ((item = cJSON_GetObjectItem(root, "smooth_alpha"))    && cJSON_IsNumber(item)) p.smooth_alpha    = (float)item->valuedouble;
    cJSON_Delete(root);
    test_harness_set_params(&p);
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, NULL, 0);
}

/* ---- GET /snapshot — full frame + NSDF array ----------------------------- */
static esp_err_t snapshot_get_handler(httpd_req_t *req)
{
    if (!s_snap_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no buf");
        return ESP_FAIL;
    }

    static pitch_frame_t s_snap;
    test_harness_get_latest_frame(&s_snap);

    int pos = snprintf(s_snap_buf, SNAP_BUFSZ,
        "{\"ts_us\":%llu,\"detected_hz\":%.4f,\"smooth_hz\":%.4f,"
        "\"note\":\"%s\",\"cents\":%.3f,\"ground_truth_hz\":%.4f,"
        "\"cents_error\":%.3f,\"nsdf_peak_val\":%.4f,"
        "\"nsdf_global_max\":%.4f,\"threshold_used\":%.4f,"
        "\"tau_detected\":%u,\"tau_interpolated\":%.4f,\"nsdf_len\":%u,"
        "\"nsdf\":[",
        (unsigned long long)s_snap.timestamp_us,
        (double)s_snap.detected_hz,     (double)s_snap.smooth_hz,
        s_snap.note,
        (double)s_snap.cents,           (double)s_snap.ground_truth_hz,
        (double)s_snap.cents_error,     (double)s_snap.nsdf_peak_val,
        (double)s_snap.nsdf_global_max, (double)s_snap.threshold_used,
        (unsigned)s_snap.tau_detected,  (double)s_snap.tau_interpolated,
        (unsigned)s_snap.nsdf_len);

    for (uint16_t i = 0; i < s_snap.nsdf_len && pos < (int)(SNAP_BUFSZ - 20); i++) {
        pos += snprintf(s_snap_buf + pos, SNAP_BUFSZ - pos, "%.5f", (double)s_snap.nsdf[i]);
        if (i < s_snap.nsdf_len - 1 && pos < (int)(SNAP_BUFSZ - 5))
            s_snap_buf[pos++] = ',';
    }
    s_snap_buf[pos++] = ']';
    s_snap_buf[pos++] = '}';

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, s_snap_buf, pos);
}

/* ---- GET /stats ---------------------------------------------------------- */
static esp_err_t stats_get_handler(httpd_req_t *req)
{
    static compact_frame_t hist[HISTORY_LEN];
    int n = test_harness_get_history(hist, HISTORY_LEN);
    if (n == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"n\":0}");
    }

    double sum_hz = 0, sum_hz2 = 0, min_hz = 1e9, max_hz = 0;
    double sum_ce = 0, sum_ce2 = 0, min_ce = 1e9, max_ce = -1e9;
    double sum_pk = 0, sum_pk2 = 0;
    int n_hz = 0, n_gt = 0;

    for (int i = 0; i < n; i++) {
        if (hist[i].detected_hz <= 0.0f) continue;
        double hz = hist[i].detected_hz;
        sum_hz += hz; sum_hz2 += hz * hz;
        if (hz < min_hz) min_hz = hz;
        if (hz > max_hz) max_hz = hz;
        double pk = hist[i].nsdf_peak_val;
        sum_pk += pk; sum_pk2 += pk * pk;
        n_hz++;
        if (hist[i].ground_truth_hz > 0.0f) {
            double ce = hist[i].cents_error;
            sum_ce += ce; sum_ce2 += ce * ce;
            if (ce < min_ce) min_ce = ce;
            if (ce > max_ce) max_ce = ce;
            n_gt++;
        }
    }

    if (n_hz == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"n\":0,\"n_with_gt\":0}");
    }

    double mean_hz = sum_hz  / n_hz;
    double std_hz  = sqrt(sum_hz2  / n_hz - mean_hz  * mean_hz);
    double mean_pk = sum_pk  / n_hz;
    double std_pk  = sqrt(sum_pk2  / n_hz - mean_pk  * mean_pk);
    double mean_ce = n_gt > 0 ? sum_ce / n_gt : 0;
    double std_ce  = n_gt > 0 ? sqrt(sum_ce2 / n_gt - mean_ce * mean_ce) : 0;

    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "{\"n\":%d,\"n_detected\":%d,\"n_with_gt\":%d,"
        "\"detected_hz\":{\"mean\":%.4f,\"std\":%.4f,\"min\":%.4f,\"max\":%.4f},"
        "\"cents_error\":{\"mean\":%.3f,\"std\":%.3f,\"min\":%.3f,\"max\":%.3f},"
        "\"nsdf_peak_val\":{\"mean\":%.4f,\"std\":%.4f}}",
        n, n_hz, n_gt,
        mean_hz, std_hz, min_hz, max_hz,
        mean_ce, std_ce, n_gt > 0 ? min_ce : 0, n_gt > 0 ? max_ce : 0,
        mean_pk, std_pk);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ---- GET /history -------------------------------------------------------- */
static esp_err_t history_get_handler(httpd_req_t *req)
{
    char qbuf[16] = {0};
    int max_n = 50;
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char nval[8] = {0};
        if (httpd_query_key_value(qbuf, "n", nval, sizeof(nval)) == ESP_OK)
            max_n = atoi(nval);
    }
    if (max_n < 1)        max_n = 1;
    if (max_n > HISTORY_LEN) max_n = HISTORY_LEN;

    static compact_frame_t hist[HISTORY_LEN];
    int n = test_harness_get_history(hist, max_n);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "[");
    char entry[512];
    for (int i = 0; i < n; i++) {
        compact_to_json(&hist[i], entry, sizeof(entry));
        httpd_resp_sendstr_chunk(req, entry);
        if (i < n - 1) httpd_resp_sendstr_chunk(req, ",");
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---- GET /ws (WebSocket) ------------------------------------------------- */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ws_add_client(fd);
        ESP_LOGI(TAG, "WS client connected fd=%d (total=%d)", fd, s_ws_n);
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {.type = HTTPD_WS_TYPE_TEXT};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK || pkt.len == 0) return ret;

    uint8_t *payload = malloc(pkt.len + 1);
    if (!payload) return ESP_ERR_NO_MEM;
    pkt.payload = payload;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    payload[pkt.len] = '\0';

    if (ret == ESP_OK && strstr((char *)payload, "snapshot"))
        httpd_queue_work(s_server, ws_broadcast_cb, NULL);

    free(payload);
    return ret;
}

/* ---- POST /history/clear ------------------------------------------------- */
static esp_err_t history_clear_handler(httpd_req_t *req)
{
    test_harness_history_clear();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ---- GET /synth ---------------------------------------------------------- */
static esp_err_t synth_get_handler(httpd_req_t *req)
{
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "{\"hz\":%.4f}\n", (double)audio_synth_get_hz());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ---- POST /synth --------------------------------------------------------- */
static esp_err_t synth_post_handler(httpd_req_t *req)
{
    char body[64] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body"); return ESP_FAIL; }
    body[received] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    cJSON *item = cJSON_GetObjectItem(root, "hz");
    if (item && cJSON_IsNumber(item)) audio_synth_set_hz((float)item->valuedouble);
    cJSON_Delete(root);
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, NULL, 0);
}

/* ---- GET /diag ----------------------------------------------------------- */
static esp_err_t diag_get_handler(httpd_req_t *req)
{
    int q_waiting, sem_count;
    uint32_t audio_hwm, pitch_hwm, display_hwm;
    main_get_diag(&q_waiting, &sem_count, &audio_hwm, &pitch_hwm, &display_hwm);

    char buf[384];
    int len = snprintf(buf, sizeof(buf),
        "{\"uptime_ms\":%llu"
        ",\"heap_free\":%u,\"heap_min\":%u"
        ",\"dma_free\":%u,\"dma_min\":%u"
        ",\"sample_q_waiting\":%d,\"buf_sem_count\":%d"
        ",\"audio_stack_hwm\":%lu,\"pitch_stack_hwm\":%lu,\"display_stack_hwm\":%lu}",
        (unsigned long long)(esp_timer_get_time() / 1000),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
        q_waiting, sem_count,
        audio_hwm, pitch_hwm, display_hwm);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, len);
}

/* ---- httpd_start_server -------------------------------------------------- */
esp_err_t httpd_start_server(void)
{
    s_snap_buf = malloc(SNAP_BUFSZ);
    if (!s_snap_buf) { ESP_LOGE(TAG, "snap buf alloc failed"); return ESP_ERR_NO_MEM; }

    s_ws_mutex = xSemaphoreCreateMutex();
    if (!s_ws_mutex) return ESP_ERR_NO_MEM;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable  = true;
    config.max_open_sockets  = 2;
    config.max_uri_handlers  = 12;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err)); return err; }

    httpd_uri_t uris[] = {
        { .uri = "/diag",     .method = HTTP_GET,  .handler = diag_get_handler     },
        { .uri = "/params",   .method = HTTP_GET,  .handler = params_get_handler   },
        { .uri = "/params",   .method = HTTP_POST, .handler = params_post_handler  },
        { .uri = "/snapshot", .method = HTTP_GET,  .handler = snapshot_get_handler },
        { .uri = "/stats",    .method = HTTP_GET,  .handler = stats_get_handler    },
        { .uri = "/history",  .method = HTTP_GET,  .handler = history_get_handler  },
        { .uri = "/history/clear", .method = HTTP_POST, .handler = history_clear_handler },
        { .uri = "/synth",    .method = HTTP_GET,  .handler = synth_get_handler    },
        { .uri = "/synth",    .method = HTTP_POST, .handler = synth_post_handler   },
        { .uri = "/ws",       .method = HTTP_GET,  .handler = ws_handler,
          .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(uris)/sizeof(uris[0]); i++)
        httpd_register_uri_handler(s_server, &uris[i]);

    xTaskCreatePinnedToCore(ws_sender_task, "ws_send", 4096*2, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "HTTP server started (/params /snapshot /stats /history /synth /ws)");
    return ESP_OK;
}

httpd_handle_t httpd_get_server(void) { return s_server; }
