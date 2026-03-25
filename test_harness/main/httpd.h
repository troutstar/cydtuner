#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Start the HTTP server and register all URI handlers.
 * Must be called after wifi_init() and test_harness_init().
 *
 * Registers:
 *   GET  /params    — current tuner_params_t as JSON
 *   POST /params    — update params (partial JSON accepted)
 *   GET  /snapshot  — full pitch_frame_t as chunked JSON (includes NSDF array)
 *   GET  /stats     — rolling stats over last 200 readings
 *   GET  /history   — last N compact frames (?n=50, max 200)
 *   GET  /ws        — WebSocket (compact frames pushed at ~10Hz)
 *
 * Also starts ws_sender_task which drives WebSocket broadcasts.
 */
esp_err_t httpd_start_server(void);

/** Returns the server handle. */
httpd_handle_t httpd_get_server(void);
