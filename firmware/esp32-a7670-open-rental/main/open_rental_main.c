/*
 * ESP32 + SIMCom A7670E — cellular MQTT (SIM7600-style AT+CMQTT*), GNSS telemetry,
 * and LED control via backend MQTT commands.
 *
 * Backend publishes: {"cmd":"led","on":true|false}, {"cmd":"ussd","code":"*310#","requestId":"..."},
 * {"cmd":"sms","to":"...","text":"...","requestId":"..."} on MQTT_TOPIC_COMMANDS.
 * USSD/SMS replies are published as JSON on MQTT_TOPIC_REPLIES.
 * This device subscribes and toggles LED_GPIO_NUM.
 *
 * ============================================================
 * USSD NOTES (A7670E / LTE-only modules):
 * ============================================================
 *  Most A7670E variants are LTE-only and do NOT support Circuit-Switched
 *  (CS) voice. USSD on LTE requires the network to support IMS-USSD or
 *  route USSD over PS (packet-switched). Many carriers do NOT support
 *  this.  If AT+CUSD=1,"*XXX#" consistently times out:
 *    1. Confirm the code works from a regular phone on the same SIM.
 *    2. Check if your carrier supports VoLTE/IMS USSD.
 *    3. As a fallback, use the modem_ussd_via_at_cusd2() variant which
 *       tries both n=1 and n=2 modes.
 *
 * SMS NOTES:
 *  - Uses TEXT mode (AT+CMGF=1) with GSM encoding.
 *  - Ctrl-Z (0x1A) terminates the message body.
 *  - AT+CMGS reports +CMGS:<ref> on success, +CMS ERROR on failure.
 *  - Storage set to "SM" (SIM) to avoid full "ME" (modem) memory blocking sends.
 *
 * MQTT AT syntax follows SIM7500/SIM7600 MQTT AT Command Manual.
 * Build: ESP-IDF v5.x, target esp32.
 */

 #include <math.h>
 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 
 #include "driver/gpio.h"
 #include "driver/uart.h"
 #include "esp_log.h"
 #include "esp_timer.h"
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 #include "freertos/portmacro.h"
 
 static const char *TAG = "OPEN_RENTAL";
 
 /* --------- User configuration --------- */
 
 #define MQTT_BROKER_HOST "51.91.97.149"
 #define MQTT_BROKER_PORT 1883
 
 #define MQTT_TOPIC_TELEMETRY "open-rental/esp/telemetry"
 #define MQTT_TOPIC_COMMANDS "open-rental/esp/commands"
 #define MQTT_TOPIC_REPLIES "open-rental/esp/replies"
 
 #define MQTT_CLIENT_ID "esp32-open-rental"
 
 /** Replace with your carrier APN. */
 #define CELLULAR_APN "internet"
 
 #define MODEM_UART_NUM UART_NUM_2
 #define MODEM_TX_PIN GPIO_NUM_17
 #define MODEM_RX_PIN GPIO_NUM_16
 #define MODEM_BAUDRATE 115200
 
 #define MODEM_PWRKEY GPIO_NUM_4
 
 /** Onboard LED on many ESP32 devkits. */
 #define LED_GPIO_NUM GPIO_NUM_2
 
 #define GNSS_POLL_INTERVAL_MS 5000
 
 #define MQTT_CLIENT_INDEX 0
 #define MQTT_QOS_SUBSCRIBE 1
 #define MQTT_QOS_PUBLISH 0
 #define MQTT_PUBLISH_TIMEOUT_SEC 60
 #define MQTT_KEEPALIVE_SEC 120
 
 /*
  * USSD network timeout.
  * LTE USSD via IMS can be slow — 60 s is typical for roaming.
  * Increase if your carrier is slower.
  */
 #ifndef USSD_NETWORK_WAIT_MS
 #define USSD_NETWORK_WAIT_MS 90000
 #endif
 
 /* Minimum gap between successful telemetry publishes. */
 #ifndef TELEMETRY_MIN_OK_INTERVAL_MS
 #define TELEMETRY_MIN_OK_INTERVAL_MS 8000
 #endif
 #ifndef TELEMETRY_RETRY_AFTER_FAIL_MS
 #define TELEMETRY_RETRY_AFTER_FAIL_MS 3500
 #endif
 
 /* ================================================================
  * MODEM UART
  * ================================================================ */
 
 static void modem_send_line(const char *cmd)
 {
     uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
     uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);
     ESP_LOGD(TAG, ">> %s", cmd);
 }
 
 static void uart_flush_rx(void)
 {
     uint8_t tmp[256];
     while (uart_read_bytes(MODEM_UART_NUM, tmp, sizeof(tmp), pdMS_TO_TICKS(50)) > 0)
     {
     }
 }
 
 /* ----------------------------------------------------------------
  * Line accumulator — splits raw bytes into logical lines,
  * triggers URC dispatch (MQTT PUSH, USSD unsolicited, etc.).
  * ---------------------------------------------------------------- */
 
 static char s_line_acc[1536];
 static size_t s_line_pos;
 
 static void handle_mqtt_json_command(const char *json); /* forward */
 
 static void finish_line(void)
 {
     if (s_line_pos == 0)
         return;
     s_line_acc[s_line_pos] = '\0';
 
     /* trim trailing CR */
     while (s_line_pos > 0 &&
            (s_line_acc[s_line_pos - 1] == '\r' || s_line_acc[s_line_pos - 1] == '\n'))
     {
         s_line_acc[--s_line_pos] = '\0';
     }
 
     if (s_line_pos > 0)
     {
         ESP_LOGD(TAG, "LINE: %s", s_line_acc);
 
         /*
          * After +CMQTTRXPAYLOAD: len — the very next complete line is the
          * raw JSON payload (for short payloads that fit in one line).
          */
         static bool s_expect_payload_body;
         if (s_expect_payload_body)
         {
             s_expect_payload_body = false;
             if (s_line_acc[0] == '{')
             {
                 handle_mqtt_json_command(s_line_acc);
             }
             goto reset;
         }
         if (strncmp(s_line_acc, "+CMQTTRXPAYLOAD:", 16) == 0)
         {
             s_expect_payload_body = true;
         }
     }
 
 reset:
     s_line_pos = 0;
 }
 
 static void feed_modem_bytes(const uint8_t *data, size_t len)
 {
     for (size_t i = 0; i < len; i++)
     {
         char c = (char)data[i];
         if (c == '\r')
             continue;
         if (c == '\n')
         {
             finish_line();
             continue;
         }
         if (s_line_pos + 1 < sizeof(s_line_acc))
         {
             s_line_acc[s_line_pos++] = c;
         }
     }
 }
 
 /* ----------------------------------------------------------------
  * Shared drain / wait helpers
  * ---------------------------------------------------------------- */
 
 static char s_drain_buf[3072];
 
 /** Line is exactly "ERROR" (not +CMS ERROR / +CGNSSINFO: ERROR / +CME ERROR). */
 static bool drain_buf_has_standalone_error_line(const char *buf)
 {
     const char *p = buf;
 
     while (p != NULL && *p != '\0') {
         const char *nl = strchr(p, '\n');
         const char *line_end = nl != NULL ? nl : p + strlen(p);
         const char *s = p;
 
         while (s < line_end && (*s == ' ' || *s == '\t')) {
             s++;
         }
         const char *e = line_end;
 
         while (e > s && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) {
             e--;
         }
         size_t n = (size_t)(e - s);
 
         if (n == 5 && memcmp(s, "ERROR", 5) == 0) {
             return true;
         }
         if (nl == NULL) {
             break;
         }
         p = nl + 1;
     }
     return false;
 }
 
 /**
  * Wait for needle — does not abort on ERROR lines (for AT+CUSD=2, MQTT teardown).
  */
 static bool modem_drain_discard_until(const char *needle, int timeout_ms)
 {
     size_t total = 0;
     s_drain_buf[0] = '\0';
     int64_t deadline_ms = esp_timer_get_time() / 1000 + timeout_ms;
 
     while (esp_timer_get_time() / 1000 < deadline_ms) {
         uint8_t chunk[256];
         int n = uart_read_bytes(MODEM_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(80));
         if (n <= 0) {
             continue;
         }
 
         feed_modem_bytes(chunk, (size_t)n);
 
         size_t copy = (size_t)n;
         if (total + copy >= sizeof(s_drain_buf)) {
             copy = sizeof(s_drain_buf) - 1 - total;
         }
         if (copy > 0) {
             memcpy(s_drain_buf + total, chunk, copy);
             total += copy;
             s_drain_buf[total] = '\0';
         }
 
         if (strstr(s_drain_buf, needle)) {
             return true;
         }
     }
     s_drain_buf[total] = '\0';
     return strstr(s_drain_buf, needle) != NULL;
 }
 
 /**
  * Read until `needle` or timeout. Always starts from an empty capture buffer.
  * Aborts only on a standalone AT ERROR line (not +CMS/+CME/+CGNSSINFO: …).
  */
 static bool modem_drain_until(const char *needle, int timeout_ms, bool *error_token)
 {
     size_t total = 0;
     s_drain_buf[0] = '\0';
     *error_token = false;
     int64_t deadline_ms = esp_timer_get_time() / 1000 + timeout_ms;
 
     while (esp_timer_get_time() / 1000 < deadline_ms) {
         uint8_t chunk[256];
         int n = uart_read_bytes(MODEM_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(80));
         if (n <= 0) {
             continue;
         }
 
         feed_modem_bytes(chunk, (size_t)n);
 
         size_t copy = (size_t)n;
         if (total + copy >= sizeof(s_drain_buf)) {
             copy = sizeof(s_drain_buf) - 1 - total;
         }
         if (copy > 0) {
             memcpy(s_drain_buf + total, chunk, copy);
             total += copy;
             s_drain_buf[total] = '\0';
         }
 
         if (strstr(s_drain_buf, needle)) {
             return true;
         }
         if (drain_buf_has_standalone_error_line(s_drain_buf)) {
             *error_token = true;
             ESP_LOGW(TAG, "Modem standalone ERROR line");
             return false;
         }
     }
     s_drain_buf[total] = '\0';
     return strstr(s_drain_buf, needle) != NULL;
 }
 
 /**
  * Read until `needle` or timeout — does NOT check for "ERROR".
  * Use when waiting for an interactive prompt (">").
  */
 static bool modem_wait_prompt(const char *needle, int timeout_ms)
 {
     size_t total = 0;
     s_drain_buf[0] = '\0';
     int64_t deadline_ms = esp_timer_get_time() / 1000 + timeout_ms;
 
     while (esp_timer_get_time() / 1000 < deadline_ms)
     {
         uint8_t chunk[128];
         int n = uart_read_bytes(MODEM_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(80));
         if (n <= 0)
             continue;
 
         feed_modem_bytes(chunk, (size_t)n);
 
         size_t copy = (size_t)n;
         if (total + copy >= sizeof(s_drain_buf))
             copy = sizeof(s_drain_buf) - 1 - total;
         if (copy > 0)
         {
             memcpy(s_drain_buf + total, chunk, copy);
             total += copy;
             s_drain_buf[total] = '\0';
         }
         if (strstr(s_drain_buf, needle))
             return true;
         if (drain_buf_has_standalone_error_line(s_drain_buf))
             return false;
     }
     return strstr(s_drain_buf, needle) != NULL;
 }
 
 /** Send AT header, wait for ">", send body, wait for OK. */
 static bool modem_interactive_body_timeout(const char *at_header, const char *body,
                                            int prompt_ms, int ok_ms)
 {
     uart_flush_rx();
     modem_send_line(at_header);
 
     if (!modem_wait_prompt(">", prompt_ms))
     {
         ESP_LOGE(TAG, "No > prompt after: %s", at_header);
         return false;
     }
 
     uart_write_bytes(MODEM_UART_NUM, body, strlen(body));
     uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);
 
     bool err = false;
     if (!modem_drain_until("OK", ok_ms, &err))
     {
         ESP_LOGE(TAG, "No OK after interactive body for: %s", at_header);
         return false;
     }
     return !err;
 }
 
 static bool modem_interactive_body(const char *at_header, const char *body)
 {
     /* Large timeouts: URC bursts (GNSS, CMQTT) can delay the ">" prompt. */
     return modem_interactive_body_timeout(at_header, body, 35000, 20000);
 }
 
 /** Send AT, expect OK — used to verify command mode. */
 static bool modem_sync_at_ok(void)
 {
     bool err = false;
     uart_flush_rx();
     modem_send_line("AT");
     return modem_drain_until("OK", 4000, &err) && !err;
 }
 
 /**
  * Exit any interactive mode (ESC flush), then confirm AT is alive.
  * Call after USSD or SMS so CMQTT state is clean.
  */
 static void modem_recover_command_mode(void)
 {
     uint8_t esc = 0x1b;
     uart_flush_rx();
     s_drain_buf[0] = '\0'; /* Clear any stale data in the drain buffer */
     uart_write_bytes(MODEM_UART_NUM, &esc, 1);
     vTaskDelay(pdMS_TO_TICKS(150));
     uart_flush_rx();
     s_drain_buf[0] = '\0'; /* Clear again after UART flush */
 
     for (int i = 0; i < 5; i++)
     {
         bool err = false;
         uart_flush_rx();
         s_drain_buf[0] = '\0'; /* Clear before each attempt */
         modem_send_line("AT");
         if (modem_drain_until("OK", 5000, &err) && !err)
             return;
         vTaskDelay(pdMS_TO_TICKS(300));
     }
     ESP_LOGW(TAG, "modem_recover_command_mode: AT not responding");
 }
 
 /* ================================================================
  * LED / JSON command dispatch
  * ================================================================ */
 
 static void led_apply(bool on)
 {
     gpio_set_level(LED_GPIO_NUM, on ? 1 : 0);
     ESP_LOGI(TAG, "LED %s", on ? "ON" : "OFF");
 }
 
 /* ================================================================
  * Pending modem job queue (USSD / SMS)
  * ================================================================ */
 
 typedef enum
 {
     PENDING_NONE = 0,
     PENDING_USSD,
     PENDING_SMS,
 } pending_kind_t;
 
 typedef struct
 {
     pending_kind_t kind;
     char request_id[48];
     char ussd_code[48];
     char sms_to[32];
     char sms_text[280];
 } pending_modem_t;
 
 static pending_modem_t s_pending;
 static portMUX_TYPE s_pending_mux = portMUX_INITIALIZER_UNLOCKED;
 
 static bool pending_modem_work_pending(void)
 {
     bool q;
     taskENTER_CRITICAL(&s_pending_mux);
     q = (s_pending.kind != PENDING_NONE);
     taskEXIT_CRITICAL(&s_pending_mux);
     return q;
 }
 
 /* ----------------------------------------------------------------
  * Tiny JSON helpers (no dynamic allocation)
  * ---------------------------------------------------------------- */
 
 static bool json_extract_str(const char *json, const char *key,
                              char *out, size_t out_len)
 {
     char pat[56];
     snprintf(pat, sizeof(pat), "\"%s\":\"", key);
     const char *p = strstr(json, pat);
     if (!p)
         return false;
     p += strlen(pat);
     size_t i = 0;
     while (*p && *p != '"' && i + 1 < out_len)
         out[i++] = *p++;
     out[i] = '\0';
     return i > 0;
 }
 
 static void sanitize_json_token_inplace(char *s)
 {
     for (; *s; s++)
     {
         if (*s == '"' || *s == '\\' || *s < 0x20)
             *s = '_';
     }
 }
 
 static void json_escape_str(const char *in, char *out, size_t out_sz)
 {
     size_t j = 0;
     for (size_t i = 0; in[i] && j + 1 < out_sz; i++)
     {
         unsigned char c = (unsigned char)in[i];
         if (c == '"' || c == '\\')
         {
             if (j + 2 >= out_sz)
                 break;
             out[j++] = '\\';
             out[j++] = (char)c;
         }
         else if (c == '\r' || c == '\n')
         {
             if (j + 1 >= out_sz)
                 break;
             out[j++] = ' ';
         }
         else if (c < 0x20)
         {
             if (j + 1 >= out_sz)
                 break;
             out[j++] = '?';
         }
         else
         {
             out[j++] = (char)c;
         }
     }
     out[j] = '\0';
 }
 
 /* ----------------------------------------------------------------
  * Queue helpers
  * ---------------------------------------------------------------- */
 
 static void queue_pending_ussd(const char *req_id, const char *code)
 {
     taskENTER_CRITICAL(&s_pending_mux);
     if (s_pending.kind != PENDING_NONE)
     {
         taskEXIT_CRITICAL(&s_pending_mux);
         ESP_LOGW(TAG, "USSD queuing ignored — job already pending");
         return;
     }
     memset(&s_pending, 0, sizeof(s_pending));
     snprintf(s_pending.request_id, sizeof(s_pending.request_id), "%s", req_id);
     snprintf(s_pending.ussd_code, sizeof(s_pending.ussd_code), "%s", code);
     s_pending.kind = PENDING_USSD;
     taskEXIT_CRITICAL(&s_pending_mux);
     ESP_LOGI(TAG, "USSD queued: code=%s reqId=%s", code, req_id);
 }
 
 static void queue_pending_sms(const char *req_id, const char *to, const char *text)
 {
     taskENTER_CRITICAL(&s_pending_mux);
     if (s_pending.kind != PENDING_NONE)
     {
         taskEXIT_CRITICAL(&s_pending_mux);
         ESP_LOGW(TAG, "SMS queuing ignored — job already pending");
         return;
     }
     memset(&s_pending, 0, sizeof(s_pending));
     snprintf(s_pending.request_id, sizeof(s_pending.request_id), "%s", req_id);
     snprintf(s_pending.sms_to, sizeof(s_pending.sms_to), "%s", to);
     snprintf(s_pending.sms_text, sizeof(s_pending.sms_text), "%s", text);
     s_pending.kind = PENDING_SMS;
     taskEXIT_CRITICAL(&s_pending_mux);
     ESP_LOGI(TAG, "SMS queued: to=%s reqId=%s", to, req_id);
 }
 
 static void handle_mqtt_json_command(const char *json)
 {
     /* LED */
     if (strstr(json, "\"cmd\"") && strstr(json, "\"led\""))
     {
         if (strstr(json, "\"on\":true") || strstr(json, "\"on\": true"))
         {
             led_apply(true);
             return;
         }
         if (strstr(json, "\"on\":false") || strstr(json, "\"on\": false"))
         {
             led_apply(false);
             return;
         }
         ESP_LOGW(TAG, "LED JSON missing on/state: %s", json);
         return;
     }
 
     char req_id[48] = "unknown";
     json_extract_str(json, "requestId", req_id, sizeof(req_id));
 
     /* USSD */
     if (strstr(json, "\"cmd\"") && strstr(json, "\"ussd\""))
     {
         char code[48] = {0};
         if (json_extract_str(json, "code", code, sizeof(code)))
         {
             queue_pending_ussd(req_id, code);
         }
         else
         {
             ESP_LOGW(TAG, "USSD cmd missing 'code': %s", json);
         }
         return;
     }
 
     /* SMS */
     if (strstr(json, "\"cmd\"") && strstr(json, "\"sms\""))
     {
         char to[32] = {0};
         char text[280] = {0};
         bool got_to = json_extract_str(json, "to", to, sizeof(to));
         bool got_text = json_extract_str(json, "text", text, sizeof(text));
         if (got_to && got_text)
         {
             queue_pending_sms(req_id, to, text);
         }
         else
         {
             ESP_LOGW(TAG, "SMS cmd missing 'to' or 'text': %s", json);
         }
         return;
     }
 
     ESP_LOGW(TAG, "Unknown MQTT JSON (ignored): %.200s", json);
 }
 
 static void led_gpio_init(void)
 {
     gpio_reset_pin(LED_GPIO_NUM);
     gpio_set_direction(LED_GPIO_NUM, GPIO_MODE_OUTPUT);
     gpio_set_level(LED_GPIO_NUM, 0);
 }
 
 /* ================================================================
  * Modem power + UART init
  * ================================================================ */
 
 static void modem_power_on(void)
 {
     gpio_config_t io_conf = {
         .pin_bit_mask = (1ULL << MODEM_PWRKEY),
         .mode = GPIO_MODE_OUTPUT,
     };
     gpio_config(&io_conf);
 
     gpio_set_level(MODEM_PWRKEY, 0);
     vTaskDelay(pdMS_TO_TICKS(100));
     gpio_set_level(MODEM_PWRKEY, 1);
     vTaskDelay(pdMS_TO_TICKS(1000));
     gpio_set_level(MODEM_PWRKEY, 0);
     ESP_LOGI(TAG, "Modem PWRKEY pulse done");
 }
 
 static void modem_uart_init(void)
 {
     uart_config_t cfg = {
         .baud_rate = MODEM_BAUDRATE,
         .data_bits = UART_DATA_8_BITS,
         .parity = UART_PARITY_DISABLE,
         .stop_bits = UART_STOP_BITS_1,
         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
         .source_clk = UART_SCLK_DEFAULT,
     };
     uart_driver_install(MODEM_UART_NUM, 4096, 0, 0, NULL, 0);
     uart_param_config(MODEM_UART_NUM, &cfg);
     uart_set_pin(MODEM_UART_NUM, MODEM_TX_PIN, MODEM_RX_PIN,
                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
 }
 
 static void send_at_command(const char *cmd)
 {
     modem_send_line(cmd);
 }
 
 static void read_response_log(void)
 {
     uint8_t data[320];
     int len = uart_read_bytes(MODEM_UART_NUM, data, sizeof(data) - 1,
                               pdMS_TO_TICKS(800));
     if (len > 0)
     {
         data[len] = '\0';
         ESP_LOGI(TAG, "AT rsp: %s", (char *)data);
         feed_modem_bytes(data, (size_t)len);
     }
 }
 
 /* ================================================================
  * GNSS + signal quality
  * ================================================================ */
 
 static float s_lat;
 static float s_lng;
 static bool s_have_fix;
 static int s_last_csq = 99;
 
 static void parse_gps_response(const char *response)
 {
     const char *start = strstr(response, "+CGNSSINFO:");
     if (!start)
         return;
     start += strlen("+CGNSSINFO:");
     while (*start == ' ' || *start == '\t')
         start++;
     if (*start == ',' || *start == '\0' || *start == '\r' || *start == '\n')
         return;
 
     int status = 0, num_sats = 0;
     float latitude = 0.0f, longitude = 0.0f;
     char lat_dir = 0, lon_dir = 0;
 
     /* Extended A7670E format: 3,14,,02,01,LAT,N,LON,E,... */
     int n = sscanf(start, "%d,%d,,%*[^,],%*[^,],%f,%c,%f,%c",
                    &status, &num_sats, &latitude, &lat_dir, &longitude, &lon_dir);
     if (n != 6)
     {
         n = sscanf(start, "%d,%d,%*[^,],%*[^,],%*[^,],%f,%c,%f,%c",
                    &status, &num_sats, &latitude, &lat_dir, &longitude, &lon_dir);
     }
     if (n != 6 || status <= 0)
         return;
 
     if (lat_dir == 'S' || lat_dir == 's')
         latitude = -fabsf(latitude);
     if (lon_dir == 'W' || lon_dir == 'w')
         longitude = -fabsf(longitude);
 
     s_lat = latitude;
     s_lng = longitude;
     s_have_fix = true;
     ESP_LOGI(TAG, "GNSS fix: %.7f, %.7f | status=%d sats=%d",
              latitude, longitude, status, num_sats);
 }
 
 static void parse_csq_response(const char *response)
 {
     const char *p = strstr(response, "+CSQ:");
     if (!p)
         return;
     int rssi = 99, ber = 99;
     if (sscanf(p, "+CSQ: %d,%d", &rssi, &ber) >= 1)
         s_last_csq = rssi;
 }
 
 static void read_response_log_csq(void)
 {
     uint8_t data[320];
     int len = uart_read_bytes(MODEM_UART_NUM, data, sizeof(data) - 1,
                               pdMS_TO_TICKS(800));
     if (len > 0)
     {
         data[len] = '\0';
         ESP_LOGI(TAG, "AT rsp: %s", (char *)data);
         feed_modem_bytes(data, (size_t)len);
         parse_csq_response((char *)data);
     }
 }
 
 static void send_at_csq_query(void)
 {
     uart_flush_rx();
     modem_send_line("AT+CSQ");
     read_response_log_csq();
 }
 
 static int csq_to_dbm_approx(int csq)
 {
     if (csq == 99 || csq < 0)
         return -100;
     if (csq > 31)
         csq = 31;
     return -113 + 2 * csq;
 }
 
 /* ================================================================
  * USSD — robust implementation for A7670E
  * ================================================================
  *
  * One UART capture pass waits for "+CUSD:" (OK may appear earlier/later in the
  * same buffer). Do not drain "OK" first with a fresh buffer — that discards +CUSD.
  *
  * AT+CUSD=2 often returns ERROR when no session; use modem_drain_discard_until.
  */
 static bool modem_run_ussd(const char *code, char *detail, size_t detail_len)
 {
     bool err = false;
 
     uart_flush_rx();
     modem_send_line("AT+CUSD=2");
     (void)modem_drain_discard_until("OK", 6000);
     uart_flush_rx();
     vTaskDelay(pdMS_TO_TICKS(200));
 
     modem_send_line("AT+CSCS=\"GSM\"");
     if (!modem_drain_until("OK", 8000, &err) || err) {
         snprintf(detail, detail_len, "AT+CSCS failed: %.200s", s_drain_buf);
         ESP_LOGE(TAG, "%s", detail);
         return false;
     }
     uart_flush_rx();
     vTaskDelay(pdMS_TO_TICKS(100));
 
     char cmd[88];
     snprintf(cmd, sizeof(cmd), "AT+CUSD=1,\"%s\"", code);
     modem_send_line(cmd);
 
     err = false;
     if (!modem_drain_until("+CUSD:", USSD_NETWORK_WAIT_MS, &err)) {
         if (err) {
             ESP_LOGW(TAG, "USSD: retry with DCS 15 after ERROR");
             uart_flush_rx();
             vTaskDelay(pdMS_TO_TICKS(250));
             snprintf(cmd, sizeof(cmd), "AT+CUSD=1,\"%s\",15", code);
             modem_send_line(cmd);
             err = false;
             if (!modem_drain_until("+CUSD:", USSD_NETWORK_WAIT_MS, &err)) {
                 snprintf(detail, detail_len,
                          "no +CUSD after retry. err=%d tail=%.200s", err ? 1 : 0,
                          s_drain_buf);
                 return false;
             }
         } else {
             snprintf(detail, detail_len,
                      "timeout no +CUSD (%d ms). LTE USSD may be unsupported. tail=%.150s",
                      USSD_NETWORK_WAIT_MS, s_drain_buf);
             ESP_LOGW(TAG, "%s", detail);
             return false;
         }
     }
 
     const char *p = strstr(s_drain_buf, "+CUSD:");
     if (p) {
         snprintf(detail, detail_len, "%s", p);
         char *nl = strchr(detail, '\n');
         if (nl)
             *nl = '\0';
         nl = strchr(detail, '\r');
         if (nl)
             *nl = '\0';
     } else {
         snprintf(detail, detail_len, "%.200s", s_drain_buf);
     }
 
     if (strstr(s_drain_buf, "+CUSD: 4") || strstr(s_drain_buf, "+CUSD: 5")) {
         ESP_LOGW(TAG, "USSD network error code: %s", detail);
         return false;
     }
 
     ESP_LOGI(TAG, "USSD ok: %s", detail);
     return true;
 }
 
 /* ================================================================
  * SMS — robust implementation for A7670E
  * ================================================================
  *
  * AT+CMGF=1         → TEXT mode
  * AT+CSCS="GSM"     → GSM 7-bit alphabet (safe for ASCII / Latin messages)
  *                     Use "UCS2" if you need Unicode / Arabic / emoji.
  * AT+CPMS="SM","SM","SM" → prefer SIM storage (avoids "ME memory full" errors)
  * AT+CMGS="<to>"    → modem responds "> " prompt
  * <text> + Ctrl-Z   → terminate and send
  *
  * Success: modem sends "+CMGS: <ref>\r\nOK"
  * Failure: modem sends "+CMS ERROR: <code>" or "+CME ERROR: <code>"
  *
  * Returns true on success; detail[] holds +CMGS ref or error text.
  */
 static bool modem_run_sms(const char *to, const char *text,
                           char *detail, size_t detail_len)
 {
     bool err = false;
 
     uart_flush_rx();
 
     /* ---- Step 1: TEXT mode ---- */
     modem_send_line("AT+CMGF=1");
     if (!modem_drain_until("OK", 8000, &err) || err)
     {
         snprintf(detail, detail_len, "AT+CMGF=1 failed: %.150s", s_drain_buf);
         ESP_LOGE(TAG, "%s", detail);
         modem_recover_command_mode();
         return false;
     }
     uart_flush_rx();
     vTaskDelay(pdMS_TO_TICKS(100));
 
     /* ---- Step 2: Character set ---- */
     modem_send_line("AT+CSCS=\"GSM\"");
     err = false;
     if (!modem_drain_until("OK", 8000, &err) || err)
     {
         snprintf(detail, detail_len, "AT+CSCS=\"GSM\" failed: %.150s", s_drain_buf);
         ESP_LOGE(TAG, "%s", detail);
         modem_recover_command_mode();
         return false;
     }
     uart_flush_rx();
     vTaskDelay(pdMS_TO_TICKS(100));
 
     /* ---- Step 3: Storage — SIM preferred to avoid "ME memory full" ---- */
     modem_send_line("AT+CPMS=\"SM\",\"SM\",\"SM\"");
     err = false;
     if (!modem_drain_until("OK", 8000, &err))
     {
         /* Non-fatal — SIM may have limited space; log and continue */
         ESP_LOGW(TAG, "AT+CPMS SM failed (continuing): %.100s", s_drain_buf);
     }
     uart_flush_rx();
     vTaskDelay(pdMS_TO_TICKS(150));
 
     /* ---- Step 4: Start message entry ---- */
     char cmgs_hdr[64];
     snprintf(cmgs_hdr, sizeof(cmgs_hdr), "AT+CMGS=\"%s\"", to);
     modem_send_line(cmgs_hdr);
 
     /*
      * Wait for the "> " prompt.
      * The A7670E can take a couple of seconds to echo the prompt,
      * especially under high URC activity.
      */
     if (!modem_wait_prompt(">", 35000))
     {
         snprintf(detail, detail_len,
                  "No > prompt after AT+CMGS. Modem may be busy. tail=%.150s",
                  s_drain_buf);
         ESP_LOGE(TAG, "%s", detail);
         modem_recover_command_mode();
         return false;
     }
 
     /* ---- Step 5: Write message body + Ctrl-Z ---- */
     ESP_LOGI(TAG, "SMS: sending body (%d chars) to %s", (int)strlen(text), to);
     uart_write_bytes(MODEM_UART_NUM, text, strlen(text));
     uint8_t ctrl_z = 0x1A;
     uart_write_bytes(MODEM_UART_NUM, &ctrl_z, 1);
 
     /*
      * Wait for +CMGS: <ref> confirmation.
      * 120 s covers worst-case crowded networks.
      * Success string: "+CMGS: 3\r\n\r\nOK"
      */
     err = false;
     if (!modem_drain_until("+CMGS:", 120000, &err))
     {
         snprintf(detail, detail_len,
                  "SMS send timeout — no +CMGS within 120 s. tail=%.150s",
                  s_drain_buf);
         ESP_LOGE(TAG, "%s", detail);
         modem_recover_command_mode();
         return false;
     }
 
     /* ---- Step 6: Check for error markers ---- */
     bool send_ok = true;
 
     if (strstr(s_drain_buf, "+CMS ERROR") || strstr(s_drain_buf, "+CME ERROR") ||
         drain_buf_has_standalone_error_line(s_drain_buf) || err)
     {
         send_ok = false;
         ESP_LOGE(TAG, "SMS failed: %.200s", s_drain_buf);
     }
     else
     {
         ESP_LOGI(TAG, "SMS sent successfully to %s: %.100s", to, s_drain_buf);
     }
 
     snprintf(detail, detail_len, "%.300s", s_drain_buf);
 
     /* ---- Step 7: Restore modem command mode ---- */
     modem_recover_command_mode();
     vTaskDelay(pdMS_TO_TICKS(300));
 
     return send_ok;
 }
 
 /* ================================================================
  * Process queued modem job (called from main loop)
  * ================================================================ */
 
 /* Scratch buffers (static: avoids multi-KB stack usage) */
 static char s_modem_detail_scratch[640];
 static char s_modem_esc_scratch[896];
 static char s_modem_reply_body_scratch[1024];
 
 static bool s_mqtt_pipeline_ok;
 static int s_mqtt_pub_fail_streak;
 static int64_t s_next_telemetry_attempt_ms;
 
 static bool mqtt_publish_json_to_topic(const char *topic, const char *json_payload);
 
 static void publish_modem_reply(const char *request_id, const char *kind,
                                 bool ok, const char *detail_raw)
 {
     if (!s_mqtt_pipeline_ok)
     {
         ESP_LOGW(TAG, "Skip modem reply — MQTT pipeline down");
         return;
     }
 
     char rid_safe[48];
     snprintf(rid_safe, sizeof(rid_safe), "%s",
              request_id ? request_id : "unknown");
     sanitize_json_token_inplace(rid_safe);
 
     json_escape_str(detail_raw ? detail_raw : "",
                     s_modem_esc_scratch, sizeof(s_modem_esc_scratch));
 
     snprintf(s_modem_reply_body_scratch, sizeof(s_modem_reply_body_scratch),
              "{\"requestId\":\"%s\",\"kind\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}",
              rid_safe, kind, ok ? "true" : "false", s_modem_esc_scratch);
 
     ESP_LOGI(TAG, "Publishing reply: %s", s_modem_reply_body_scratch);
 
     if (!mqtt_publish_json_to_topic(MQTT_TOPIC_REPLIES, s_modem_reply_body_scratch))
     {
         ESP_LOGW(TAG, "Modem reply MQTT publish failed");
     }
 }
 
 static void process_pending_modem_command(void)
 {
     pending_modem_t job;
 
     taskENTER_CRITICAL(&s_pending_mux);
     if (s_pending.kind == PENDING_NONE)
     {
         taskEXIT_CRITICAL(&s_pending_mux);
         return;
     }
     job = s_pending;
     memset(&s_pending, 0, sizeof(s_pending));
     s_pending.kind = PENDING_NONE;
     taskEXIT_CRITICAL(&s_pending_mux);
 
     memset(s_modem_detail_scratch, 0, sizeof(s_modem_detail_scratch));
 
     bool ok = false;
     const char *kind = "unknown";
 
     if (job.kind == PENDING_USSD)
     {
         kind = "ussd";
         ESP_LOGI(TAG, "Executing USSD: %s", job.ussd_code);
         ok = modem_run_ussd(job.ussd_code,
                             s_modem_detail_scratch, sizeof(s_modem_detail_scratch));
         ESP_LOGI(TAG, "USSD result: ok=%d detail=%s", ok, s_modem_detail_scratch);
     }
     else if (job.kind == PENDING_SMS)
     {
         kind = "sms";
         ESP_LOGI(TAG, "Executing SMS: to=%s", job.sms_to);
         ok = modem_run_sms(job.sms_to, job.sms_text,
                            s_modem_detail_scratch, sizeof(s_modem_detail_scratch));
         ESP_LOGI(TAG, "SMS result: ok=%d detail=%.80s", ok, s_modem_detail_scratch);
     }
     else
     {
         return;
     }
 
     /* Always bring modem back to command mode before CMQTT operations */
     modem_recover_command_mode();
     vTaskDelay(pdMS_TO_TICKS(300));
 
     publish_modem_reply(job.request_id, kind, ok, s_modem_detail_scratch);
 }
 
 /* ================================================================
  * MQTT publish helper
  * ================================================================ */
 
 static bool mqtt_publish_json_to_topic(const char *topic, const char *json_payload)
 {
     uart_flush_rx();
 
     for (int attempt = 0; attempt < 4; attempt++)
     {
         if (attempt > 0)
         {
             ESP_LOGW(TAG, "MQTT publish retry %d/3 — recovering command mode first", attempt);
             modem_recover_command_mode();
             vTaskDelay(pdMS_TO_TICKS(650));
         }
 
         if (!modem_sync_at_ok())
         {
             ESP_LOGW(TAG, "AT sync failed before MQTT publish (attempt %d)", attempt);
         }
         vTaskDelay(pdMS_TO_TICKS(120));
 
         char hdr[80];
         snprintf(hdr, sizeof(hdr), "AT+CMQTTTOPIC=%d,%d",
                  MQTT_CLIENT_INDEX, (int)strlen(topic));
         if (!modem_interactive_body(hdr, topic))
             continue;
 
         snprintf(hdr, sizeof(hdr), "AT+CMQTTPAYLOAD=%d,%d",
                  MQTT_CLIENT_INDEX, (int)strlen(json_payload));
         if (!modem_interactive_body(hdr, json_payload))
             continue;
 
         char pub[64];
         snprintf(pub, sizeof(pub), "AT+CMQTTPUB=%d,%d,%d",
                  MQTT_CLIENT_INDEX, MQTT_QOS_PUBLISH, MQTT_PUBLISH_TIMEOUT_SEC);
         modem_send_line(pub);
 
         bool err = false;
         if (!modem_drain_until("+CMQTTPUB:", 25000, &err))
         {
             ESP_LOGE(TAG, "CMQTTPUB timeout (attempt %d)", attempt);
             continue;
         }
         if (!strstr(s_drain_buf, "+CMQTTPUB: 0,0"))
         {
             ESP_LOGW(TAG, "CMQTTPUB unexpected response: %.100s", s_drain_buf);
         }
         return !err;
     }
     return false;
 }
 
 /* ================================================================
  * Telemetry
  * ================================================================ */
 
 static void telemetry_publish_if_ready(void)
 {
     if (!s_mqtt_pipeline_ok)
     {
         ESP_LOGD(TAG, "Skip telemetry (MQTT down)");
         return;
     }
     if (!s_have_fix)
     {
         ESP_LOGD(TAG, "Skip telemetry (no fix)");
         return;
     }
 
     int64_t now_ms = esp_timer_get_time() / 1000;
     if (now_ms < s_next_telemetry_attempt_ms)
         return;
 
     int64_t uptime_sec = esp_timer_get_time() / 1000000;
     char body[320];
 
     if (s_last_csq >= 0 && s_last_csq <= 31)
     {
         snprintf(body, sizeof(body),
                  "{\"lat\":%.7f,\"lng\":%.7f,\"uptimeSec\":%lld,"
                  "\"csq\":%d,\"rssi\":%d}",
                  s_lat, s_lng, (long long)uptime_sec,
                  s_last_csq, csq_to_dbm_approx(s_last_csq));
     }
     else
     {
         snprintf(body, sizeof(body),
                  "{\"lat\":%.7f,\"lng\":%.7f,\"uptimeSec\":%lld,"
                  "\"csq\":%d,\"rssi\":null}",
                  s_lat, s_lng, (long long)uptime_sec, s_last_csq);
     }
 
     ESP_LOGI(TAG, "MQTT pub telemetry: %s", body);
 
     if (!mqtt_publish_json_to_topic(MQTT_TOPIC_TELEMETRY, body))
     {
         ESP_LOGE(TAG, "Telemetry publish failed");
         s_mqtt_pub_fail_streak++;
         s_next_telemetry_attempt_ms = now_ms + TELEMETRY_RETRY_AFTER_FAIL_MS;
         if (s_mqtt_pub_fail_streak >= 2)
         {
             s_mqtt_pipeline_ok = false;
             ESP_LOGW(TAG, "MQTT stuck after %d fails — scheduling pipeline reconnect",
                      s_mqtt_pub_fail_streak);
         }
         return;
     }
 
     s_mqtt_pub_fail_streak = 0;
     s_next_telemetry_attempt_ms = now_ms + TELEMETRY_MIN_OK_INTERVAL_MS;
     ESP_LOGI(TAG, "Telemetry OK → %s", MQTT_TOPIC_TELEMETRY);
 }
 
 /* ================================================================
  * PDP / packet data helpers
  * ================================================================ */
 
 static bool pdp_context1_read_ip(char *ip_out, size_t ip_len)
 {
     bool err = false;
     uart_flush_rx();
     modem_send_line("AT+CGPADDR=1");
     if (!modem_drain_until("OK", 8000, &err) || err)
         return false;
 
     const char *p = strstr(s_drain_buf, "+CGPADDR:");
     if (!p)
         return false;
 
     int cid = -1;
     char ip[48];
     if (sscanf(p, "+CGPADDR: %d,%47[^,\r\n]", &cid, ip) < 2)
         return false;
     if (strcmp(ip, "0.0.0.0") == 0 || strlen(ip) < 7)
         return false;
 
     if (ip_out && ip_len > 0)
     {
         strncpy(ip_out, ip, ip_len - 1);
         ip_out[ip_len - 1] = '\0';
     }
     ESP_LOGI(TAG, "PDP context 1 IP: %s", ip);
     return true;
 }
 
 static void wait_for_packet_data_ip(uint32_t max_wait_ms)
 {
     const uint32_t step_ms = 750;
     uint32_t waited = 0;
     while (waited < max_wait_ms)
     {
         char ip[48];
         if (pdp_context1_read_ip(ip, sizeof(ip)))
         {
             ESP_LOGI(TAG, "Packet data up — settling 3 s before MQTT...");
             vTaskDelay(pdMS_TO_TICKS(3000));
             return;
         }
         vTaskDelay(pdMS_TO_TICKS(step_ms));
         waited += step_ms;
     }
     ESP_LOGW(TAG, "No PDP IPv4 after %u ms — MQTT may fail until registration completes",
              (unsigned)max_wait_ms);
 }
 
 /**
  * Wait for +CMQTTSTART: URC. Only a standalone ERROR line ends early (stack may
  * already be running). Does not match "+CMS ERROR" / "+CGNSSINFO: ERROR".
  */
 static bool modem_drain_cmqttstart_wait(int timeout_ms, bool *standalone_err)
 {
     size_t total = 0;
     s_drain_buf[0] = '\0';
     *standalone_err = false;
     int64_t deadline_ms = esp_timer_get_time() / 1000 + timeout_ms;
 
     while (esp_timer_get_time() / 1000 < deadline_ms) {
         uint8_t chunk[256];
         int n = uart_read_bytes(MODEM_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(80));
         if (n <= 0)
             continue;
 
         feed_modem_bytes(chunk, (size_t)n);
 
         size_t copy = (size_t)n;
         if (total + copy >= sizeof(s_drain_buf))
             copy = sizeof(s_drain_buf) - 1 - total;
         if (copy > 0) {
             memcpy(s_drain_buf + total, chunk, copy);
             total += copy;
             s_drain_buf[total] = '\0';
         }
 
         if (strstr(s_drain_buf, "+CMQTTSTART:"))
             return true;
         if (drain_buf_has_standalone_error_line(s_drain_buf)) {
             *standalone_err = true;
             return false;
         }
     }
     s_drain_buf[total] = '\0';
     return strstr(s_drain_buf, "+CMQTTSTART:") != NULL;
 }
 
 static void modem_cmqtt_teardown_best_effort(void)
 {
     uart_flush_rx();
     modem_send_line("AT+CMQTTDISC=0,0");
     (void)modem_drain_discard_until("OK", 8000);
 
     uart_flush_rx();
     modem_send_line("AT+CMQTTREL=0");
     (void)modem_drain_discard_until("OK", 8000);
 
     uart_flush_rx();
     modem_send_line("AT+CMQTTSTOP");
     (void)modem_drain_discard_until("OK", 25000);
 
     vTaskDelay(pdMS_TO_TICKS(1500));
 }
 
 static bool modem_cmqtt_start_until_ok(void)
 {
     for (int attempt = 0; attempt < 8; attempt++) {
         if (attempt > 0) {
             ESP_LOGW(TAG, "CMQTTSTART retry %d/7", attempt);
             modem_cmqtt_teardown_best_effort();
             wait_for_packet_data_ip(45000);
         }
 
         uart_flush_rx();
         modem_send_line("AT+CMQTTSTART");
 
         bool standalone_err = false;
         bool found = modem_drain_cmqttstart_wait(90000, &standalone_err);
 
         if (found && strstr(s_drain_buf, "+CMQTTSTART: 0")) {
             ESP_LOGI(TAG, "CMQTTSTART OK (+CMQTTSTART: 0)");
             return true;
         }
 
         if (standalone_err) {
             ESP_LOGW(TAG,
                      "CMQTTSTART standalone ERROR — stack may already run; probe ACCQ");
             return true;
         }
 
         if (found) {
             ESP_LOGW(TAG, "CMQTTSTART non-zero (attempt %d): %.120s", attempt,
                      s_drain_buf);
         } else {
             ESP_LOGW(TAG, "CMQTTSTART timeout (attempt %d)", attempt);
         }
 
         vTaskDelay(pdMS_TO_TICKS(4000));
     }
     return false;
 }
 
 static bool mqtt_stack_start_and_connect(void)
 {
     bool err = false;
 
     if (CELLULAR_APN[0])
     {
         char apn_cmd[96];
         snprintf(apn_cmd, sizeof(apn_cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", CELLULAR_APN);
         uart_flush_rx();
         modem_send_line(apn_cmd);
         modem_drain_until("OK", 5000, &err);
     }
 
     modem_cmqtt_teardown_best_effort();
     wait_for_packet_data_ip(120000);
 
     if (!modem_cmqtt_start_until_ok())
     {
         ESP_LOGE(TAG, "CMQTTSTART never succeeded — check SIM/APN");
         return false;
     }
 
     char accq[80];
     snprintf(accq, sizeof(accq), "AT+CMQTTACCQ=%d,\"%s\"",
              MQTT_CLIENT_INDEX, MQTT_CLIENT_ID);
     uart_flush_rx();
     modem_send_line(accq);
     bool accq_ok = modem_drain_discard_until("OK", 10000);
     if (!accq_ok) {
         if (strstr(s_drain_buf, "+CME ERROR") != NULL ||
             strstr(s_drain_buf, "+CMS ERROR") != NULL ||
             drain_buf_has_standalone_error_line(s_drain_buf)) {
             ESP_LOGW(TAG,
                      "CMQTTACCQ ERROR — client slot may already be acquired; continuing");
         } else {
             ESP_LOGE(TAG, "CMQTTACCQ timeout");
             return false;
         }
     } else {
         ESP_LOGI(TAG, "CMQTTACCQ OK");
     }
 
     char conn[192];
     snprintf(conn, sizeof(conn),
              "AT+CMQTTCONNECT=%d,\"tcp://%s:%d\",%d,1",
              MQTT_CLIENT_INDEX, MQTT_BROKER_HOST, MQTT_BROKER_PORT, MQTT_KEEPALIVE_SEC);
     uart_flush_rx();
     modem_send_line(conn);
     if (!modem_drain_until("+CMQTTCONNECT:", 60000, &err))
     {
         ESP_LOGE(TAG, "CMQTTCONNECT timeout");
         return false;
     }
     if (!strstr(s_drain_buf, "+CMQTTCONNECT: 0,0"))
     {
         ESP_LOGE(TAG, "CMQTTCONNECT failed: %s", s_drain_buf);
         return false;
     }
 
     char subhdr[64];
     snprintf(subhdr, sizeof(subhdr), "AT+CMQTTSUB=%d,%d,%d",
              MQTT_CLIENT_INDEX, (int)strlen(MQTT_TOPIC_COMMANDS), MQTT_QOS_SUBSCRIBE);
     if (!modem_interactive_body(subhdr, MQTT_TOPIC_COMMANDS))
     {
         ESP_LOGE(TAG, "CMQTTSUB failed");
         return false;
     }
     modem_drain_until("+CMQTTSUB:", 10000, &err);
     if (!strstr(s_drain_buf, "+CMQTTSUB: 0,0"))
     {
         ESP_LOGW(TAG, "CMQTTSUB response: %.100s", s_drain_buf);
     }
 
     ESP_LOGI(TAG, "MQTT connected + subscribed to %s", MQTT_TOPIC_COMMANDS);
     return true;
 }
 
 /* ================================================================
  * Background UART poll
  * ================================================================ */
 
 static void poll_uart_background(uint32_t window_ms)
 {
     int64_t end = esp_timer_get_time() / 1000 + (int64_t)window_ms;
     while (esp_timer_get_time() / 1000 < end)
     {
         uint8_t b[256];
         int n = uart_read_bytes(MODEM_UART_NUM, b, sizeof(b), pdMS_TO_TICKS(40));
         if (n > 0)
             feed_modem_bytes(b, (size_t)n);
     }
 }
 
 /* ================================================================
  * app_main
  * ================================================================ */
 
 void app_main(void)
 {
     led_gpio_init();
     modem_uart_init();
     modem_power_on();
 
     vTaskDelay(pdMS_TO_TICKS(5000));
 
     uart_flush_rx();
     send_at_command("AT");
     read_response_log();
 
     send_at_csq_query();
 
     send_at_command("AT+CREG?");
     read_response_log();
 
     send_at_command("AT+CGATT?");
     read_response_log();
 
     send_at_command("AT+CGACT=1,1");
     read_response_log();
 
     send_at_command("AT+CGPADDR=1");
     read_response_log();
 
     s_mqtt_pipeline_ok = mqtt_stack_start_and_connect();
     if (!s_mqtt_pipeline_ok)
     {
         ESP_LOGE(TAG, "MQTT setup failed — telemetry will wait for reconnect");
     }
 
     send_at_command("AT+CGNSSPWR=1");
     read_response_log();
     ESP_LOGI(TAG, "Waiting for GNSS stack...");
     for (int i = 0; i < 50; i++)
         poll_uart_background(200);
 
     int mqtt_retry_loops = 0;
 
     while (1) {
         poll_uart_background(80);
 
         bool defer_bg_at = pending_modem_work_pending();
 
         process_pending_modem_command();
 
         /* Reconnect MQTT if pipeline died */
         uint32_t reconnect_threshold = (s_mqtt_pub_fail_streak >= 2) ? 4u : 18u;
         if (!s_mqtt_pipeline_ok)
         {
             if (++mqtt_retry_loops >= (int)reconnect_threshold)
             {
                 mqtt_retry_loops = 0;
                 ESP_LOGW(TAG, "Reconnecting MQTT (fail_streak=%d)...",
                          s_mqtt_pub_fail_streak);
                 if (mqtt_stack_start_and_connect())
                 {
                     s_mqtt_pipeline_ok = true;
                     s_mqtt_pub_fail_streak = 0;
                     s_next_telemetry_attempt_ms = 0;
                 }
             }
         }
         else {
             mqtt_retry_loops = 0;
         }
 
         if (!defer_bg_at) {
             send_at_csq_query();
 
             /* Publish telemetry before GNSS poll — CGNSSINFO bursts can wedge CMQTT */
             telemetry_publish_if_ready();
 
             send_at_command("AT+CGNSSINFO");
             uint8_t data[512];
             int len = uart_read_bytes(MODEM_UART_NUM, data, sizeof(data) - 1,
                                       pdMS_TO_TICKS(1500));
             if (len > 0) {
                 data[len] = '\0';
                 ESP_LOGI(TAG, "GNSS rsp: %s", (char *)data);
                 parse_gps_response((char *)data);
                 parse_csq_response((char *)data);
                 feed_modem_bytes(data, (size_t)len);
             }
         } else {
             /* USSD/SMS was queued: run it before competing AT+CSQ / GNSS this tick */
             poll_uart_background(120);
         }
 
         poll_uart_background(200);
         vTaskDelay(pdMS_TO_TICKS(GNSS_POLL_INTERVAL_MS));
     }
 }
 